import {
  backendStatusSchema,
  type Backend,
  type BackendStatus,
  type CreateBackendInput,
  type UpdateBackendInput,
} from '@vps-agent/contracts';

import { AppError } from '../lib/http.js';

interface BackendRow {
  id: string;
  name: string;
  base_url: string;
  region: string | null;
  tags_json: string;
  enabled: number;
  last_status: string | null;
  last_checked_at: string | null;
  created_at: string;
  updated_at: string;
}

export class BackendRepository {
  constructor(private readonly db: D1Database) {}

  async list(): Promise<Backend[]> {
    const result = await this.db
      .prepare('SELECT * FROM backends ORDER BY name ASC')
      .all<BackendRow>();
    return result.results.map(toBackend);
  }

  async get(id: string): Promise<Backend> {
    const row = await this.db
      .prepare('SELECT * FROM backends WHERE id = ?')
      .bind(id)
      .first<BackendRow>();
    if (!row) throw new AppError('backend_not_found', `Backend '${id}' was not found.`, 404);
    return toBackend(row);
  }

  async create(input: CreateBackendInput): Promise<Backend> {
    const now = new Date().toISOString();
    try {
      await this.db
        .prepare(
          `INSERT INTO backends (id, name, base_url, region, tags_json, enabled, created_at, updated_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
        )
        .bind(
          input.id,
          input.name,
          input.baseUrl,
          null,
          JSON.stringify(input.tags),
          Number(input.enabled),
          now,
          now,
        )
        .run();
    } catch {
      throw new AppError('backend_exists', `Backend '${input.id}' already exists.`, 409);
    }
    return this.get(input.id);
  }

  async update(id: string, input: UpdateBackendInput): Promise<Backend> {
    const current = await this.get(id);
    const next = { ...current, ...input, updatedAt: new Date().toISOString() };
    await this.db
      .prepare(
        `UPDATE backends SET name = ?, base_url = ?, region = ?, tags_json = ?, enabled = ?, updated_at = ?
         WHERE id = ?`,
      )
      .bind(
        next.name,
        next.baseUrl,
        null,
        JSON.stringify(next.tags),
        Number(next.enabled),
        next.updatedAt,
        id,
      )
      .run();
    return this.get(id);
  }

  async delete(id: string): Promise<void> {
    const response = await this.db.prepare('DELETE FROM backends WHERE id = ?').bind(id).run();
    if ((response.meta.changes ?? 0) === 0)
      throw new AppError('backend_not_found', `Backend '${id}' was not found.`, 404);
  }

  async recordStatus(id: string, status: BackendStatus): Promise<void> {
    await this.db
      .prepare(
        'UPDATE backends SET last_status = ?, last_checked_at = ?, updated_at = ? WHERE id = ?',
      )
      .bind(JSON.stringify(status), new Date().toISOString(), new Date().toISOString(), id)
      .run();
  }
}

function toBackend(row: BackendRow): Backend {
  const status = parseStatus(row.last_status);
  return {
    id: row.id,
    name: row.name,
    baseUrl: row.base_url,
    tags: JSON.parse(row.tags_json) as string[],
    enabled: Boolean(row.enabled),
    createdAt: row.created_at,
    updatedAt: row.updated_at,
    ...(status ? { lastStatus: status } : {}),
    ...(row.last_checked_at ? { lastCheckedAt: row.last_checked_at } : {}),
  };
}

function parseStatus(value: string | null): BackendStatus | undefined {
  if (!value) return undefined;
  try {
    const parsed = backendStatusSchema.safeParse(JSON.parse(value));
    return parsed.success ? parsed.data : undefined;
  } catch {
    return undefined;
  }
}
