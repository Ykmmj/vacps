import { describe, expect, it } from 'vitest';

import {
  base64Decode,
  base64Encode,
  base64UrlDecode,
  base64UrlEncode,
} from '../../src/security/base64url';

describe('base64 wrappers (vacps:crypto mock)', () => {
  it('standard base64 roundtrip', () => {
    const enc = base64Encode('vacps-base64');
    expect(enc).toBe('dmFjcHMtYmFzZTY0');
    const back = base64Decode(enc);
    expect(new TextDecoder().decode(back)).toBe('vacps-base64');
  });

  it('base64url roundtrip without padding', () => {
    const bytes = new Uint8Array([0xfb, 0xff, 0x00]);
    const enc = base64UrlEncode(bytes);
    expect(enc.includes('+') || enc.includes('/')).toBe(false);
    expect(enc.endsWith('=')).toBe(false);
    const back = base64UrlDecode(enc);
    expect([...back]).toEqual([...bytes]);
  });
});
