import type { Env } from '../env.js';
import { AppError } from '../lib/http.js';

const SESSION_COOKIE = 'vps_agent_control_session';
export const SESSION_MAX_AGE_SECONDS = 12 * 60 * 60;
const encoder = new TextEncoder();

interface AuthConfiguration {
  password: string;
  sessionSecret: string;
}

interface SessionPayload {
  version: 1;
  exp: number;
}

export function requireAuthConfiguration(env: Env): AuthConfiguration {
  const password = env.CONTROL_PANEL_PASSWORD;
  const sessionSecret = env.CONTROL_PANEL_SESSION_SECRET;
  if (
    !password ||
    password.length < 12 ||
    /\s/.test(password) ||
    !sessionSecret ||
    sessionSecret.length < 32 ||
    /\s/.test(sessionSecret)
  ) {
    throw new AppError(
      'control_panel_auth_unavailable',
      'Control panel authentication is not configured.',
      503,
    );
  }
  return { password, sessionSecret };
}

export async function passwordMatches(candidate: string, env: Env): Promise<boolean> {
  const { password, sessionSecret } = requireAuthConfiguration(env);
  const signature = await sign(`password:${password}`, sessionSecret);
  return crypto.subtle.verify(
    'HMAC',
    await hmacKey(sessionSecret, ['verify']),
    signature,
    encoder.encode(`password:${candidate}`),
  );
}

export async function createSessionCookie(env: Env, now = Date.now()): Promise<string> {
  const { sessionSecret } = requireAuthConfiguration(env);
  const payload: SessionPayload = {
    version: 1,
    exp: Math.floor(now / 1000) + SESSION_MAX_AGE_SECONDS,
  };
  const encodedPayload = base64UrlEncode(encoder.encode(JSON.stringify(payload)));
  const signature = base64UrlEncode(await sign(encodedPayload, sessionSecret));
  return `${SESSION_COOKIE}=${encodedPayload}.${signature}; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=${SESSION_MAX_AGE_SECONDS}`;
}

export function clearSessionCookie(): string {
  return `${SESSION_COOKIE}=; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=0`;
}

export async function hasValidSession(
  request: Request,
  env: Env,
  now = Date.now(),
): Promise<boolean> {
  const { sessionSecret } = requireAuthConfiguration(env);
  const token = readCookie(request.headers.get('cookie'), SESSION_COOKIE);
  if (!token) return false;

  const [encodedPayload, encodedSignature, ...rest] = token.split('.');
  if (!encodedPayload || !encodedSignature || rest.length > 0) return false;

  try {
    const valid = await crypto.subtle.verify(
      'HMAC',
      await hmacKey(sessionSecret, ['verify']),
      base64UrlDecode(encodedSignature),
      encoder.encode(encodedPayload),
    );
    if (!valid) return false;
    const payload = JSON.parse(
      new TextDecoder().decode(base64UrlDecode(encodedPayload)),
    ) as unknown;
    return isUnexpiredSessionPayload(payload, now);
  } catch {
    return false;
  }
}

export async function requireAuthenticated(request: Request, env: Env): Promise<void> {
  if (!(await hasValidSession(request, env))) {
    throw new AppError('authentication_required', 'Authentication is required.', 401);
  }
}

function isUnexpiredSessionPayload(payload: unknown, now: number): payload is SessionPayload {
  return Boolean(
    payload &&
    typeof payload === 'object' &&
    (payload as SessionPayload).version === 1 &&
    Number.isSafeInteger((payload as SessionPayload).exp) &&
    (payload as SessionPayload).exp > Math.floor(now / 1000),
  );
}

async function hmacKey(secret: string, usages: KeyUsage[]): Promise<CryptoKey> {
  return crypto.subtle.importKey(
    'raw',
    encoder.encode(secret),
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    usages,
  );
}

async function sign(value: string, secret: string): Promise<ArrayBuffer> {
  return crypto.subtle.sign('HMAC', await hmacKey(secret, ['sign']), encoder.encode(value));
}

function readCookie(header: string | null, name: string): string | undefined {
  if (!header) return undefined;
  for (const part of header.split(';')) {
    const [key, ...value] = part.trim().split('=');
    if (key === name) return value.join('=');
  }
  return undefined;
}

function base64UrlEncode(value: Uint8Array | ArrayBuffer): string {
  const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) : value;
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '');
}

function base64UrlDecode(value: string): ArrayBuffer {
  const normalized = value.replaceAll('-', '+').replaceAll('_', '/');
  const padded = normalized.padEnd(normalized.length + ((4 - (normalized.length % 4)) % 4), '=');
  const binary = atob(padded);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes.buffer;
}
