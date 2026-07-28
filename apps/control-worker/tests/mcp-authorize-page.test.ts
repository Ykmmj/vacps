import { describe, expect, it } from 'vitest';

import { createSessionCookie } from '../src/auth/session.js';
import type { Env } from '../src/env.js';
import { handleAuthorize } from '../src/mcp/authorize-page.js';

const PASSWORD = 'control-panel-password';
const SECRET = 'a-local-session-secret-with-more-than-thirty-two-characters';

const AUTH_REQUEST = {
  responseType: 'code',
  clientId: 'client-1',
  redirectUri: 'https://client.example/callback',
  scope: ['mcp'],
  state: 'state-xyz',
  codeChallenge: 'challenge',
  codeChallengeMethod: 'S256',
};

const AUTHORIZE_URL =
  'https://control.example/authorize?response_type=code&client_id=client-1' +
  '&redirect_uri=https%3A%2F%2Fclient.example%2Fcallback&scope=mcp&state=state-xyz' +
  '&code_challenge=challenge&code_challenge_method=S256';

function makeEnv(overrides: Record<string, unknown> = {}): Env {
  const provider = {
    parseAuthRequest: async () => AUTH_REQUEST,
    lookupClient: async (id: string) =>
      id === 'client-1' ? { clientId: 'client-1', clientName: 'ChatGPT' } : null,
    completeAuthorization: async () => ({
      redirectTo: 'https://client.example/callback?code=auth-code&state=state-xyz',
    }),
    ...overrides,
  };
  return {
    CONTROL_PANEL_PASSWORD: PASSWORD,
    CONTROL_PANEL_SESSION_SECRET: SECRET,
    OAUTH_PROVIDER: provider,
  } as unknown as Env;
}

function formPost(body: Record<string, string>, cookie?: string): Request {
  const headers: Record<string, string> = {
    'content-type': 'application/x-www-form-urlencoded',
  };
  if (cookie) headers.cookie = cookie;
  return new Request(AUTHORIZE_URL, {
    method: 'POST',
    headers,
    body: new URLSearchParams(body),
  });
}

describe('MCP OAuth authorize page', () => {
  it('renders a consent page with the client name and a password field when unauthenticated', async () => {
    const response = await handleAuthorize(new Request(AUTHORIZE_URL), makeEnv());
    expect(response.status).toBe(200);
    expect(response.headers.get('cache-control')).toBe('no-store');
    const html = await response.text();
    expect(html).toContain('ChatGPT');
    expect(html).toContain('name="password"');
  });

  it('omits the password field when a valid session cookie is present', async () => {
    const env = makeEnv();
    const cookie = (await createSessionCookie(env)).split(';')[0] ?? '';
    const response = await handleAuthorize(
      new Request(AUTHORIZE_URL, { headers: { cookie } }),
      env,
    );
    const html = await response.text();
    expect(html).not.toContain('name="password"');
  });

  it('rejects an unknown client with a 400', async () => {
    const env = makeEnv({ lookupClient: async () => null });
    const response = await handleAuthorize(new Request(AUTHORIZE_URL), env);
    expect(response.status).toBe(400);
  });

  it('redirects back to the client with access_denied when the user denies', async () => {
    const response = await handleAuthorize(formPost({ decision: 'deny' }), makeEnv());
    expect(response.status).toBe(302);
    const location = new URL(response.headers.get('location') ?? '');
    expect(location.origin + location.pathname).toBe('https://client.example/callback');
    expect(location.searchParams.get('error')).toBe('access_denied');
    expect(location.searchParams.get('state')).toBe('state-xyz');
  });

  it('re-renders with an error and 401 when the submitted password is wrong', async () => {
    const response = await handleAuthorize(
      formPost({ decision: 'approve', password: 'wrong-password' }),
      makeEnv(),
    );
    expect(response.status).toBe(401);
    expect(await response.text()).toContain('Incorrect password');
  });

  it('completes authorization and sets a session cookie on a correct password', async () => {
    const response = await handleAuthorize(
      formPost({ decision: 'approve', password: PASSWORD }),
      makeEnv(),
    );
    expect(response.status).toBe(302);
    expect(response.headers.get('location')).toBe(
      'https://client.example/callback?code=auth-code&state=state-xyz',
    );
    expect(response.headers.get('set-cookie')).toContain('vacps_control_session=');
  });

  it('completes authorization with an existing session and no password', async () => {
    const env = makeEnv();
    const cookie = (await createSessionCookie(env)).split(';')[0] ?? '';
    const response = await handleAuthorize(formPost({ decision: 'approve' }, cookie), env);
    expect(response.status).toBe(302);
    expect(response.headers.get('location')).toBe(
      'https://client.example/callback?code=auth-code&state=state-xyz',
    );
  });
});
