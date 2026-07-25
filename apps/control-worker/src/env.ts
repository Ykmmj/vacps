export interface Env {
  ASSETS: Fetcher;
  DB: D1Database;
  BACKEND_SHARED_TOKEN: string;
  CLOUDFLARE_OAUTH_CLIENT_ID?: string;
  CLOUDFLARE_OAUTH_CLIENT_SECRET?: string;
  CLOUDFLARE_OAUTH_REDIRECT_URL?: string;
}
