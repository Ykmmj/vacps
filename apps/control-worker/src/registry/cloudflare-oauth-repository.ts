export interface CloudflareOAuthConnection {
  accountId: string;
  zoneId: string;
  baseDomain: string;
  accessTokenCiphertext: string;
  refreshTokenCiphertext?: string;
  expiresAt?: string;
  scopes?: string;
  connectedAt: string;
  updatedAt: string;
}

export interface CloudflareOAuthState {
  state: string;
  accountId: string;
  zoneId: string;
  baseDomain: string;
  expiresAt: string;
  createdAt: string;
}

interface CloudflareOAuthConnectionRow {
  account_id: string;
  zone_id: string;
  base_domain: string;
  access_token_ciphertext: string;
  refresh_token_ciphertext: string | null;
  expires_at: string | null;
  scopes: string | null;
  connected_at: string;
  updated_at: string;
}

interface CloudflareOAuthStateRow {
  state: string;
  account_id: string;
  zone_id: string;
  base_domain: string;
  expires_at: string;
  created_at: string;
}

export class CloudflareOAuthRepository {
  constructor(private readonly db: D1Database) {}

  async connection(): Promise<CloudflareOAuthConnection | undefined> {
    const row = await this.db
      .prepare("SELECT * FROM cloudflare_oauth_connections WHERE id = 'default'")
      .first<CloudflareOAuthConnectionRow>();
    return row ? toConnection(row) : undefined;
  }

  async saveConnection(
    input: Omit<CloudflareOAuthConnection, 'connectedAt' | 'updatedAt'>,
  ): Promise<CloudflareOAuthConnection> {
    const existing = await this.connection();
    const now = new Date().toISOString();
    const connectedAt = existing?.connectedAt ?? now;
    await this.db
      .prepare(
        `INSERT INTO cloudflare_oauth_connections (
           id, account_id, zone_id, base_domain, access_token_ciphertext,
           refresh_token_ciphertext, expires_at, scopes, connected_at, updated_at
         ) VALUES ('default', ?, ?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(id) DO UPDATE SET
           account_id = excluded.account_id,
           zone_id = excluded.zone_id,
           base_domain = excluded.base_domain,
           access_token_ciphertext = excluded.access_token_ciphertext,
           refresh_token_ciphertext = excluded.refresh_token_ciphertext,
           expires_at = excluded.expires_at,
           scopes = excluded.scopes,
           updated_at = excluded.updated_at`,
      )
      .bind(
        input.accountId,
        input.zoneId,
        input.baseDomain,
        input.accessTokenCiphertext,
        input.refreshTokenCiphertext ?? null,
        input.expiresAt ?? null,
        input.scopes ?? null,
        connectedAt,
        now,
      )
      .run();
    return { ...input, connectedAt, updatedAt: now };
  }

  async deleteConnection(): Promise<void> {
    await this.db.prepare("DELETE FROM cloudflare_oauth_connections WHERE id = 'default'").run();
  }

  async createState(input: Omit<CloudflareOAuthState, 'createdAt'>): Promise<CloudflareOAuthState> {
    const createdAt = new Date().toISOString();
    await this.db
      .prepare(
        `INSERT INTO cloudflare_oauth_states (
           state, account_id, zone_id, base_domain, expires_at, created_at
         ) VALUES (?, ?, ?, ?, ?, ?)`,
      )
      .bind(
        input.state,
        input.accountId,
        input.zoneId,
        input.baseDomain,
        input.expiresAt,
        createdAt,
      )
      .run();
    return { ...input, createdAt };
  }

  async consumeState(state: string): Promise<CloudflareOAuthState | undefined> {
    const row = await this.db
      .prepare('SELECT * FROM cloudflare_oauth_states WHERE state = ?')
      .bind(state)
      .first<CloudflareOAuthStateRow>();
    if (!row) return undefined;
    await this.db.prepare('DELETE FROM cloudflare_oauth_states WHERE state = ?').bind(state).run();
    return toState(row);
  }

  async removeExpiredStates(): Promise<void> {
    await this.db
      .prepare('DELETE FROM cloudflare_oauth_states WHERE expires_at <= ?')
      .bind(new Date().toISOString())
      .run();
  }
}

function toConnection(row: CloudflareOAuthConnectionRow): CloudflareOAuthConnection {
  return {
    accountId: row.account_id,
    zoneId: row.zone_id,
    baseDomain: row.base_domain,
    accessTokenCiphertext: row.access_token_ciphertext,
    ...(row.refresh_token_ciphertext
      ? { refreshTokenCiphertext: row.refresh_token_ciphertext }
      : {}),
    ...(row.expires_at ? { expiresAt: row.expires_at } : {}),
    ...(row.scopes ? { scopes: row.scopes } : {}),
    connectedAt: row.connected_at,
    updatedAt: row.updated_at,
  };
}

function toState(row: CloudflareOAuthStateRow): CloudflareOAuthState {
  return {
    state: row.state,
    accountId: row.account_id,
    zoneId: row.zone_id,
    baseDomain: row.base_domain,
    expiresAt: row.expires_at,
    createdAt: row.created_at,
  };
}
