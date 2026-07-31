import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import {
  assertSafeAbsolutePath,
  configurePathGuard,
  getAllowedRoots,
  isUnderAllowedRoot,
  resetPathGuardForTests,
  resolveWorkspacePath,
} from '../../src/runtime/path-guard';

afterEach(() => {
  resetPathGuardForTests();
});

describe('configurePathGuard / allowlist', () => {
  it('defaults to /tmp only', () => {
    resetPathGuardForTests();
    expect(getAllowedRoots()).toEqual(['/tmp']);
    expect(assertSafeAbsolutePath('/tmp/foo')).toBe('/tmp/foo');
  });

  it('adds dataDir and extra roots', () => {
    configurePathGuard({
      dataDir: '/var/lib/vacps',
      extraRoots: ['/home/agent/work'],
    });
    expect(assertSafeAbsolutePath('/var/lib/vacps/x')).toBe('/var/lib/vacps/x');
    expect(assertSafeAbsolutePath('/home/agent/work/a')).toBe('/home/agent/work/a');
    expect(assertSafeAbsolutePath('/tmp/z')).toBe('/tmp/z');
  });

  it('rejects paths outside allowlist', () => {
    configurePathGuard({ dataDir: '/data/vacps', replace: true, extraRoots: [] });
    // replace still falls back to /tmp if empty roots — use dataDir + no replace default /tmp
    configurePathGuard({ dataDir: '/data/vacps' });
    expect(() => assertSafeAbsolutePath('/etc/shadow')).toThrow(/outside allowed roots/);
    expect(() => assertSafeAbsolutePath('/root/.ssh/id_ed25519')).toThrow(/outside allowed roots/);
    expect(() => assertSafeAbsolutePath('/etc/vacps/env')).toThrow(/outside allowed roots/);
  });

  it('still rejects kernel filesystems even if listed as extra roots', () => {
    configurePathGuard({
      dataDir: '/data',
      extraRoots: ['/proc', '/sys', '/dev'],
    });
    expect(getAllowedRoots().some((r) => r === '/proc')).toBe(false);
    expect(() => assertSafeAbsolutePath('/proc/self')).toThrow(/kernel filesystem/);
    expect(() => assertSafeAbsolutePath('/sys/kernel')).toThrow(/kernel filesystem/);
    expect(() => assertSafeAbsolutePath('/dev/null')).toThrow(/kernel filesystem/);
  });

  it('isUnderAllowedRoot respects longest root', () => {
    configurePathGuard({
      dataDir: '/data',
      extraRoots: ['/data/nested'],
    });
    expect(isUnderAllowedRoot('/data/nested/x')).toBe(true);
    expect(isUnderAllowedRoot('/data/other')).toBe(true);
    expect(isUnderAllowedRoot('/other')).toBe(false);
  });
});

describe('assertSafeAbsolutePath', () => {
  beforeEach(() => {
    resetPathGuardForTests();
  });

  it('normalizes . and //', () => {
    expect(assertSafeAbsolutePath('/tmp/./a//b')).toBe('/tmp/a/b');
  });

  it('rejects relative paths', () => {
    expect(() => assertSafeAbsolutePath('rel')).toThrow(/absolute/);
  });

  it('rejects empty / nullish', () => {
    expect(() => assertSafeAbsolutePath('')).toThrow(/required/);
  });

  it('rejects null byte', () => {
    expect(() => assertSafeAbsolutePath('/tmp/a\0b')).toThrow(/null byte/);
  });

  it('rejects escaping past root via ..', () => {
    expect(() => assertSafeAbsolutePath('/../etc')).toThrow(/escapes root/);
  });

  it('rejects .. that escapes allowlist root', () => {
    expect(() => assertSafeAbsolutePath('/tmp/../etc/passwd')).toThrow(/outside allowed roots/);
  });
});

describe('resolveWorkspacePath', () => {
  beforeEach(() => {
    resetPathGuardForTests();
  });

  it('joins relative under workspace when workspace allowed', () => {
    expect(resolveWorkspacePath('/tmp/ws', 'a/b')).toBe('/tmp/ws/a/b');
  });

  it('defaults workspace to /tmp', () => {
    expect(resolveWorkspacePath(undefined, 'z')).toBe('/tmp/z');
  });

  it('rejects relative with ..', () => {
    expect(() => resolveWorkspacePath('/tmp/ws', 'a/../b')).toThrow(/\.\./);
  });

  it('rejects absolute paths outside sandbox even if workspace set', () => {
    expect(() => resolveWorkspacePath('/tmp/ws', '/etc/passwd')).toThrow(/outside allowed roots/);
  });
});
