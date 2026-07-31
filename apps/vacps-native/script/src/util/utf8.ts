/** Longest valid UTF-8 prefix length ≤ limit (product files.read max_bytes). */
export function utf8PrefixEnd(bytes: Uint8Array, limit: number): number {
  let end = Math.min(limit, bytes.length);
  if (end === bytes.length || end === 0) return end;

  let lead = end - 1;
  // Find lead byte of the code unit that straddles the cut.
  while (lead > 0 && (bytes[lead]! & 0xc0) === 0x80) {
    lead--;
  }

  const first = bytes[lead]!;
  let width: number;
  if ((first & 0x80) === 0) width = 1;
  else if ((first & 0xe0) === 0xc0) width = 2;
  else if ((first & 0xf0) === 0xe0) width = 3;
  else if ((first & 0xf8) === 0xf0) width = 4;
  else return lead; // invalid lead — exclude it from the prefix

  return lead + width <= end ? end : lead;
}

/** Decode UTF-8 with U+FFFD replacement for illegal sequences (non-fatal). */
export function utf8Decode(bytes: Uint8Array): string {
  let out = '';
  let i = 0;
  while (i < bytes.length) {
    const b0 = bytes[i]!;
    if (b0 < 0x80) {
      out += String.fromCharCode(b0);
      i += 1;
      continue;
    }
    let need = 0;
    let cp = 0;
    if ((b0 & 0xe0) === 0xc0) {
      need = 1;
      cp = b0 & 0x1f;
    } else if ((b0 & 0xf0) === 0xe0) {
      need = 2;
      cp = b0 & 0x0f;
    } else if ((b0 & 0xf8) === 0xf0) {
      need = 3;
      cp = b0 & 0x07;
    } else {
      out += '\uFFFD';
      i += 1;
      continue;
    }
    if (i + need >= bytes.length) {
      out += '\uFFFD';
      break;
    }
    let ok = true;
    for (let j = 1; j <= need; j++) {
      const bj = bytes[i + j]!;
      if ((bj & 0xc0) !== 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (bj & 0x3f);
    }
    if (!ok) {
      out += '\uFFFD';
      i += 1;
      continue;
    }
    i += need + 1;
    if (cp <= 0xffff) out += String.fromCharCode(cp);
    else {
      const v = cp - 0x10000;
      out += String.fromCharCode(0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff));
    }
  }
  return out;
}

export function utf8ByteLengthOfString(s: string): number {
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      n += 4;
      i += 1;
    } else if (c < 0x80) n += 1;
    else if (c < 0x800) n += 2;
    else n += 3;
  }
  return n;
}

export function truncateStringToUtf8Bytes(s: string, maxBytes: number): string {
  let n = 0;
  let i = 0;
  while (i < s.length) {
    const c = s.charCodeAt(i);
    let step = 1;
    let utf8 = 1;
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      step = 2;
      utf8 = 4;
    } else if (c < 0x80) utf8 = 1;
    else if (c < 0x800) utf8 = 2;
    else utf8 = 3;
    if (n + utf8 > maxBytes) break;
    n += utf8;
    i += step;
  }
  return s.slice(0, i);
}
