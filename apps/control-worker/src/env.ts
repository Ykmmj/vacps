import type { OAuthHelpers } from '@cloudflare/workers-oauth-provider';

export interface Env {
  ASSETS: Fetcher;
  DB: D1Database;
  /** Base64url PKCS#8 Ed25519 private key. It signs control-plane -> Agent requests. */
  CONTROL_PLANE_SIGNING_PRIVATE_KEY?: string;
  /** Base64url raw Ed25519 public key paired with CONTROL_PLANE_SIGNING_PRIVATE_KEY. */
  CONTROL_PLANE_SIGNING_PUBLIC_KEY?: string;
  CONTROL_PANEL_PASSWORD?: string;
  CONTROL_PANEL_SESSION_SECRET?: string;
  // CLOUDFLARE_OAUTH_* below are unrelated to the MCP OAuth provider: they configure the Worker as an
  // OAuth *client* to the Cloudflare API (managed tunnels). Do not reuse them for the MCP flow.
  CLOUDFLARE_OAUTH_CLIENT_ID?: string;
  CLOUDFLARE_OAUTH_CLIENT_SECRET?: string;
  CLOUDFLARE_OAUTH_REDIRECT_URL?: string;
  CLOUDFLARE_OAUTH_SCOPES?: string;
  CLOUDFLARE_ACCOUNT_ID?: string;
  // MCP OAuth 2.1 provider (@cloudflare/workers-oauth-provider): KV-backed client/grant/token store,
  // plus the helper API the provider injects at runtime for use in the /authorize page.
  OAUTH_KV: KVNamespace;
  OAUTH_PROVIDER: OAuthHelpers;
}
