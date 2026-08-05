/**
 * Agent → control-plane request signatures (parity with apps/vacps).
 * Uses vacps:crypto (OpenSSL) instead of Node webcrypto.
 * Canonical version: vacps-request-v2.
 */
import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';

import { requestTargetOf } from './request-target';

export { requestTargetFromParts, requestTargetOf } from './request-target';

export function createAgentSignatureHeaders(
  backendId: string,
  privateKeyEncoded: string,
  method: string,
  url: string,
  body: string,
): Record<string, string> {
  const timestamp = String(Math.floor(host.nowMs() / 1000));
  const nonce = crypto.base64UrlEncode(crypto.randomBytes(16));
  const target = requestTargetOf(url);
  const bodyDigest = crypto.base64UrlEncode(crypto.sha256(body));
  const canonical = [
    'vacps-request-v2',
    'agent',
    method.toUpperCase(),
    target,
    backendId,
    timestamp,
    nonce,
    bodyDigest,
  ].join('\n');
  const seed = crypto.ed25519SeedFromPrivateKey(privateKeyEncoded);
  const signature = crypto.ed25519Sign(seed, canonical);
  return {
    'x-vacps-id': backendId,
    'x-vacps-timestamp': timestamp,
    'x-vacps-nonce': nonce,
    'x-vacps-signature': crypto.base64UrlEncode(signature),
  };
}
