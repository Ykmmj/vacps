import { AppError } from '../lib/http.js';
import {
  normalizeBaseDomain,
  type CloudflareOAuthService,
  type CloudflareTunnelCredentials,
} from '../cloudflare/oauth-service.js';
import type { ManagedTunnelRepository } from '../registry/managed-tunnel-repository.js';

/** Stable Managed Tunnel / node IDs look like vacps-715f765653e6. */
export const MANAGED_TUNNEL_ID_PATTERN = /^vacps-[a-f0-9]{12}$/;
/** Legacy IDs created before the vacps- rename. */
export const LEGACY_MANAGED_TUNNEL_ID_PATTERN = /^vps-[a-f0-9]{12}$/;

interface ProvisionInput {
  name?: string | undefined;
}

interface AttachInput {
  tunnelId: string;
  backendId?: string | undefined;
}

interface ProvisionedTunnel {
  backendId: string;
  hostname: string;
  publicUrl: string;
  tunnelToken: string;
  tunnelId: string;
  reused: boolean;
}

export interface AvailableManagedTunnel {
  tunnelId: string;
  name: string;
  backendId?: string;
  hostname?: string;
  publicUrl?: string;
  bound: boolean;
  boundBackendId?: string;
  deleted: boolean;
}

interface CloudflareResponse<T> {
  success: boolean;
  errors?: Array<{ message?: string; code?: number }>;
  result?: T;
  result_info?: { total_count?: number; count?: number; page?: number; per_page?: number };
}

interface CloudflareTunnel {
  id: string;
  name: string;
  deleted_at?: string | null;
  status?: string;
}

interface CreatedTunnel {
  id: string;
  name?: string;
  token?: string;
}

interface CreatedDnsRecord {
  id: string;
}

interface DnsRecord {
  id: string;
  name: string;
  type: string;
  content: string;
}

export class ManagedTunnelService {
  constructor(
    private readonly cloudflare: CloudflareOAuthService,
    private readonly repository: ManagedTunnelRepository,
  ) {}

  async provision(input: ProvisionInput): Promise<ProvisionedTunnel> {
    const configuration = await this.configuration();
    const backendId = createManagedBackendId();
    // Cloudflare tunnel names must be unique; the durable ID is the canonical name (vacps-…).
    // Optional input.name is only a UI label on the control plane, not the CF tunnel name.
    void input.name;
    const tunnelName = backendId;
    const hostname = `${backendId}.${configuration.baseDomain}`;
    let tunnelId: string | undefined;
    let dnsRecordId: string | undefined;

    try {
      const tunnel = await this.request<CreatedTunnel>(configuration, '/cfd_tunnel', {
        method: 'POST',
        body: {
          name: tunnelName,
          config_src: 'cloudflare',
        },
      });
      if (!tunnel.id)
        throw new AppError('tunnel_provisioning_failed', 'Tunnel ID was missing.', 502);
      tunnelId = tunnel.id;

      await this.applyIngress(configuration, tunnelId, hostname);
      dnsRecordId = await this.ensureDnsRecord(configuration, hostname, tunnelId);

      const tunnelToken = tunnel.token ?? (await this.tunnelToken(configuration, tunnelId));
      await this.repository.upsert({ backendId, tunnelId, hostname, dnsRecordId });
      return {
        backendId,
        hostname,
        publicUrl: `https://${hostname}`,
        tunnelToken,
        tunnelId,
        reused: false,
      };
    } catch (error) {
      await this.cleanup(configuration, tunnelId, dnsRecordId).catch(() => undefined);
      throw error;
    }
  }

  async attach(input: AttachInput): Promise<ProvisionedTunnel> {
    const configuration = await this.configuration();
    const tunnelId = input.tunnelId.trim();
    if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(tunnelId)) {
      throw new AppError('invalid_request', 'tunnelId must be a Cloudflare tunnel UUID.', 400);
    }

    const tunnel = await this.request<CloudflareTunnel>(configuration, `/cfd_tunnel/${tunnelId}`, {
      method: 'GET',
    });
    if (tunnel.deleted_at)
      throw new AppError('tunnel_deleted', 'That Cloudflare Tunnel has been deleted.', 409);

