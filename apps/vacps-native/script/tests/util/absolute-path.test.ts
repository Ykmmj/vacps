import { describe, expect, it } from 'vitest';

import { normalizeAbsolutePath, requireAbsolutePath } from '../../src/util/absolute-path';

function expectInvalidPath(fn: () => unknown, message?: RegExp) {
  try {
    fn();
    expect.fail('expected throw');
  } catch (e) {
    const err = e as Error & { code?: string; statusCode?: number };
    expect(err.code).toBe('invalid_path');
    expect(err.statusCode).toBe(400);
    if (message) expect(err.message).toMatch(message);
  }
}

describe('requireAbsolutePath', () => {
  it('rejects empty / non-string', () => {
    expectInvalidPath(() => requireAbsolutePath(''), /required/);
    expectInvalidPath(() => requireAbsolutePath(null as unknown as string), /required/);
    expectInvalidPath(() => requireAbsolutePath(undefined as unknown as string), /required/);
  });

  it('rejects non-absolute paths', () => {
    expectInvalidPath(() => requireAbsolutePath('rel/path'), /absolute/);
    expectInvalidPath(() => requireAbsolutePath('./x'), /absolute/);
    expectInvalidPath(() => requireAbsolutePath('tmp/foo'), /absolute/);
  });

  it('rejects embedded NUL', () => {
    expectInvalidPath(() => requireAbsolutePath('/tmp/a\0b'), /null byte/);
  });

  it('accepts ordinary absolute paths including /proc and /etc (validation, not sandbox)', () => {
    expect(requireAbsolutePath('/proc/self/status')).toBe('/proc/self/status');
    expect(requireAbsolutePath('/etc/passwd')).toBe('/etc/passwd');
    expect(requireAbsolutePath('/tmp/work/file.txt')).toBe('/tmp/work/file.txt');
  });

  it('normalizes repeated separators, dot, and dotdot', () => {
    expect(requireAbsolutePath('/tmp//a/./b')).toBe('/tmp/a/b');
    expect(requireAbsolutePath('/tmp/a/b/../c')).toBe('/tmp/a/c');
    expect(requireAbsolutePath('/tmp/././x//y/')).toBe('/tmp/x/y');
  });

  it('stops dotdot at root', () => {
    expect(requireAbsolutePath('/../etc')).toBe('/etc');
    expect(requireAbsolutePath('/../../..')).toBe('/');
    expect(requireAbsolutePath('/tmp/../../etc/passwd')).toBe('/etc/passwd');
  });
});

describe('normalizeAbsolutePath', () => {
  it('collapses separators and dots', () => {
    expect(normalizeAbsolutePath('/a//b/./c')).toBe('/a/b/c');
    expect(normalizeAbsolutePath('/')).toBe('/');
    expect(normalizeAbsolutePath('/.')).toBe('/');
  });

  it('applies dotdot and stops at root', () => {
    expect(normalizeAbsolutePath('/a/b/../c')).toBe('/a/c');
    expect(normalizeAbsolutePath('/a/../..')).toBe('/');
    expect(normalizeAbsolutePath('/../x')).toBe('/x');
  });
});
