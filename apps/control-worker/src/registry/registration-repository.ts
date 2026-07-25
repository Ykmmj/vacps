import type {
  BackendRegistration,
  RegisterBackendInput,
  RegistrationStatus,
} from '@vps-agent/contracts';

import { AppError } from '../lib/http.js';

interface RegistrationRow {
  id: string;
  backend_id: string;
  name: string;
  base_url: string;
  tags_json: string;
  agent_version: string;
  status: RegistrationStatus;
  rejection_reason: string | null;
  requested_at: string;
  updated_at: string;
  decision_at: string | null;
  ip: string | null;
  location: string | null;
}

interface RegistrationNetworkDetails {
  ip?: string;
  location?: string;
}

export class RegistrationRepository {
  constructor(private readonly db: D1Database) {}

  async list(status?: RegistrationStatus): Promise<BackendRegistration[]> {
    const statement = status
      ? this.db
          .prepare(
            'SELECT * FROM backend_registrations WHERE status = ? ORDER BY requested_at DESC',
          )
          .bind(status)
      : this.db.prepare('SELECT * FROM backend_registrations ORDER BY requested_at DESC');
    const result = await statement.all<RegistrationRow>();
    return result.results.map(toRegistration);
  }

  async get(id: string): Promise<BackendRegistration> {
    const row = await this.db
      .prepare('SELECT * FROM backend_registrations WHERE id = ?')
      .bind(id)
      .first<RegistrationRow>();
    if (!row)
      throw new AppError('registration_not_found', `Registration '${id}' was not found.`, 404);
    return toRegistration(row);
  }

  async request(
    input: RegisterBackendInput,
    network: RegistrationNetworkDetails = {},
  ): Promise<BackendRegistration> {
    const now = new Date().toISOString();
    const existing = await this.db
      .prepare('SELECT * FROM backend_registrations WHERE backend_id = ?')
      .bind(input.backendId)
      .first<RegistrationRow>();

    if (!existing) {
      const id = crypto.randomUUID();
      await this.db
        .prepare(
          `INSERT INTO backend_registrations
           (id, backend_id, name, base_url, tags_json, agent_version, status, requested_at, updated_at, ip, location)
           VALUES (?, ?, ?, ?, ?, ?, 'pending', ?, ?, ?, ?)`,
        )
        .bind(
          id,
          input.backendId,
          input.name,
          input.baseUrl,
          JSON.stringify(input.tags),
          input.agentVersion,
          now,
          now,
          network.ip ?? null,
          network.location ?? null,
        )
        .run();
      return this.get(id);
    }

    if (existing.status === 'approved') {
      await this.db
        .prepare(
          `UPDATE backend_registrations
           SET name = ?, base_url = ?, tags_json = ?, agent_version = ?, updated_at = ?, ip = ?, location = ?
           WHERE id = ?`,
        )
        .bind(
          input.name,
          input.baseUrl,
          JSON.stringify(input.tags),
          input.agentVersion,
          now,
          network.ip ?? null,
          network.location ?? null,
          existing.id,
        )
        .run();
      return this.get(existing.id);
    }

    await this.db
      .prepare(
        `UPDATE backend_registrations
         SET name = ?, base_url = ?, tags_json = ?, agent_version = ?, status = 'pending',
             rejection_reason = NULL, requested_at = ?, updated_at = ?, decision_at = NULL, ip = ?, location = ?
         WHERE id = ?`,
      )
      .bind(
        input.name,
        input.baseUrl,
        JSON.stringify(input.tags),
        input.agentVersion,
        now,
        now,
        network.ip ?? null,
        network.location ?? null,
        existing.id,
      )
      .run();
    return this.get(existing.id);
  }

  async approve(id: string): Promise<BackendRegistration> {
    const now = new Date().toISOString();
    await this.db
      .prepare(
        `UPDATE backend_registrations
         SET status = 'approved', rejection_reason = NULL, decision_at = ?, updated_at = ? WHERE id = ?`,
      )
      .bind(now, now, id)
      .run();
    return this.get(id);
  }

  async reject(id: string, reason?: string): Promise<BackendRegistration> {
    const now = new Date().toISOString();
    await this.db
      .prepare(
        `UPDATE backend_registrations
         SET status = 'rejected', rejection_reason = ?, decision_at = ?, updated_at = ? WHERE id = ?`,
      )
      .bind(reason ?? null, now, now, id)
      .run();
    return this.get(id);
  }
}

function toRegistration(row: RegistrationRow): BackendRegistration {
  return {
    id: row.id,
    backendId: row.backend_id,
    name: row.name,
    baseUrl: row.base_url,
    tags: JSON.parse(row.tags_json) as string[],
    agentVersion: row.agent_version,
    status: row.status,
    requestedAt: row.requested_at,
    updatedAt: row.updated_at,
    ...(row.decision_at ? { decisionAt: row.decision_at } : {}),
    ...(row.rejection_reason ? { rejectionReason: row.rejection_reason } : {}),
    ...(row.ip ? { ip: row.ip } : {}),
    ...(row.location ? { location: row.location } : {}),
  };
}
