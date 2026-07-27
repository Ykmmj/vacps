import type { Env } from '../env.js';
import { AppError } from '../lib/http.js';
import type {
  CloudflareOAuthConnection,
  CloudflareOAuthRepository,
} from '../registry/cloudflare-oauth-repository.js';

const AUTHORIZATION_ENDPOINT = 'https://dash.cloudflare.com/oauth2/auth';
const TOKEN_ENDPOINT = 'https://dash.cloudflare.com/oauth2/token';
const STATE_TTL_MILLISECONDS = 10 * 60 * 1000;
const REFRESH_WINDOW_MILLISECONDS = 60 * 1000;

interface OAuthTokenResponse {
  access_token?: string;
  refresh_token?: string;
  expires_in?: number;
  scope?: string;
}

export interface CloudflareConnectionInput {
  zoneId: string;
  baseDomain: string;
}

export interface CloudflareTunnelCredentials extends CloudflareConnectionInput {
  accountId: string;
  accessToken: string;
}

export interface CloudflareZone {
  id: string;
  name: string;
}

export class CloudflareOAuthService {
  constructor(
    private readonly env: Env,
    private readonly repository: CloudflareOAuthRepository,
  ) {}

  async status(): Promise<{
    configured: boolean;
    connected: boolean;
    accountId?: string;
    zoneId?: string;
    baseDomain?: string;
    connectedAt?: string;
  }> {
    const configured = this.isConfigured();
    if (!configured) return { configured, connected: false };
    const connection = await this.repository.connection();
    if (!connection)
      return {
        configured,
        connected: false,
        ...(this.env.CLOUDFLARE_ACCOUNT_ID ? { accountId: this.env.CLOUDFLARE_ACCOUNT_ID } : {}),
      };
    return {
      configured,
      connected: true,
      accountId: connection.accountId,
      ...(connection.zoneId ? { zoneId: connection.zoneId } : {}),
      ...(connection.baseDomain ? { baseDomain: connection.baseDomain } : {}),
      connectedAt: connection.connectedAt,
    };
  }

  async begin(): Promise<{ authorizationUrl: string }> {
    const configuration = this.configuration();
    const accountId = this.env.CLOUDFLARE_ACCOUNT_ID;
    if (!accountId)
      throw new AppError(
        'cloudflare_account_not_configured',
        'Run the Managed Tunnel bootstrap again to save the Cloudflare account for this control plane.',
        409,
      );
    await this.repository.removeExpiredStates();
    const state = randomBase64Url(32);
    await this.repository.createState({
      state,
      accountId,
      zoneId: '',
      baseDomain: '',
      expiresAt: new Date(Date.now() + STATE_TTL_MILLISECONDS).toISOString(),
    });
    const authorizationUrl = new URL(AUTHORIZATION_ENDPOINT);
    authorizationUrl.searchParams.set('response_type', 'code');
    authorizationUrl.searchParams.set('client_id', configuration.clientId);
    authorizationUrl.searchParams.set('redirect_uri', configuration.redirectUrl);
    authorizationUrl.searchParams.set('state', state);
    authorizationUrl.searchParams.set('scope', configuration.scopes);
    return { authorizationUrl: authorizationUrl.toString() };
  }

