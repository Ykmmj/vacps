import { createHash, randomBytes, webcrypto } from 'node:crypto';

import type { AgentConfig } from '../config.js';

const MAX_CLOCK_SKEW_SECONDS = 5 * 60;
const signingKeys = new Map<string, Promise<CryptoKey>>();
const verificationKeys = new Map<string, Promise<CryptoKey>>();

export async function createAgentSignatureHeaders(
  config: Pick<AgentConfig, 'BACKEND_ID' | 'AGENT_PRIVATE_KEY'>,
  request: Pick<Request, 'method' | 'url'>,
  body: string,
): Promise<Record<string, string>> {
  const timestamp = String(Math.floor(Date.now() / 1000));
  const nonce = randomBytes(16).toString('base64url');
  const signature = await webcrypto.subtle.sign(
    'Ed25519',
    await signingKey(config.AGENT_PRIVATE_KEY),
    Buffer.from(canonicalRequest('agent', request, config.BACKEND_ID, timestamp, nonce, body)),
  );
  return {
    'x-vacps-id': config.BACKEND_ID,
    'x-vacps-timestamp': timestamp,
    'x-vacps-nonce': nonce,
    'x-vacps-signature': Buffer.from(signature).toString('base64url'),
  };
}

export async function verifyControlPlaneRequest(
  config: Pick<AgentConfig, 'CONTROL_PLANE_PUBLIC_KEY' | 'BACKEND_ID'>,
  input: {
    method: string;
    url: string;
    headers: Record<string, string | string[] | undefined>;
    body: string;
  },
): Promise<{ nonce: string; backendId: string }> {
  const backendId = requiredHeader(input.headers, 'x-vps-control-backend-id');
  if (backendId !== config.BACKEND_ID) {
    throw new Error('Control-plane signature targets a different backend.');
  }
  const timestamp = requiredTimestamp(input.headers, 'x-vps-control-timestamp');
  const nonce = requiredNonce(input.headers, 'x-vps-control-nonce');
  const signature = requiredBase64Url(input.headers, 'x-vps-control-signature', 86);
  const request = new Request(new URL(input.url, 'http://agent.local'), { method: input.method });
  const verified = await webcrypto.subtle.verify(
    'Ed25519',
    await verificationKey(config.CONTROL_PLANE_PUBLIC_KEY),
    Buffer.from(signature, 'base64url'),
    Buffer.from(canonicalRequest('control', request, backendId, timestamp, nonce, input.body)),
  );
  if (!verified) throw new Error('Control-plane signature is invalid.');
  return { nonce, backendId };
}

function signingKey(value: string): Promise<CryptoKey> {
  let key = signingKeys.get(value);
  if (!key) {
    key = webcrypto.subtle.importKey(
      'pkcs8',
      Buffer.from(value, 'base64url'),
      { name: 'Ed25519' },
      false,
      ['sign'],
    );
    signingKeys.set(value, key);
  }
  return key;
}

function verificationKey(value: string): Promise<CryptoKey> {
  let key = verificationKeys.get(value);
  if (!key) {
    key = webcrypto.subtle.importKey(
      'raw',
      Buffer.from(value, 'base64url'),
      { name: 'Ed25519' },
      false,
      ['verify'],
    );
    verificationKeys.set(value, key);
  }
  return key;
}

/** pathname + search (search includes leading `?` when non-empty); fragments excluded. */
export function requestTargetOf(url: string): string {
  const parsed = new URL(url, 'http://vacps.invalid');
  return `${parsed.pathname}${parsed.search}`;
}

function canonicalRequest(
  issuer: 'agent' | 'control',
  request: Pick<Request, 'method' | 'url'>,
  backendId: string,
  timestamp: string,
  nonce: string,
  body: string,
): string {
  return [
    'vacps-request-v2',
    issuer,
    request.method.toUpperCase(),
    requestTargetOf(request.url),
    backendId,
    timestamp,
    nonce,
    createHash('sha256').update(body).digest('base64url'),
  ].join('\n');
}

function requiredTimestamp(
  headers: Record<string, string | string[] | undefined>,
  name: string,
): string {
  const value = requiredHeader(headers, name);
  const parsed = Number(value);
  if (
    !Number.isSafeInteger(parsed) ||
    Math.abs(Math.floor(Date.now() / 1000) - parsed) > MAX_CLOCK_SKEW_SECONDS
  )
    throw new Error('Control-plane signature timestamp is invalid or expired.');
  return value;
}

function requiredNonce(
  headers: Record<string, string | string[] | undefined>,
  name: string,
): string {
  const value = requiredHeader(headers, name);
  if (!/^[A-Za-z0-9_-]{16,128}$/.test(value)) throw new Error('Control-plane nonce is invalid.');
  return value;
}

function requiredBase64Url(
  headers: Record<string, string | string[] | undefined>,
  name: string,
  length: number,
): string {
  const value = requiredHeader(headers, name);
  if (!new RegExp(`^[A-Za-z0-9_-]{${length}}$`).test(value))
    throw new Error('Control-plane signature is invalid.');
  return value;
}

function requiredHeader(
  headers: Record<string, string | string[] | undefined>,
  name: string,
): string {
  const value = headers[name];
  const normalized = Array.isArray(value) ? value[0] : value;
  if (!normalized?.trim()) throw new Error(`Missing ${name} header.`);
  return normalized.trim();
}
