import { AppError } from '../lib/http.js';
import { base64UrlEncode, sha256Base64Url } from '../security/request-signatures.js';

const REGISTRATION_TOKEN_TTL_MS = 10 * 60 * 1000;

export interface RegistrationToken {
  token: string;
  expiresAt: string;
}

/**
 * One-time bootstrap capabilities for Agent identity enrollment. The returned secret is never
 * persisted: D1 stores only SHA-256(token), allowing a database read to remain non-sensitive.
 */
export class RegistrationTokenRepository {
  constructor(private readonly db: D1Database) {}

  async issue(now = Date.now()): Promise<RegistrationToken> {
    const token = base64UrlEncode(crypto.getRandomValues(new Uint8Array(32)));
    const expiresAt = new Date(now + REGISTRATION_TOKEN_TTL_MS).toISOString();
    await this.db
      .prepare(
        `INSERT INTO registration_tokens (id, token_hash, expires_at, created_at)
         VALUES (?, ?, ?, ?)`,
      )
      .bind(
        crypto.randomUUID(),
        await sha256Base64Url(token),
        expiresAt,
        new Date(now).toISOString(),
      )
      .run();
    return { token, expiresAt };
  }

  /** Atomically claims a token. A retry or a parallel replay can never claim it twice. */
  async consume(token: string): Promise<void> {
    if (!/^[A-Za-z0-9_-]{43}$/.test(token))
      throw new AppError(
        'invalid_registration_token',
        'Registration token is invalid or expired.',
        401,
      );
    const now = new Date().toISOString();
    const result = await this.db
      .prepare(
        `UPDATE registration_tokens
         SET consumed_at = ?
         WHERE token_hash = ? AND consumed_at IS NULL AND expires_at > ?`,
      )
      .bind(now, await sha256Base64Url(token), now)
      .run();
    if ((result.meta.changes ?? 0) !== 1)
      throw new AppError(
        'invalid_registration_token',
        'Registration token is invalid or expired.',
        401,
      );
  }

  async purgeExpired(): Promise<void> {
    await this.db
      .prepare('DELETE FROM registration_tokens WHERE expires_at <= ?')
      .bind(new Date().toISOString())
      .run();
  }
}
