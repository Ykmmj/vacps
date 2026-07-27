import { AppError } from '../lib/http.js';

const encoder = new TextEncoder();
const MAX_CLOCK_SKEW_SECONDS = 5 * 60;

export interface SignedRequestIdentity {
  backendId: string;
  nonce: string;
}

/**
 * Verifies the Agent-to-control-plane request signature. The signed representation deliberately
 * includes method, path, identity, timestamp, nonce and an exact-body digest, so a signature
 * cannot be moved to another endpoint or paired with a modified JSON body.
 */
export async function verifyAgentRequestSignature(
  request: Request,
  publicKey: string,
  body?: string,
): Promise<SignedRequestIdentity> {
  const backendId = requiredHeader(request, 'x-vps-agent-id');
  const timestamp = requiredTimestamp(request, 'x-vps-agent-timestamp');
  const nonce = requiredNonce(request, 'x-vps-agent-nonce');
  const signature = requiredBase64Url(request, 'x-vps-agent-signature', 86);
  const signedBody = body ?? (await request.clone().text());
  const key = await crypto.subtle.importKey(
    'raw',
    toArrayBuffer(base64UrlDecode(publicKey)),
    { name: 'Ed25519' },
    false,
    ['verify'],
  );
  const verified = await crypto.subtle.verify(
    'Ed25519',
    key,
    toArrayBuffer(base64UrlDecode(signature)),
    toArrayBuffer(
      encoder.encode(
        await canonicalRequest('agent', request, backendId, timestamp, nonce, signedBody),
      ),
    ),
  );
  if (!verified) throw new AppError('invalid_agent_signature', 'Agent signature is invalid.', 401);
  return { backendId, nonce };
}

/** Sign a control-plane request so the Agent can authenticate every health, task and scheduler call. */
export async function createControlPlaneSignatureHeaders(
  privateKey: string | undefined,
  request: Pick<Request, 'method' | 'url'>,
  body: string,
): Promise<Record<string, string>> {
  if (!privateKey)
    throw new AppError(
      'control_plane_identity_unconfigured',
      'Control-plane signing key is not configured.',
      503,
    );
  const timestamp = String(Math.floor(Date.now() / 1000));
  const nonce = base64UrlEncode(crypto.getRandomValues(new Uint8Array(16)));
  const key = await crypto.subtle.importKey(
    'pkcs8',
    toArrayBuffer(base64UrlDecode(privateKey)),
    { name: 'Ed25519' },
    false,
    ['sign'],
  );
  const signature = await crypto.subtle.sign(
    'Ed25519',
    key,
    toArrayBuffer(
      encoder.encode(await canonicalRequest('control', request, undefined, timestamp, nonce, body)),
    ),
  );
  return {
    'x-vps-control-timestamp': timestamp,
    'x-vps-control-nonce': nonce,
    'x-vps-control-signature': base64UrlEncode(new Uint8Array(signature)),
  };
}

export async function sha256Base64Url(value: string): Promise<string> {
  return base64UrlEncode(
    new Uint8Array(await crypto.subtle.digest('SHA-256', encoder.encode(value))),
  );
}

export function base64UrlEncode(bytes: Uint8Array): string {
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '');
}

export function base64UrlDecode(value: string): Uint8Array {
  if (!/^[A-Za-z0-9_-]+$/.test(value))
    throw new AppError('invalid_signature_encoding', 'Signature encoding is invalid.', 401);
  const padded =
    value.replaceAll('-', '+').replaceAll('_', '/') + '='.repeat((4 - (value.length % 4)) % 4);
  try {
    const binary = atob(padded);
    return Uint8Array.from(binary, (character) => character.charCodeAt(0));
  } catch {
    throw new AppError('invalid_signature_encoding', 'Signature encoding is invalid.', 401);
  }
}

function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

async function canonicalRequest(
  issuer: 'agent' | 'control',
  request: Pick<Request, 'method' | 'url'>,
  backendId: string | undefined,
  timestamp: string,
  nonce: string,
  body: string,
): Promise<string> {
  const path = new URL(request.url).pathname;
  return [
    'vps-agent-request-v1',
    issuer,
    request.method.toUpperCase(),
    path,
    backendId ?? '',
    timestamp,
    nonce,
    await sha256Base64Url(body),
  ].join('\n');
}

function requiredHeader(request: Request, name: string): string {
  const value = request.headers.get(name)?.trim();
  if (!value) throw new AppError('missing_agent_signature', `Missing ${name} header.`, 401);
  return value;
}

function requiredTimestamp(request: Request, name: string): string {
  const value = requiredHeader(request, name);
  const parsed = Number(value);
  if (
    !Number.isSafeInteger(parsed) ||
    Math.abs(Math.floor(Date.now() / 1000) - parsed) > MAX_CLOCK_SKEW_SECONDS
  )
    throw new AppError(
      'expired_agent_signature',
      'Agent signature timestamp is invalid or expired.',
      401,
    );
  return value;
}

function requiredNonce(request: Request, name: string): string {
  const value = requiredHeader(request, name);
  if (!/^[A-Za-z0-9_-]{16,128}$/.test(value))
    throw new AppError('invalid_agent_nonce', 'Agent signature nonce is invalid.', 401);
  return value;
}

function requiredBase64Url(request: Request, name: string, length: number): string {
  const value = requiredHeader(request, name);
  if (!new RegExp(`^[A-Za-z0-9_-]{${length}}$`).test(value))
    throw new AppError('invalid_agent_signature', 'Agent signature is invalid.', 401);
  return value;
}
