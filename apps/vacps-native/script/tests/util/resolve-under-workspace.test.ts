import { describe, expect, it } from 'vitest';

import { resolveUnderWorkspace } from '../../src/util/resolve-under-workspace';

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

describe('resolveUnderWorkspace', () => {
  const ws = '/tmp/ws';

  it('resolves relative child paths under workspace', () => {
    expect(resolveUnderWorkspace(ws, 'a/b')).toBe('/tmp/ws/a/b');
    expect(resolveUnderWorkspace(ws, 'file.txt')).toBe('/tmp/ws/file.txt');
    expect(resolveUnderWorkspace(ws, 'a/./b//c')).toBe('/tmp/ws/a/b/c');
  });

  it('rejects relative paths containing ..', () => {
    expectInvalidPath(() => resolveUnderWorkspace(ws, 'a/../b'), /\.\./);
    expectInvalidPath(() => resolveUnderWorkspace(ws, '..'), /\.\./);
    expectInvalidPath(() => resolveUnderWorkspace(ws, '../escape'), /\.\./);
  });

  it('accepts normalized absolute children of workspace', () => {
    expect(resolveUnderWorkspace(ws, '/tmp/ws/a')).toBe('/tmp/ws/a');
    expect(resolveUnderWorkspace(ws, '/tmp/ws/./x//y')).toBe('/tmp/ws/x/y');
    expect(resolveUnderWorkspace(ws, '/tmp/ws/a/b/../c')).toBe('/tmp/ws/a/c');
  });

  it('rejects absolute paths outside workspace including normalized escape', () => {
    expectInvalidPath(() => resolveUnderWorkspace(ws, '/etc/passwd'), /escapes workspace/);
    expectInvalidPath(() => resolveUnderWorkspace(ws, '/tmp/other'), /escapes workspace/);
    expectInvalidPath(() => resolveUnderWorkspace(ws, '/tmp/ws/../other'), /escapes workspace/);
    expectInvalidPath(() => resolveUnderWorkspace(ws, '/tmp/ws/../../etc'), /escapes workspace/);
  });

  it('rejects prefix-twin paths (workspace is not a string prefix alone)', () => {
    expectInvalidPath(() => resolveUnderWorkspace(ws, '/tmp/ws2/x'), /escapes workspace/);
    expectInvalidPath(() => resolveUnderWorkspace('/tmp/ws', '/tmp/ws-extra/f'), /escapes workspace/);
  });

  it('treats workspace root itself as in-scope', () => {
    expect(resolveUnderWorkspace(ws, '/tmp/ws')).toBe('/tmp/ws');
    expect(resolveUnderWorkspace(ws, '/tmp/ws/.')).toBe('/tmp/ws');
  });

  it('supports workspace at filesystem root', () => {
    expect(resolveUnderWorkspace('/', 'etc/passwd')).toBe('/etc/passwd');
    expect(resolveUnderWorkspace('/', '/proc/self/status')).toBe('/proc/self/status');
    expect(resolveUnderWorkspace('/', '/')).toBe('/');
  });

  it('rejects empty path and embedded NUL', () => {
    expectInvalidPath(() => resolveUnderWorkspace(ws, ''), /required/);
    expectInvalidPath(() => resolveUnderWorkspace(ws, null as unknown as string), /required/);
    expectInvalidPath(() => resolveUnderWorkspace(ws, 'a\0b'), /null byte/);
    expectInvalidPath(() => resolveUnderWorkspace(ws, '/tmp/ws/\0x'), /null byte/);
  });
});