  async callback(request: Request): Promise<Response> {
    const configuration = this.configuration();
    const url = new URL(request.url);
    const state = url.searchParams.get('state');
    const returnUrl = returnUrlFor(configuration.redirectUrl);
    if (!state) return redirect(returnUrl, 'missing_state');
    const pending = await this.repository.consumeState(state);
    if (!pending || Date.parse(pending.expiresAt) <= Date.now())
      return redirect(returnUrl, 'expired');
    if (url.searchParams.has('error')) return redirect(returnUrl, 'denied');
    const code = url.searchParams.get('code');
    if (!code) return redirect(returnUrl, 'missing_code');

    try {
      const tokens = await this.tokenRequest({ grant_type: 'authorization_code', code });
      if (!tokens.access_token)
        throw new AppError('cloudflare_oauth_failed', 'No access token returned.', 502);
      await this.repository.saveConnection({
        accountId: pending.accountId,
        zoneId: pending.zoneId,
        baseDomain: pending.baseDomain,
        accessTokenCiphertext: await this.encrypt(tokens.access_token, configuration.clientSecret),
        ...(tokens.refresh_token
          ? {
              refreshTokenCiphertext: await this.encrypt(
                tokens.refresh_token,
                configuration.clientSecret,
              ),
            }
          : {}),
        ...(tokens.expires_in
          ? { expiresAt: new Date(Date.now() + tokens.expires_in * 1000).toISOString() }
          : {}),
        ...(tokens.scope ? { scopes: tokens.scope } : {}),
      });
      return redirect(returnUrl, 'connected');
    } catch (error) {
      const result = error instanceof AppError ? error.code : 'token_exchange_failed';
      console.error('Cloudflare OAuth callback failed', { result });
      return redirect(returnUrl, result);
    }
  }

  async disconnect(): Promise<void> {
    await this.repository.deleteConnection();
  }

  async zones(): Promise<CloudflareZone[]> {
    const { connection, accessToken } = await this.authorizedConnection();
    const url = new URL('https://api.cloudflare.com/client/v4/zones');
    url.searchParams.set('account.id', connection.accountId);
    url.searchParams.set('per_page', '50');
    const response = await fetch(url, {
      headers: { authorization: `Bearer ${accessToken}` },
    });
    const payload = (await response.json().catch(() => undefined)) as
      { success?: boolean; result?: unknown; errors?: Array<{ message?: string }> } | undefined;
    if (!response.ok || !payload?.success || !Array.isArray(payload.result)) {
      throw new AppError(
        'cloudflare_zones_unavailable',
        payload?.errors?.[0]?.message ?? 'Cloudflare could not list DNS zones for this account.',
        502,
      );
    }
    return payload.result
      .filter((zone): zone is { id: string; name: string; status?: string } =>
        Boolean(
          zone &&
          typeof zone === 'object' &&
          typeof zone.id === 'string' &&
          typeof zone.name === 'string',
        ),
      )
      .filter((zone) => zone.status === undefined || zone.status === 'active')
      .map((zone) => ({ id: zone.id, name: normalizeBaseDomain(zone.name) }))
      .sort((left, right) => left.name.localeCompare(right.name));
  }

  async selectZone(zoneId: string): Promise<Awaited<ReturnType<CloudflareOAuthService['status']>>> {
    const { connection } = await this.authorizedConnection();
    const zone = (await this.zones()).find((candidate) => candidate.id === zoneId);
    if (!zone)
      throw new AppError(
        'cloudflare_zone_not_found',
        'Select a DNS zone available to this account.',
        404,
      );
    await this.repository.saveConnection({
      accountId: connection.accountId,
      zoneId: zone.id,
      baseDomain: zone.name,
      accessTokenCiphertext: connection.accessTokenCiphertext,
      ...(connection.refreshTokenCiphertext
        ? { refreshTokenCiphertext: connection.refreshTokenCiphertext }
        : {}),
      ...(connection.expiresAt ? { expiresAt: connection.expiresAt } : {}),
      ...(connection.scopes ? { scopes: connection.scopes } : {}),
    });
    return this.status();
  }

  async credentials(): Promise<CloudflareTunnelCredentials> {
    const { connection, accessToken } = await this.authorizedConnection();
    if (!connection.zoneId || !connection.baseDomain)
      throw new AppError(
        'cloudflare_zone_not_selected',
        'Select a DNS zone before creating a managed Tunnel.',
        409,
      );
    return {
      accountId: connection.accountId,
      zoneId: connection.zoneId,
      baseDomain: connection.baseDomain,
      accessToken,
    };
  }

  private isConfigured(): boolean {
    return Boolean(
      this.env.CLOUDFLARE_OAUTH_CLIENT_ID &&
      this.env.CLOUDFLARE_OAUTH_CLIENT_SECRET &&
      this.env.CLOUDFLARE_OAUTH_REDIRECT_URL &&
      this.env.CLOUDFLARE_OAUTH_SCOPES,
    );
  }

