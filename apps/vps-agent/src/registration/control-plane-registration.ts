import {
  backendTelemetrySchema,
  telemetrySettingsSchema,
  type BackendStatus,
} from '@vps-agent/contracts';

import type { AgentConfig } from '../config.js';
import { publicInterfaceAddresses } from '../network/public-interface-addresses.js';

export async function registerWithControlPlane(config: AgentConfig): Promise<string | undefined> {
  if (!config.CONTROL_PLANE_URL || !config.PUBLIC_BASE_URL) return undefined;
  const response = await fetch(`${config.CONTROL_PLANE_URL}/api/registrations`, {
    method: 'POST',
    headers: {
      authorization: `Bearer ${config.BACKEND_SHARED_TOKEN}`,
      'content-type': 'application/json',
    },
    body: JSON.stringify({
      backendId: config.BACKEND_ID,
      name: config.BACKEND_NAME ?? config.BACKEND_ID,
      baseUrl: config.PUBLIC_BASE_URL,
      tags: config.BACKEND_TAGS.split(',')
        .map((tag) => tag.trim())
        .filter(Boolean),
      publicIps: publicInterfaceAddresses(),
      agentVersion: '0.1.0',
    }),
  });
  const body = (await response.json().catch(() => undefined)) as { status?: unknown } | undefined;
  if (!response.ok) throw new Error(`Control-plane registration returned HTTP ${response.status}.`);
  return typeof body?.status === 'string' ? body.status : undefined;
}

export async function reportTelemetry(
  config: AgentConfig,
  status: BackendStatus,
): Promise<number | undefined> {
  if (!config.CONTROL_PLANE_URL) return undefined;
  const telemetry = backendTelemetrySchema.parse({
    backendId: config.BACKEND_ID,
    agentVersion: status.health.version,
    observedAt: new Date().toISOString(),
    ...status,
  });
  const response = await fetch(`${config.CONTROL_PLANE_URL}/api/telemetry`, {
    method: 'POST',
    headers: {
      authorization: `Bearer ${config.BACKEND_SHARED_TOKEN}`,
      'content-type': 'application/json',
    },
    body: JSON.stringify(telemetry),
  });
  const body = await response.json().catch(() => undefined);
  if (!response.ok) throw new Error(`Control-plane telemetry returned HTTP ${response.status}.`);
  const parsed = telemetrySettingsSchema.safeParse(body);
  return parsed.success ? parsed.data.intervalSeconds : undefined;
}
