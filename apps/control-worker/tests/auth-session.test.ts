import { describe, expect, it } from 'vitest';

import {
  createSessionCookie,
  hasValidSession,
  passwordMatches,
  SESSION_MAX_AGE_SECONDS,
} from '../src/auth/session.js';
import type { Env } from '../src/env.js';

const env = {
  CONTROL_PANEL_PASSWORD: 'control-panel-password',
  CONTROL_PANEL_SESSION_SECRET: 'a-local-session-secret-with-more-than-thirty-two-characters',
} as Env;

describe('control panel sessions', () => {
  it('issues a signed, HttpOnly twelve-hour cookie that can be verified', async () => {
    const now = Date.UTC(2026, 0, 1);
    const cookie = await createSessionCookie(env, now);
    const value = cookie.split(';')[0] ?? '';

    expect(cookie).toContain('HttpOnly');
    expect(cookie).toContain('Secure');
    expect(cookie).toContain('SameSite=Lax');
    expect(cookie).toContain(`Max-Age=${SESSION_MAX_AGE_SECONDS}`);
    await expect(
      hasValidSession(
        new Request('https://control.example/api/auth/session', { headers: { cookie: value } }),
        env,
        now + SESSION_MAX_AGE_SECONDS * 1000 - 1,
      ),
    ).resolves.toBe(true);
  });

  it('rejects modified and expired session payloads', async () => {
    const now = Date.UTC(2026, 0, 1);
    const cookie = await createSessionCookie(env, now);
    const value = cookie.split(';')[0] ?? '';
    const separator = value.lastIndexOf('.');
    const signature = value.slice(separator + 1);
    const tampered = `${value.slice(0, separator + 1)}${signature.startsWith('a') ? 'b' : 'a'}${signature.slice(1)}`;

    await expect(
      hasValidSession(
        new Request('https://control.example/api/auth/session', { headers: { cookie: tampered } }),
        env,
        now,
      ),
    ).resolves.toBe(false);
    await expect(
      hasValidSession(
        new Request('https://control.example/api/auth/session', { headers: { cookie: value } }),
        env,
        now + SESSION_MAX_AGE_SECONDS * 1000,
      ),
    ).resolves.toBe(false);
  });

  it('checks the control password without exposing it', async () => {
    await expect(passwordMatches('control-panel-password', env)).resolves.toBe(true);
    await expect(passwordMatches('incorrect-control-password', env)).resolves.toBe(false);
  });
});
