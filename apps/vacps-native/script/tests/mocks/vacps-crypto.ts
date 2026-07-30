/**
 * Node stand-in for vacps:crypto in vitest (OpenSSL path tested via C++ gtest).
 */
import { createHash, randomBytes as nodeRandom } from "node:crypto";

function toBuffer(data: string | ArrayBuffer | Uint8Array): Buffer {
  if (typeof data === "string") return Buffer.from(data, "utf8");
  if (data instanceof ArrayBuffer) return Buffer.from(data);
  return Buffer.from(data.buffer, data.byteOffset, data.byteLength);
}

export function randomBytes(n: number): ArrayBuffer {
  const b = nodeRandom(n);
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
}

export function sha256(data: string | ArrayBuffer | Uint8Array): ArrayBuffer {
  const dig = createHash("sha256").update(toBuffer(data)).digest();
  return dig.buffer.slice(dig.byteOffset, dig.byteOffset + dig.byteLength);
}

export function sha256Hex(data: string | ArrayBuffer | Uint8Array): string {
  return createHash("sha256").update(toBuffer(data)).digest("hex");
}

export function toHex(bytes: ArrayBuffer | Uint8Array): string {
  return Buffer.from(toBuffer(bytes)).toString("hex");
}

export function fromHex(hex: string): ArrayBuffer {
  const b = Buffer.from(hex, "hex");
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
}

export function base64Encode(data: string | ArrayBuffer | Uint8Array): string {
  return toBuffer(data).toString("base64");
}

export function base64Decode(s: string): ArrayBuffer {
  const b = Buffer.from(s, "base64");
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
}

export function base64UrlEncode(data: string | ArrayBuffer | Uint8Array): string {
  return toBuffer(data)
    .toString("base64url")
    .replace(/=+$/, "");
}

export function base64UrlDecode(s: string): ArrayBuffer {
  const b = Buffer.from(s, "base64url");
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
}

export function ed25519SeedFromPrivateKey(encoded: string): ArrayBuffer {
  return base64UrlDecode(encoded);
}

export function ed25519PublicFromPrivate(_privateKey: ArrayBuffer | Uint8Array): ArrayBuffer {
  return randomBytes(32);
}

export function ed25519Sign(
  _privateKey: ArrayBuffer | Uint8Array,
  _message: string | ArrayBuffer | Uint8Array,
): ArrayBuffer {
  return randomBytes(64);
}

export function ed25519Verify(
  _publicKey: ArrayBuffer | Uint8Array,
  _message: string | ArrayBuffer | Uint8Array,
  _signature: ArrayBuffer | Uint8Array,
): boolean {
  return true;
}
