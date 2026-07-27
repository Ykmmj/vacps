import { AppError } from '../lib/http.js';

export interface ManagedTunnel {
  backendId: string;
  tunnelId: string;
  hostname: string;
  dnsRecordId: string;
  createdAt: string;
}

interface ManagedTunnelRow {
  backend_id: string;
  tunnel_id: string;
  hostname: string;
  dns_record_id: string;
  created_at: string;
}

export class ManagedTunnelRepository {
  constructor(private readonly db: D1Database) {}

  async create(input: Omit<ManagedTunnel, 'createdAt'>): Promise<ManagedTunnel> {
    const createdAt = new Date().toISOString();
    await this.db
      .prepare(
        `INSERT INTO managed_tunnels (backend_id, tunnel_id, hostname, dns_record_id, created_at)
         VALUES (?, ?, ?, ?, ?)`,
      )
      .bind(input.backendId, input.tunnelId, input.hostname, input.dnsRecordId, createdAt)
      .run();
    return { ...input, createdAt };
  }

  async upsert(input: Omit<ManagedTunnel, 'createdAt'>): Promise<ManagedTunnel> {
    const existing = await this.find(input.backendId);
    if (existing) {
      await this.db
        .prepare(
          `UPDATE managed_tunnels
           SET tunnel_id = ?, hostname = ?, dns_record_id = ?
           WHERE backend_id = ?`,
        )
        .bind(input.tunnelId, input.hostname, input.dnsRecordId, input.backendId)
        .run();
      return { ...input, createdAt: existing.createdAt };
    }
    return this.create(input);
  }

  async list(): Promise<ManagedTunnel[]> {
    const result = await this.db
      .prepare('SELECT * FROM managed_tunnels ORDER BY created_at DESC')
      .all<ManagedTunnelRow>();
    return result.results.map(toManagedTunnel);
  }

  async find(backendId: string): Promise<ManagedTunnel | undefined> {
    const row = await this.db
      .prepare('SELECT * FROM managed_tunnels WHERE backend_id = ?')
      .bind(backendId)
      .first<ManagedTunnelRow>();
    return row ? toManagedTunnel(row) : undefined;
  }

  async findByTunnelId(tunnelId: string): Promise<ManagedTunnel | undefined> {
    const row = await this.db
      .prepare('SELECT * FROM managed_tunnels WHERE tunnel_id = ?')
      .bind(tunnelId)
      .first<ManagedTunnelRow>();
    return row ? toManagedTunnel(row) : undefined;
  }

  async delete(backendId: string): Promise<void> {
    const result = await this.db
      .prepare('DELETE FROM managed_tunnels WHERE backend_id = ?')
      .bind(backendId)
      .run();
    if ((result.meta.changes ?? 0) === 0)
      throw new AppError('managed_tunnel_not_found', 'Managed tunnel metadata was not found.', 404);
  }
}

function toManagedTunnel(row: ManagedTunnelRow): ManagedTunnel {
  return {
    backendId: row.backend_id,
    tunnelId: row.tunnel_id,
    hostname: row.hostname,
    dnsRecordId: row.dns_record_id,
    createdAt: row.created_at,
  };
}