    const backendId = resolveBackendId(input.backendId, tunnel.name);
    if (!backendId) {
      throw new AppError(
        'invalid_request',
        'Could not derive a node ID from the tunnel name. Pass backendId as vacps-<12 hex chars>.',
        400,
      );
    }

    const existing = await this.repository.find(backendId);
    if (existing && existing.tunnelId !== tunnelId) {
      throw new AppError(
        'managed_tunnel_conflict',
        `Backend '${backendId}' is already bound to a different tunnel.`,
        409,
      );
    }
    const boundElsewhere = await this.repository.findByTunnelId(tunnelId);
    if (boundElsewhere && boundElsewhere.backendId !== backendId) {
      throw new AppError(
        'managed_tunnel_conflict',
        `Tunnel is already bound to backend '${boundElsewhere.backendId}'.`,
        409,
      );
    }

    const hostname = `${backendId}.${configuration.baseDomain}`;
    await this.applyIngress(configuration, tunnelId, hostname);
    const dnsRecordId = await this.ensureDnsRecord(configuration, hostname, tunnelId);
    const tunnelToken = await this.tunnelToken(configuration, tunnelId);
    await this.repository.upsert({ backendId, tunnelId, hostname, dnsRecordId });
    return {
      backendId,
      hostname,
      publicUrl: `https://${hostname}`,
      tunnelToken,
      tunnelId,
      reused: true,
    };
  }

  async listAvailable(): Promise<AvailableManagedTunnel[]> {
    const configuration = await this.configuration();
    const remote = await this.listCloudflareTunnels(configuration);
    const local = await this.repository.list();
    const localByTunnelId = new Map(local.map((row) => [row.tunnelId, row]));
    const localByBackendId = new Map(local.map((row) => [row.backendId, row]));

    const items: AvailableManagedTunnel[] = remote.map((tunnel) => {
      const parsedBackendId = parseBackendIdFromTunnelName(tunnel.name);
      const bound =
        localByTunnelId.get(tunnel.id) ??
        (parsedBackendId ? localByBackendId.get(parsedBackendId) : undefined);
      const backendId = bound?.backendId ?? parsedBackendId;
      const hostname =
        bound?.hostname ?? (backendId ? `${backendId}.${configuration.baseDomain}` : undefined);
      return {
        tunnelId: tunnel.id,
        name: tunnel.name,
        ...(backendId ? { backendId } : {}),
        ...(hostname ? { hostname, publicUrl: `https://${hostname}` } : {}),
        bound: Boolean(bound),
        ...(bound ? { boundBackendId: bound.backendId } : {}),
        deleted: Boolean(tunnel.deleted_at),
      };
    });

    // Include D1-only rows whose Cloudflare tunnel is no longer listable.
    for (const row of local) {
      if (items.some((item) => item.tunnelId === row.tunnelId)) continue;
      items.push({
        tunnelId: row.tunnelId,
        name: row.backendId,
        backendId: row.backendId,
        hostname: row.hostname,
        publicUrl: `https://${row.hostname}`,
        bound: true,
        boundBackendId: row.backendId,
        deleted: false,
      });
    }

    return items.sort((left, right) => left.name.localeCompare(right.name));
  }

  async remove(backendId: string): Promise<void> {
    const configuration = await this.configuration().catch(() => undefined);
    const local = await this.repository.find(backendId);

    if (local) {
      if (configuration) {
        await this.request(
          configuration,
          `/zones/${configuration.zoneId}/dns_records/${local.dnsRecordId}`,
          { method: 'DELETE', scope: 'zone' },
        ).catch(() => undefined);
        await this.request(configuration, `/cfd_tunnel/${local.tunnelId}`, {
          method: 'DELETE',
        }).catch(() => undefined);
      }
      await this.repository.delete(backendId).catch(() => undefined);
      return;
    }

    // Stale D1 node without managed_tunnels metadata: still try to remove a matching CF tunnel.
    if (!configuration) return;
    const tunnels = await this.listCloudflareTunnels(configuration).catch(() => []);
    const match = tunnels.find((tunnel) => {
      if (tunnel.deleted_at) return false;
      const parsed = parseBackendIdFromTunnelName(tunnel.name);
      return tunnel.name === backendId || parsed === backendId;
    });
    if (!match) return;

    const hostname = `${backendId}.${configuration.baseDomain}`;
    const dns = await this.findDnsRecord(configuration, hostname).catch(() => undefined);
    if (dns)
      await this.request(configuration, `/zones/${configuration.zoneId}/dns_records/${dns.id}`, {
        method: 'DELETE',
        scope: 'zone',
      }).catch(() => undefined);
    await this.request(configuration, `/cfd_tunnel/${match.id}`, { method: 'DELETE' }).catch(
      () => undefined,
    );
  }

  private async applyIngress(
    configuration: CloudflareTunnelCredentials,
    tunnelId: string,
    hostname: string,
  ): Promise<void> {
    await this.request(configuration, `/cfd_tunnel/${tunnelId}/configurations`, {
      method: 'PUT',
      body: {
        config: {
          ingress: [
            { hostname, service: 'http://127.0.0.1:3100', originRequest: {} },
            { service: 'http_status:404' },
          ],
        },
      },
    });
  }

  private async ensureDnsRecord(
    configuration: CloudflareTunnelCredentials,
    hostname: string,
    tunnelId: string,
  ): Promise<string> {
    const content = `${tunnelId}.cfargotunnel.com`;
    const existing = await this.findDnsRecord(configuration, hostname);
    if (existing) {
      if (existing.type === 'CNAME' && existing.content === content) return existing.id;
      await this.request(
        configuration,
        `/zones/${configuration.zoneId}/dns_records/${existing.id}`,
        {
          method: 'PUT',
          scope: 'zone',
          body: { type: 'CNAME', proxied: true, name: hostname, content },
        },
      );
      return existing.id;
    }

    const dns = await this.request<CreatedDnsRecord>(
      configuration,
      `/zones/${configuration.zoneId}/dns_records`,
      {
        method: 'POST',
        scope: 'zone',
        body: { type: 'CNAME', proxied: true, name: hostname, content },
      },
    );
    if (!dns.id)
      throw new AppError('tunnel_provisioning_failed', 'DNS record ID was missing.', 502);
    return dns.id;
  }

  private async findDnsRecord(
    configuration: CloudflareTunnelCredentials,
    hostname: string,
  ): Promise<DnsRecord | undefined> {
    const records = await this.request<DnsRecord[]>(
      configuration,
      `/zones/${configuration.zoneId}/dns_records?type=CNAME&name=${encodeURIComponent(hostname)}`,
      { method: 'GET', scope: 'zone' },
    );
    return Array.isArray(records) ? records[0] : undefined;
  }

  private async listCloudflareTunnels(
    configuration: CloudflareTunnelCredentials,
  ): Promise<CloudflareTunnel[]> {
    const perPage = 50;
    let page = 1;
    const tunnels: CloudflareTunnel[] = [];
    for (;;) {
      const batch = await this.requestList<CloudflareTunnel>(
        configuration,
        `/cfd_tunnel?is_deleted=false&per_page=${perPage}&page=${page}`,
      );
      tunnels.push(...batch);
      if (batch.length < perPage) break;
      page += 1;
      if (page > 20) break;
    }
    return tunnels;
  }

  private async configuration(): Promise<CloudflareTunnelCredentials> {
    const configuration = await this.cloudflare.credentials();
    return { ...configuration, baseDomain: normalizeBaseDomain(configuration.baseDomain) };
  }

  private async tunnelToken(
    configuration: CloudflareTunnelCredentials,
    tunnelId: string,
  ): Promise<string> {
    const response = await this.request<string>(configuration, `/cfd_tunnel/${tunnelId}/token`, {
      method: 'GET',
    });
    if (!response)
      throw new AppError('tunnel_provisioning_failed', 'Tunnel token was missing.', 502);
    return response;
  }

  private async cleanup(
    configuration: CloudflareTunnelCredentials,
    tunnelId?: string,
    dnsRecordId?: string,
  ): Promise<void> {
    if (!tunnelId) return;
    if (dnsRecordId)
      await this.request(
        configuration,
        `/zones/${configuration.zoneId}/dns_records/${dnsRecordId}`,
        {
          method: 'DELETE',
          scope: 'zone',
        },
      );
    await this.request(configuration, `/cfd_tunnel/${tunnelId}`, { method: 'DELETE' });
  }

  private async requestList<T>(
    configuration: CloudflareTunnelCredentials,
    path: string,
  ): Promise<T[]> {
    const target = `https://api.cloudflare.com/client/v4/accounts/${configuration.accountId}${path}`;
    const response = await fetch(target, {
      method: 'GET',
      headers: { authorization: `Bearer ${configuration.accessToken}` },
    });
    const payload = (await response.json().catch(() => undefined)) as
      CloudflareResponse<T[]> | undefined;
    if (!response.ok || !payload?.success || !Array.isArray(payload.result)) {
      const detail = payload?.errors
        ?.map((error) => error.message)
        .filter(Boolean)
        .join('; ');
      throw new AppError(
        'cloudflare_api_failed',
        detail || `Cloudflare Tunnel API returned HTTP ${response.status}.`,
        502,
      );
    }
    return payload.result;
  }

  private async request<T>(
    configuration: CloudflareTunnelCredentials,
    path: string,
    input: { method: 'GET' | 'POST' | 'PUT' | 'DELETE'; body?: unknown; scope?: 'zone' } = {
      method: 'POST',
    },
  ): Promise<T> {
    const target =
      input.scope === 'zone'
        ? `https://api.cloudflare.com/client/v4${path}`
        : `https://api.cloudflare.com/client/v4/accounts/${configuration.accountId}${path}`;
    const response = await fetch(target, {
      method: input.method,
      headers: {
        authorization: `Bearer ${configuration.accessToken}`,
        ...(input.body ? { 'content-type': 'application/json' } : {}),
      },
      ...(input.body ? { body: JSON.stringify(input.body) } : {}),
    });
    const payload = (await response.json().catch(() => undefined)) as
      CloudflareResponse<T> | undefined;
    // DELETE may return result: null on success.
    if (
      !response.ok ||
      !payload?.success ||
      (input.method !== 'DELETE' && payload.result === undefined)
    ) {
      const detail = payload?.errors
        ?.map((error) => error.message)
        .filter(Boolean)
        .join('; ');
      throw new AppError(
        'cloudflare_api_failed',
        detail || `Cloudflare Tunnel API returned HTTP ${response.status}.`,
        502,
      );
    }
    return payload.result as T;
  }
}

export function createManagedBackendId(): string {
  return `vacps-${crypto.randomUUID().replaceAll('-', '').slice(0, 12)}`;
}

export function isManagedBackendId(value: string): boolean {
  return MANAGED_TUNNEL_ID_PATTERN.test(value) || LEGACY_MANAGED_TUNNEL_ID_PATTERN.test(value);
}

export function parseBackendIdFromTunnelName(name: string): string | undefined {
  const trimmed = name.trim();
  if (isManagedBackendId(trimmed)) return trimmed;
  // Legacy display names: "VACPS vps-715f765653e6"
  const embedded = trimmed.match(/\b((?:vacps|vps)-[a-f0-9]{12})\b/i);
  if (embedded?.[1]) return embedded[1].toLowerCase();
  return undefined;
}

function resolveBackendId(explicit: string | undefined, tunnelName: string): string | undefined {
  if (explicit?.trim()) {
    const value = explicit.trim().toLowerCase();
    if (!isManagedBackendId(value)) {
      throw new AppError(
        'invalid_request',
        'backendId must match vacps-<12 hex characters> (or legacy vps-…).',
        400,
      );
    }
    return value;
  }
  return parseBackendIdFromTunnelName(tunnelName);
}