  private configuration(): {
    clientId: string;
    clientSecret: string;
    redirectUrl: string;
    scopes: string;
  } {
    const clientId = this.env.CLOUDFLARE_OAUTH_CLIENT_ID;
    const clientSecret = this.env.CLOUDFLARE_OAUTH_CLIENT_SECRET;
    const redirectUrl = this.env.CLOUDFLARE_OAUTH_REDIRECT_URL;
    const scopes = this.env.CLOUDFLARE_OAUTH_SCOPES;
    if (!clientId || !clientSecret || !redirectUrl || !scopes)
      throw new AppError(
        'cloudflare_oauth_not_configured',
        'Cloudflare OAuth is not configured for this control plane.',
        409,
      );
    try {
      const parsed = new URL(redirectUrl);
      const localhost = parsed.hostname === 'localhost' || parsed.hostname === '127.0.0.1';
      if (parsed.protocol !== 'https:' && !localhost)
        throw new Error('A public callback requires HTTPS.');
      return { clientId, clientSecret, redirectUrl: parsed.toString(), scopes };
    } catch {
      throw new AppError(
        'cloudflare_oauth_misconfigured',
        'The Cloudflare OAuth callback URL is invalid.',
        500,
      );
    }
  }

  private async accessToken(
    connection: CloudflareOAuthConnection,
    clientSecret: string,
  ): Promise<string> {
    const expiresAt = connection.expiresAt ? Date.parse(connection.expiresAt) : undefined;
    if (!expiresAt || expiresAt > Date.now() + REFRESH_WINDOW_MILLISECONDS)
      return this.decrypt(connection.accessTokenCiphertext, clientSecret);
    if (!connection.refreshTokenCiphertext)
      throw new AppError(
        'cloudflare_authorization_expired',
        'Cloudflare authorization expired. Connect Cloudflare again.',
        // Must not be 401: the Web UI treats authentication_required 401 as a control-panel logout.
        409,
      );
    const refreshToken = await this.decrypt(connection.refreshTokenCiphertext, clientSecret);
    const tokens = await this.tokenRequest({
      grant_type: 'refresh_token',
      refresh_token: refreshToken,
    });
    if (!tokens.access_token)
      throw new AppError(
        'cloudflare_authorization_expired',
        'Cloudflare authorization expired. Connect Cloudflare again.',
        409,
      );
    await this.repository.saveConnection({
      accountId: connection.accountId,
      zoneId: connection.zoneId,
      baseDomain: connection.baseDomain,
      accessTokenCiphertext: await this.encrypt(tokens.access_token, clientSecret),
      refreshTokenCiphertext: tokens.refresh_token
        ? await this.encrypt(tokens.refresh_token, clientSecret)
        : connection.refreshTokenCiphertext,
      ...(tokens.expires_in
        ? { expiresAt: new Date(Date.now() + tokens.expires_in * 1000).toISOString() }
        : {}),
      ...(tokens.scope
        ? { scopes: tokens.scope }
        : connection.scopes
          ? { scopes: connection.scopes }
          : {}),
    });
    return tokens.access_token;
  }

  private async authorizedConnection(): Promise<{
    connection: CloudflareOAuthConnection;
    accessToken: string;
  }> {
    const configuration = this.configuration();
    const connection = await this.repository.connection();
    if (!connection)
      throw new AppError(
        'cloudflare_not_connected',
        'Connect Cloudflare before creating a managed Tunnel.',
        409,
      );
    const accessToken = await this.accessToken(connection, configuration.clientSecret);
    return { connection: (await this.repository.connection()) ?? connection, accessToken };
  }

