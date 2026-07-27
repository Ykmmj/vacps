import { AppError } from '../lib/http.js';

/** D1-backed nonce cache: unlike eventually consistent KV, this makes Agent request replay checks global. */
export class AgentSignatureRepository {
  constructor(private readonly db: D1Database) {}

  async claimNonce(backendId: string, nonce: string): Promise<void> {
    try {
      await this.db
        .prepare(
          `INSERT INTO agent_request_nonces (backend_id, nonce, expires_at)
           VALUES (?, ?, ?)`,
        )
        .bind(backendId, nonce, new Date(Date.now() + 5 * 60 * 1000).toISOString())
        .run();
    } catch {
      throw new AppError('replayed_agent_request', 'Agent request has already been used.', 401);
    }
  }

  async purgeExpired(): Promise<void> {
    await this.db
      .prepare('DELETE FROM agent_request_nonces WHERE expires_at <= ?')
      .bind(new Date().toISOString())
      .run();
  }

  async purgeBackend(backendId: string): Promise<void> {
    await this.db
      .prepare('DELETE FROM agent_request_nonces WHERE backend_id = ?')
      .bind(backendId)
      .run();
  }
}
