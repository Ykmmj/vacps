/**
 * Pure-logic checks for signature helpers (loaded in QuickJS Host).
 * Does not hit network.
 */
import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';

// Inline minimal base64url + seed extract (mirror security/base64url.ts)
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function b64urlEncode(bytes) {
  let out = '';
  let i = 0;
  for (; i + 2 < bytes.length; i += 3) {
    const n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
  }
  if (i < bytes.length) {
    const a = bytes[i];
    if (i + 1 === bytes.length) {
      const n = a << 16;
      out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + '==';
    } else {
      const n = (a << 16) | (bytes[i + 1] << 8);
      out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + '=';
    }
  }
  return out.replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '');
}
function b64urlDecode(value) {
  const padded =
    value.replaceAll('-', '+').replaceAll('_', '/') + '='.repeat((4 - (value.length % 4)) % 4);
  const lookup = new Uint8Array(256);
  for (let i = 0; i < B64.length; i++) lookup[B64.charCodeAt(i)] = i;
  const out = new Uint8Array((padded.length * 3) / 4);
  let o = 0;
  for (let i = 0; i < padded.length; i += 4) {
    const n =
      (lookup[padded.charCodeAt(i)] << 18) |
      (lookup[padded.charCodeAt(i + 1)] << 12) |
      (lookup[padded.charCodeAt(i + 2)] << 6) |
      lookup[padded.charCodeAt(i + 3)];
    out[o++] = (n >> 16) & 255;
    if (padded[i + 2] !== '=') out[o++] = (n >> 8) & 255;
    if (padded[i + 3] !== '=') out[o++] = n & 255;
  }
  return out.subarray(0, o);
}

// Roundtrip random
const raw = new Uint8Array(crypto.randomBytes(32));
const enc = b64urlEncode(raw);
const dec = b64urlDecode(enc);
if (dec.length !== 32) throw new Error('decode length');
for (let i = 0; i < 32; i++) {
  if (dec[i] !== raw[i]) throw new Error('roundtrip mismatch');
}

// Sign canonical string like control-plane
const seed = crypto.randomBytes(32);
const pub = crypto.ed25519PublicFromPrivate(seed);
const body = JSON.stringify({ backendId: 'test-node', name: 't' });
const digest = b64urlEncode(new Uint8Array(crypto.sha256(body)));
const ts = String(Math.floor(host.nowMs() / 1000));
const nonce = b64urlEncode(new Uint8Array(crypto.randomBytes(16)));
const canonical = [
  'vacps-request-v1',
  'agent',
  'POST',
  '/api/registrations',
  'test-node',
  ts,
  nonce,
  digest,
].join('\n');
const sig = crypto.ed25519Sign(seed, canonical);
if (new Uint8Array(sig).byteLength !== 64) throw new Error('sig len');
if (!crypto.ed25519Verify(pub, canonical, sig)) throw new Error('verify failed');
if (crypto.ed25519Verify(pub, canonical + 'x', sig)) throw new Error('tamper should fail');

// getenv exists
const path = host.getenv('PATH');
if (path !== null && typeof path !== 'string') throw new Error('getenv type');

export default { ok: true };
