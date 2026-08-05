/**
 * Verify control-plane → agent request signatures (issuer = control).
 * Canonical version: vacps-request-v2. Requires x-vps-control-backend-id audience.
 */
import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';

import { requestTargetFromParts } from './request-target';

const MAX_CLOCK_SKEW_SECONDS = 5 * 60;

export function verifyControlPlaneRequest(input: {
  publicKeyB64: string;
  /** Receiving application's configured BACKEND_ID (audience). */
  expectedBackendId: string;
  method: string;
  /** Path without query (normalized). */
  path: string;
  /** Query without leading `?`; empty/undefined when absent. */
  query?: string;
  headers: Readonly<Record<string, string>>;
  body: string;
}): { nonce: string; backendId: string } {
  const backendId = requiredHeader(input.headers, 'x-vps-control-backend-id');
  if (backendId !== input.expectedBackendId) {
    throw new Error('Control-plane signature targets a different backend.');
  }

  const timestamp = requiredHeader(input.headers, 'x-vps-control-timestamp');
  const nonce = requiredHeader(input.headers, 'x-vps-control-nonce');
  const signature = requiredHeader(input.headers, 'x-vps-control-signature');

  const ts = Number(timestamp);
  if (
    !Number.isSafeInteger(ts) ||
    Math.abs(Math.floor(host.nowMs() / 1000) - ts) > MAX_CLOCK_SKEW_SECONDS
  ) {
    throw new Error('Control-plane signature timestamp is invalid or expired.');
  }
  if (!/^[A-Za-z0-9_-]{16,128}$/.test(nonce)) {
    throw new Error('Control-plane nonce is invalid.');
  }
  if (!/^[A-Za-z0-9_-]{86}$/.test(signature)) {
    throw new Error('Control-plane signature is invalid.');
  }

  const target = requestTargetFromParts(input.path, input.query);
  const bodyDigest = crypto.base64UrlEncode(crypto.sha256(input.body));
  const canonical = [
    'vacps-request-v2',
    'control',
    input.method.toUpperCase(),
    target,
    backendId,
    timestamp,
    nonce,
    bodyDigest,
  ].join('\n');

  const pub = crypto.base64UrlDecode(input.publicKeyB64);
  const sig = crypto.base64UrlDecode(signature);
  if (!crypto.ed25519Verify(pub, canonical, sig)) {
    throw new Error('Control-plane signature is invalid.');
  }
  return { nonce, backendId };
}

function requiredHeader(headers: Readonly<Record<string, string>>, name: string): string {
  const direct = headers[name];
  if (direct) return direct.trim();
  const lower = name.toLowerCase();
  for (const [k, v] of Object.entries(headers)) {
    if (k.toLowerCase() === lower && v) return v.trim();
  }
  throw new Error(`Missing ${name} header.`);
}