  private async tokenRequest(
    grant:
      | { grant_type: 'authorization_code'; code: string }
      | { grant_type: 'refresh_token'; refresh_token: string },
  ): Promise<OAuthTokenResponse> {
    const configuration = this.configuration();
    const body = new URLSearchParams({
      ...grant,
      client_id: configuration.clientId,
      client_secret: configuration.clientSecret,
      redirect_uri: configuration.redirectUrl,
    });
    const response = await fetch(TOKEN_ENDPOINT, {
      method: 'POST',
      headers: { 'content-type': 'application/x-www-form-urlencoded' },
      body,
    });
    const payload = (await response.json().catch(() => undefined)) as
      (OAuthTokenResponse & { error?: string; error_description?: string }) | undefined;
    if (!response.ok || !payload) {
      const oauthError = payload?.error;
      throw new AppError(
        oauthError === 'invalid_client' || oauthError === 'invalid_grant'
          ? `cloudflare_oauth_${oauthError}`
          : 'cloudflare_oauth_failed',
        payload?.error_description ?? payload?.error ?? 'Cloudflare OAuth token exchange failed.',
        502,
      );
    }
    return payload;
  }

  private async encrypt(value: string, clientSecret: string): Promise<string> {
    const key = await encryptionKey(clientSecret, ['encrypt']);
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const encrypted = new Uint8Array(
      await crypto.subtle.encrypt(
        { name: 'AES-GCM', iv: arrayBuffer(iv) },
        key,
        arrayBuffer(new TextEncoder().encode(value)),
      ),
    );
    return `${base64Url(iv)}.${base64Url(encrypted)}`;
  }

  private async decrypt(ciphertext: string, clientSecret: string): Promise<string> {
    const [encodedIv, encodedValue, extra] = ciphertext.split('.');
    if (!encodedIv || !encodedValue || extra)
      throw new AppError(
        'cloudflare_oauth_corrupt',
        'Cloudflare authorization data is invalid.',
        500,
      );
    try {
      const key = await encryptionKey(clientSecret, ['decrypt']);
      const decrypted = await crypto.subtle.decrypt(
        { name: 'AES-GCM', iv: arrayBuffer(fromBase64Url(encodedIv)) },
        key,
        arrayBuffer(fromBase64Url(encodedValue)),
      );
      return new TextDecoder().decode(decrypted);
    } catch {
      throw new AppError(
        'cloudflare_oauth_corrupt',
        'Cloudflare authorization data cannot be decrypted. Connect Cloudflare again.',
        409,
      );
    }
  }
}

export function normalizeBaseDomain(value: string): string {
  const baseDomain = value.trim().toLowerCase().replace(/\.$/, '');
  if (
    !/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$/.test(
      baseDomain,
    )
  )
    throw new AppError('invalid_base_domain', 'The managed Tunnel base domain is invalid.');
  return baseDomain;
}

async function encryptionKey(secret: string, usages: KeyUsage[]): Promise<CryptoKey> {
  const material = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(secret));
  return crypto.subtle.importKey('raw', material, { name: 'AES-GCM' }, false, usages);
}

function randomBase64Url(length: number): string {
  return base64Url(crypto.getRandomValues(new Uint8Array(length)));
}

function base64Url(value: Uint8Array): string {
  let binary = '';
  for (const byte of value) binary += String.fromCharCode(byte);
  return btoa(binary).replaceAll('+', '-').replaceAll('/', '_').replace(/=+$/, '');
}

function fromBase64Url(value: string): Uint8Array {
  const base64 =
    value.replaceAll('-', '+').replaceAll('_', '/') + '='.repeat((4 - (value.length % 4)) % 4);
  const binary = atob(base64);
  return Uint8Array.from(binary, (character) => character.charCodeAt(0));
}

function arrayBuffer(value: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(value.byteLength);
  copy.set(value);
  return copy.buffer;
}

function returnUrlFor(redirectUrl: string): URL {
  const url = new URL(redirectUrl);
  url.pathname = '/';
  url.search = '';
  url.hash = '';
  return url;
}

function redirect(url: URL, result: string): Response {
  url.searchParams.set('cloudflare', result);
  return Response.redirect(url.toString(), 302);
}
