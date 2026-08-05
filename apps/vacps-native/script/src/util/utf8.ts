/**
 * UTF-8 helpers for product code.
 *
 * Codec path: WHATWG TextEncoder / TextDecoder (installed via simdutf).
 * Do not re-implement UTF-8 here.
 */

function encoder(): TextEncoder {
  if (typeof TextEncoder === 'undefined') {
    throw new Error('TextEncoder is not available (native Encoding API missing)');
  }
  return new TextEncoder();
}

function decoder(fatal = false): TextDecoder {
  if (typeof TextDecoder === 'undefined') {
    throw new Error('TextDecoder is not available (native Encoding API missing)');
  }
  return new TextDecoder('utf-8', { fatal });
}

/** Encode a JS string to UTF-8 bytes. */
export function utf8Encode(s: string): Uint8Array {
  return encoder().encode(s);
}

/** Decode UTF-8 bytes to a JS string (replacement for invalid sequences). */
export function utf8Decode(bytes: Uint8Array): string {
  return decoder(false).decode(bytes);
}

export function utf8ByteLengthOfString(s: string): number {
  return encoder().encode(s).byteLength;
}

/**
 * Longest valid UTF-8 prefix length ≤ limit (do not split a multi-byte sequence).
 * Operates on already-encoded bytes; not a full codec.
 */
export function utf8PrefixEnd(bytes: Uint8Array, limit: number): number {
  let end = Math.min(limit, bytes.length);
  if (end === bytes.length || end === 0) return end;

  let lead = end - 1;
  while (lead > 0 && (bytes[lead]! & 0xc0) === 0x80) {
    lead--;
  }

  const first = bytes[lead]!;
  let width: number;
  if ((first & 0x80) === 0) width = 1;
  else if ((first & 0xe0) === 0xc0) width = 2;
  else if ((first & 0xf0) === 0xe0) width = 3;
  else if ((first & 0xf8) === 0xf0) width = 4;
  else return lead;

  return lead + width <= end ? end : lead;
}

/**
 * Slice a JS string by UTF-8 byte offsets [start, end).
 * Uses TextEncoder/TextDecoder; cuts never split multi-byte sequences.
 */
export function utf8ByteSlice(
  s: string,
  start: number,
  end: number,
): {
  content: string;
  totalBytes: number;
  start: number;
  end: number;
} {
  const bytes = encoder().encode(s);
  const totalBytes = bytes.byteLength;
  let a = Math.max(0, Math.min(start | 0, totalBytes));
  let b = Math.max(a, Math.min(end | 0, totalBytes));
  while (a > 0 && a < totalBytes && (bytes[a]! & 0xc0) === 0x80) a += 1;
  while (b > 0 && b < totalBytes && (bytes[b]! & 0xc0) === 0x80) b -= 1;
  return {
    content: decoder(false).decode(bytes.subarray(a, b)),
    totalBytes,
    start: a,
    end: b,
  };
}

export function truncateStringToUtf8Bytes(s: string, maxBytes: number): string {
  const bytes = encoder().encode(s);
  if (bytes.byteLength <= maxBytes) return s;
  const end = utf8PrefixEnd(bytes, maxBytes);
  return decoder(false).decode(bytes.subarray(0, end));
}
