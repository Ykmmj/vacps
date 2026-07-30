/**
 * Thin wrappers over vacps:crypto OpenSSL Base64 / Base64url.
 * Prefer importing `vacps:crypto` directly in new code.
 */
import * as crypto from 'vacps:crypto';

export function base64UrlEncode(bytes: Uint8Array | ArrayBuffer | string): string {
  return crypto.base64UrlEncode(bytes);
}

export function base64UrlDecode(value: string): Uint8Array {
  return new Uint8Array(crypto.base64UrlDecode(value));
}

export function base64Encode(bytes: Uint8Array | ArrayBuffer | string): string {
  return crypto.base64Encode(bytes);
}

export function base64Decode(value: string): Uint8Array {
  return new Uint8Array(crypto.base64Decode(value));
}
