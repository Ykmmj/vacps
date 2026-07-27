import type { AuthRequest, ClientInfo } from '@cloudflare/workers-oauth-provider';

import { createSessionCookie, hasValidSession, passwordMatches } from '../auth/session.js';
import type { Env } from '../env.js';

// The control plane has a single shared password rather than per-user identity, so every MCP grant
// maps to this one constant subject. The OAuth layer gives a remote client (e.g. ChatGPT) a
// token-based path that is equivalent to knowing the control-panel password — it is not multi-user.
const CONTROL_PANEL_USER_ID = 'control-panel';

const noStore = { 'cache-control': 'no-store' } as const;

/**
 * Server-rendered OAuth consent page for the Remote MCP endpoint. This lives in the Worker (not the
 * Svelte SPA) because the authorization flow must end in a server-side 302 back to the client's
 * redirect_uri with an auth code. It reuses the existing single-password primitives from
 * `auth/session.ts`.
 */
export async function handleAuthorize(request: Request, env: Env): Promise<Response> {
  if (request.method === 'GET') return renderConsent(request, env);
  if (request.method === 'POST') return submitConsent(request, env);
  return new Response('Method Not Allowed', { status: 405, headers: noStore });
}

async function renderConsent(request: Request, env: Env): Promise<Response> {
  let authRequest: AuthRequest;
  try {
    authRequest = await env.OAUTH_PROVIDER.parseAuthRequest(request);
  } catch {
    return htmlResponse(errorPage('This authorization request is invalid or incomplete.'), 400);
  }
  const client = await env.OAUTH_PROVIDER.lookupClient(authRequest.clientId);
  if (!client) {
    return htmlResponse(errorPage('The requesting client is not registered.'), 400);
  }
  const authenticated = await hasValidSession(request, env);
  return htmlResponse(consentPage({ request, client, authenticated }));
}

async function submitConsent(request: Request, env: Env): Promise<Response> {
  let authRequest: AuthRequest;
  try {
    // The original OAuth query parameters are preserved on the form's action URL, so we re-parse them
    // here rather than trusting a serialized hidden field the browser could tamper with.
    authRequest = await env.OAUTH_PROVIDER.parseAuthRequest(request);
  } catch {
    return htmlResponse(errorPage('This authorization request is invalid or incomplete.'), 400);
  }
  const client = await env.OAUTH_PROVIDER.lookupClient(authRequest.clientId);
  if (!client) {
    return htmlResponse(errorPage('The requesting client is not registered.'), 400);
  }

  const form = await request.formData();
  const decision = form.get('decision');

  if (decision === 'deny') {
    return Response.redirect(denyRedirectUrl(authRequest), 302);
  }

  // Approve requires either an existing control-panel session or the correct password submitted now.
  let authenticated = await hasValidSession(request, env);
  let setCookie: string | undefined;
  if (!authenticated) {
    const password = form.get('password');
    if (typeof password === 'string' && (await passwordMatches(password, env))) {
      authenticated = true;
      setCookie = await createSessionCookie(env);
    }
  }
  if (!authenticated) {
    return htmlResponse(
      consentPage({
        request,
        client,
        authenticated: false,
        error: 'Incorrect password.',
      }),
      401,
    );
  }

  const { redirectTo } = await env.OAUTH_PROVIDER.completeAuthorization({
    request: authRequest,
    userId: CONTROL_PANEL_USER_ID,
    scope: authRequest.scope,
    metadata: { via: 'mcp' },
    props: { userId: CONTROL_PANEL_USER_ID },
  });

  const headers = new Headers({ location: redirectTo, ...noStore });
  if (setCookie) headers.append('set-cookie', setCookie);
  return new Response(null, { status: 302, headers });
}

function denyRedirectUrl(authRequest: AuthRequest): string {
  const url = new URL(authRequest.redirectUri);
  url.searchParams.set('error', 'access_denied');
  if (authRequest.state) url.searchParams.set('state', authRequest.state);
  return url.toString();
}

function htmlResponse(body: string, status = 200): Response {
  return new Response(body, {
    status,
    headers: { 'content-type': 'text/html; charset=utf-8', ...noStore },
  });
}

interface ConsentPageOptions {
  request: Request;
  client: ClientInfo;
  authenticated: boolean;
  error?: string;
}

function consentPage({ request, client, authenticated, error }: ConsentPageOptions): string {
  const clientName = escapeHtml(client.clientName || client.clientId);
  const action = escapeHtml(new URL(request.url).href);
  const errorBlock = error ? `<p class="error">${escapeHtml(error)}</p>` : '';
  const passwordField = authenticated
    ? ''
    : `<label for="password">Control panel password</label>
       <input id="password" name="password" type="password" autocomplete="current-password"
              autofocus required />`;
  return layout(
    'Authorize MCP access',
    `<h1>Authorize access</h1>
     <p><strong>${clientName}</strong> is requesting access to the VPS Agent control plane over MCP.</p>
     <p class="note">Granting access is equivalent to sharing the control-panel password: the client
       will be able to list backends, queue tasks (including shell commands), and manage schedules.</p>
     ${errorBlock}
     <form method="post" action="${action}">
       ${passwordField}
       <div class="actions">
         <button type="submit" name="decision" value="approve" class="primary">Approve</button>
         <button type="submit" name="decision" value="deny" class="secondary">Deny</button>
       </div>
     </form>`,
  );
}

function errorPage(message: string): string {
  return layout(
    'Authorization error',
    `<h1>Authorization error</h1><p class="error">${escapeHtml(message)}</p>`,
  );
}

function layout(title: string, body: string): string {
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <meta name="robots" content="noindex" />
  <title>${escapeHtml(title)}</title>
  <style>
    :root { color-scheme: light dark; }
    body { font-family: system-ui, sans-serif; margin: 0; display: grid; place-items: center;
           min-height: 100vh; background: Canvas; color: CanvasText; }
    main { width: min(28rem, 92vw); padding: 2rem; border: 1px solid color-mix(in srgb, CanvasText 15%, transparent);
           border-radius: 12px; }
    h1 { font-size: 1.25rem; margin: 0 0 1rem; }
    p { line-height: 1.5; }
    .note { font-size: 0.875rem; opacity: 0.8; }
    .error { color: #b91c1c; font-weight: 600; }
    label { display: block; margin: 1rem 0 0.25rem; font-size: 0.875rem; }
    input { width: 100%; box-sizing: border-box; padding: 0.6rem; border-radius: 8px;
            border: 1px solid color-mix(in srgb, CanvasText 25%, transparent); background: Field; color: FieldText; }
    .actions { display: flex; gap: 0.75rem; margin-top: 1.5rem; }
    button { flex: 1; padding: 0.6rem 1rem; border-radius: 8px; border: 0; font-size: 0.95rem; cursor: pointer; }
    .primary { background: #2563eb; color: #fff; }
    .secondary { background: color-mix(in srgb, CanvasText 12%, transparent); color: CanvasText; }
  </style>
</head>
<body><main>${body}</main></body>
</html>`;
}

function escapeHtml(value: string): string {
  return value
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;');
}
