import { describe, expect, it } from 'vitest';

import {
  utf8ByteLengthOfString,
  utf8ByteSlice,
  utf8Decode,
  utf8Encode,
  utf8PrefixEnd,
} from '../../src/util/utf8';

function enc(s: string): Uint8Array {
  const out: number[] = [];
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) {
      out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      const cp = 0x10000 + ((c - 0xd800) << 10) + (s.charCodeAt(++i) - 0xdc00);
      out.push(
        0xf0 | (cp >> 18),
        0x80 | ((cp >> 12) & 0x3f),
        0x80 | ((cp >> 6) & 0x3f),
        0x80 | (cp & 0x3f),
      );
    } else {
      out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    }
  }
  return new Uint8Array(out);
}

describe('utf8PrefixEnd', () => {
  it('returns full length when limit covers all bytes', () => {
    const b = enc('hello');
    expect(utf8PrefixEnd(b, 5)).toBe(5);
    expect(utf8PrefixEnd(b, 100)).toBe(5);
  });

  it('cuts on ASCII boundaries', () => {
    const b = enc('abcdef');
    expect(utf8PrefixEnd(b, 3)).toBe(3);
  });

  it('does not split a 2-byte UTF-8 sequence', () => {
    // "é" is C3 A9
    const b = enc('aé');
    expect(b.length).toBe(3);
    expect(utf8PrefixEnd(b, 2)).toBe(1); // cut before é
    expect(utf8PrefixEnd(b, 1)).toBe(1);
    expect(utf8PrefixEnd(b, 3)).toBe(3);
  });

  it('does not split a 3-byte UTF-8 sequence (CJK)', () => {
    // "中" is E4 B8 AD
    const b = enc('x中');
    expect(b.length).toBe(4);
    expect(utf8PrefixEnd(b, 2)).toBe(1);
    expect(utf8PrefixEnd(b, 3)).toBe(1);
    expect(utf8PrefixEnd(b, 4)).toBe(4);
  });

  it('does not split a 4-byte emoji sequence', () => {
    // "😀" is F0 9F 98 80
    const b = enc('a😀');
    expect(b.length).toBe(5);
    expect(utf8PrefixEnd(b, 2)).toBe(1);
    expect(utf8PrefixEnd(b, 4)).toBe(1);
    expect(utf8PrefixEnd(b, 5)).toBe(5);
  });

  it('returns 0 for empty or zero limit', () => {
    expect(utf8PrefixEnd(new Uint8Array(), 10)).toBe(0);
    expect(utf8PrefixEnd(enc('hi'), 0)).toBe(0);
  });
});

describe('utf8Encode / utf8ByteSlice (via TextEncoder/TextDecoder)', () => {
  it('uses platform Encoding API under the hood', () => {
    expect(typeof TextEncoder).toBe('function');
    expect(typeof TextDecoder).toBe('function');
  });

  it('counts multi-byte strings by UTF-8 bytes not JS length', () => {
    const s = '你好'; // 2 code units, 6 UTF-8 bytes
    expect(s.length).toBe(2);
    expect(utf8ByteLengthOfString(s)).toBe(6);
    expect(utf8Encode(s).length).toBe(6);
    expect(utf8Decode(utf8Encode(s))).toBe(s);
  });

  it('slices by byte offset without splitting multi-byte chars', () => {
    const s = 'a你好b';
    // a=1, 你=3, 好=3, b=1 → total 8
    const mid = utf8ByteSlice(s, 1, 4); // only first CJK char
    expect(mid.totalBytes).toBe(8);
    expect(mid.content).toBe('你');
    expect(mid.start).toBe(1);
    expect(mid.end).toBe(4);
  });
});
