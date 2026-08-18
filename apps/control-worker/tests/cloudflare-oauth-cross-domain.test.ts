import { describe, expect, it, vi } from 'vitest';

import {
  CloudflareOAuthService,
  isCloudflareOAuthCallbackRoute,
} from '../src/cloudflare/oauth-service.js';
import type { Env } from '../src/env.js';
import type {
  CloudflareOAuthRepository,
  CloudflareOAuthState,
} from '../src/registry/cloudflare-oauth-repository.js';

const env = {
  CLOUDFLARE_ACCOUNT_ID: 'a'.repeat(32),
  CLOUDFLARE_OAUTH_CLIENT_ID: 'oauth-client-id',
  CLOUDFLARE_OAUTH_CLIENT_SECRET: 'oauth-client-secret',
  CLOUDFLARE_OAUTH_REDIRECT_URL:
    'https://vacps.example-account.workers.dev/api/cloudflare/oauth/callback',
  CLOUDFLARE_OAUTH_SCOPES: 'tunnel-write dns-write',
  CONTROL_PANEL_PASSWORD: 'control-panel-password',
  CONTROL_PANEL_SESSION_SECRET: 'a-local-session-secret-with-more-than-thirty-two-characters',
  DB: {} as D1Database,
} as Env;

function state(overrides: Partial<CloudflareOAuthState> = {}): CloudflareOAuthState {
  return {
    state: 'oauth-state',
    accountId: 'a'.repeat(32),
    zoneId: '',
    baseDomain: '',
    returnUrl: 'https://control.example/',
    expiresAt: new Date(Date.now() + 60_000).toISOString(),
    createdAt: new Date().toISOString(),
    ...overrides,
  };
}

function repository(
  overrides: Partial<Record<keyof CloudflareOAuthRepository, ReturnType<typeof vi.fn>>> = {},
): CloudflareOAuthRepository {
  return {
    removeExpiredStates: vi.fn().mockResolvedValue(undefined),
    createState: vi.fn(async (input) => ({ ...input, createdAt: new Date().toISOString() })),
    consumeState: vi.fn().mockResolvedValue(state()),
    ...overrides,
  } as unknown as CloudflareOAuthRepository;
}

describe('cross-domain Cloudflare OAuth', () => {
  it('binds the initiating panel origin to the short-lived OAuth state', async () => {
    const oauthRepository = repository();
    const service = new CloudflareOAuthService(env, oauthRepository);

    const result = await service.begin(
      new Request('https://control.example/api/cloudflare/oauth/connect', { method: 'POST' }),
    );

    expect(oauthRepository.createState).toHaveBeenCalledWith(
      expect.objectContaining({ returnUrl: 'https://control.example/' }),
    );
    const authorizationUrl = new URL(result.authorizationUrl);
    expect(authorizationUrl.searchParams.get('redirect_uri')).toBe(
      env.CLOUDFLARE_OAUTH_REDIRECT_URL,
    );
  });

  it('returns to the panel origin after a callback on another hostname', async () => {
    const service = new CloudflareOAuthService(env, repository());

    const response = await service.callback(
      new Request(
        'https://vacps.example-account.workers.dev/api/cloudflare/oauth/callback?state=oauth-state&error=access_denied',
      ),
    );

    expect(response.status).toBe(302);
    expect(response.headers.get('location')).toBe('https://control.example/?cloudflare=denied');
  });

  it('rejects an unsafe stored return URL and falls back to the configured origin', async () => {
    const service = new CloudflareOAuthService(
      env,
      repository({
        consumeState: vi.fn().mockResolvedValue(state({ returnUrl: 'http://attacker.example/' })),
      }),
    );

    const response = await service.callback(
      new Request(
        'https://vacps.example-account.workers.dev/api/cloudflare/oauth/callback?state=oauth-state&error=access_denied',
      ),
    );

    expect(response.headers.get('location')).toBe(
      'https://vacps.example-account.workers.dev/?cloudflare=denied',
    );
  });

  it('exempts only the callback route from the panel session boundary', () => {
    expect(isCloudflareOAuthCallbackRoute('cloudflare', 'oauth', 'callback', 'GET')).toBe(true);
    expect(isCloudflareOAuthCallbackRoute('cloudflare', 'oauth', 'status', 'GET')).toBe(false);
    expect(isCloudflareOAuthCallbackRoute('cloudflare', 'oauth', 'callback', 'POST')).toBe(false);
  });
});
