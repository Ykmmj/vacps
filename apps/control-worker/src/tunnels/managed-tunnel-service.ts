import { AppError } from '../lib/http.js';
import {
  normalizeBaseDomain,
  type CloudflareOAuthService,
  type CloudflareTunnelCredentials,
} from '../cloudflare/oauth-service.js';
import type { ManagedTunnelRepository } from '../registry/managed-tunnel-repository.js';

interface ProvisionInput {
  name?: string | undefined;
}

interface ProvisionedTunnel {
  backendId: string;
  hostname: string;
  publicUrl: string;
  tunnelToken: string;
}

interface CloudflareResponse<T> {
  success: boolean;
  errors?: Array<{ message?: string; code?: number }>;
  result?: T;
}

interface CreatedTunnel {
  id: string;
  token?: string;
}

interface CreatedDnsRecord {
  id: string;
}

export class ManagedTunnelService {
  constructor(
    private readonly cloudflare: CloudflareOAuthService,
    private readonly repository: ManagedTunnelRepository,
  ) {}

  async provision(input: ProvisionInput): Promise<ProvisionedTunnel> {
    const configuration = await this.configuration();
    const backendId = `vps-${crypto.randomUUID().replaceAll('-', '').slice(0, 12)}`;
    const hostname = `${backendId}.${configuration.baseDomain}`;
    let tunnelId: string | undefined;
    let dnsRecordId: string | undefined;

    try {
      const tunnel = await this.request<CreatedTunnel>(configuration, '/cfd_tunnel', {
        method: 'POST',
        body: {
          name: input.name?.trim() || `VPS Agent ${backendId}`,
          config_src: 'cloudflare',
        },
      });
      if (!tunnel.id)
        throw new AppError('tunnel_provisioning_failed', 'Tunnel ID was missing.', 502);
      tunnelId = tunnel.id;

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

      const dns = await this.request<CreatedDnsRecord>(
        configuration,
        `/zones/${configuration.zoneId}/dns_records`,
        {
          method: 'POST',
          scope: 'zone',
          body: {
            type: 'CNAME',
            proxied: true,
            name: hostname,
            content: `${tunnelId}.cfargotunnel.com`,
          },
        },
      );
      if (!dns.id)
        throw new AppError('tunnel_provisioning_failed', 'DNS record ID was missing.', 502);
      dnsRecordId = dns.id;

      const tunnelToken = tunnel.token ?? (await this.tunnelToken(configuration, tunnelId));
      await this.repository.create({ backendId, tunnelId, hostname, dnsRecordId });
      return { backendId, hostname, publicUrl: `https://${hostname}`, tunnelToken };
    } catch (error) {
      await this.cleanup(configuration, tunnelId, dnsRecordId).catch(() => undefined);
      throw error;
    }
  }

  async remove(backendId: string): Promise<void> {
    const tunnel = await this.repository.find(backendId);
    if (!tunnel) return;
    const configuration = await this.configuration();
    await this.request(
      configuration,
      `/zones/${configuration.zoneId}/dns_records/${tunnel.dnsRecordId}`,
      {
        method: 'DELETE',
        scope: 'zone',
      },
    );
    await this.request(configuration, `/cfd_tunnel/${tunnel.tunnelId}`, { method: 'DELETE' });
    await this.repository.delete(backendId);
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
    if (!response.ok || !payload?.success || payload.result === undefined) {
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
}
