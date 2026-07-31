import { describe, expect, it } from 'vitest';

import {
  allowUnsignedWhenNoKey,
  assertControlPlaneAuthConfig,
  isPublicHttpPath,
  PUBLIC_HTTP_PATHS,
} from '../../src/security/http-auth';

describe('isPublicHttpPath', () => {
  it('only allows exact /health', () => {
    expect(isPublicHttpPath('/health')).toBe(true);
    expect(PUBLIC_HTTP_PATHS.has('/health')).toBe(true);
  });

  it('does not treat former public paths as open', () => {
    for (const p of [
      '/ready',
      '/status',
      '/info',
      '/metrics',
      '/capabilities',
      '/script/ping',
      '/tasks',
      '/tasks/abc',
      '/fs/read',
      '/fs/stat',
      '/healthz',
      '/health/extra',
    ]) {
      expect(isPublicHttpPath(p)).toBe(false);
    }
  });
});

describe('assertControlPlaneAuthConfig', () => {
  it('accepts when public key is present', () => {
    expect(() =>
      assertControlPlaneAuthConfig({
        CONTROL_PLANE_PUBLIC_KEY: 'A'.repeat(43),
      }),
    ).not.toThrow();
  });

  it('accepts missing key only with ALLOW_INSECURE_NO_AUTH', () => {
    expect(() =>
      assertControlPlaneAuthConfig({
        ALLOW_INSECURE_NO_AUTH: true,
      }),
    ).not.toThrow();
  });

  it('rejects missing key in production mode', () => {
    expect(() => assertControlPlaneAuthConfig({})).toThrow(/CONTROL_PLANE_PUBLIC_KEY is required/);
    expect(() => assertControlPlaneAuthConfig({ ALLOW_INSECURE_NO_AUTH: false })).toThrow(
      /CONTROL_PLANE_PUBLIC_KEY is required/,
    );
  });
});

describe('allowUnsignedWhenNoKey', () => {
  it('is true only for insecure + no key', () => {
    expect(allowUnsignedWhenNoKey({ ALLOW_INSECURE_NO_AUTH: true })).toBe(true);
    expect(
      allowUnsignedWhenNoKey({
        CONTROL_PLANE_PUBLIC_KEY: 'x',
        ALLOW_INSECURE_NO_AUTH: true,
      }),
    ).toBe(false);
    expect(allowUnsignedWhenNoKey({})).toBe(false);
  });
});
