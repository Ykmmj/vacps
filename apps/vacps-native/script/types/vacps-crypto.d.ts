/**
 * vacps:crypto — stateless OpenSSL helpers.
 *
 * BinaryInput: string | ArrayBuffer | Uint8Array for message/key inputs.
 * Byte-returning APIs yield ArrayBuffer (binding uses JS_NewArrayBufferCopy).
 */
declare module 'vacps:crypto' {
  export type BinaryInput = string | ArrayBuffer | Uint8Array;

  export function randomBytes(n: number): ArrayBuffer;
  export function sha256(data: BinaryInput): ArrayBuffer;
  export function sha256Hex(data: BinaryInput): string;
  export function toHex(bytes: ArrayBuffer | Uint8Array): string;
  export function fromHex(hex: string): ArrayBuffer;

  /** Standard Base64 (OpenSSL EVP_EncodeBlock), with `=` padding. */
  export function base64Encode(data: BinaryInput): string;
  export function base64Decode(s: string): ArrayBuffer;

  /** Base64url (RFC 4648 §5): `-_`, no padding on encode. */
  export function base64UrlEncode(data: BinaryInput): string;
  export function base64UrlDecode(s: string): ArrayBuffer;

  /**
   * Parse AGENT_PRIVATE_KEY (base64url): raw 32-byte seed or PKCS#8/DER Ed25519.
   * Returns 32-byte seed for ed25519Sign / ed25519PublicFromPrivate.
   */
  export function ed25519SeedFromPrivateKey(encoded: string): ArrayBuffer;

  /** Derive 32-byte public key from 32-byte Ed25519 private seed. */
  export function ed25519PublicFromPrivate(privateKey: ArrayBuffer | Uint8Array): ArrayBuffer;
  export function ed25519Sign(
    privateKey: ArrayBuffer | Uint8Array,
    message: BinaryInput,
  ): ArrayBuffer;
  export function ed25519Verify(
    publicKey: ArrayBuffer | Uint8Array,
    message: BinaryInput,
    signature: ArrayBuffer | Uint8Array,
  ): boolean;
}
