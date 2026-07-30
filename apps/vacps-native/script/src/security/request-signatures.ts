/**
 * Agent → control-plane request signatures (parity with apps/vacps).
 * Uses vacps:crypto (OpenSSL) instead of Node webcrypto.
 */
import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';

export function createAgentSignatureHeaders(
  backendId: string,
  privateKeyEncoded: string,
  method: string,
  url: string,
  body: string,
): Record<string, string> {
  const timestamp = String(Math.floor(host.nowMs() / 1000));
  const nonce = crypto.base64UrlEncode(crypto.randomBytes(16));
  const path = pathnameOf(url);
  const bodyDigest = crypto.base64UrlEncode(crypto.sha256(body));
  const canonical = [
    'vacps-request-v1',
    'agent',
    method.toUpperCase(),
    path,
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

function pathnameOf(url: string): string {
  // Minimal URL path extract (QuickJS has no URL global).
  const scheme = url.indexOf('://');
  if (scheme < 0) throw new Error('invalid url for signature');
  const rest = url.slice(scheme + 3);
  const slash = rest.indexOf('/');
  if (slash < 0) return '/';
  const pathQuery = rest.slice(slash);
  const hash = pathQuery.indexOf('#');
  const noHash = hash >= 0 ? pathQuery.slice(0, hash) : pathQuery;
  const q = noHash.indexOf('?');
  return q >= 0 ? noHash.slice(0, q) : noHash;
}
