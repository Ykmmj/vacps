import {
  backendTelemetrySchema,
  telemetrySettingsSchema,
  type BackendStatus,
} from '@vps-agent/contracts';

import type { AgentConfig } from '../config.js';
import { publicInterfaceAddresses } from '../network/public-interface-addresses.js';
import { createAgentSignatureHeaders } from '../security/request-signatures.js';

export async function registerWithControlPlane(config: AgentConfig): Promise<string | undefined> {
  if (!config.CONTROL_PLANE_URL || !config.PUBLIC_BASE_URL) return undefined;
  const body = JSON.stringify({
    backendId: config.BACKEND_ID,
    name: config.BACKEND_NAME ?? config.BACKEND_ID,
    baseUrl: config.PUBLIC_BASE_URL,
    tags: config.BACKEND_TAGS.split(',')
      .map((tag) => tag.trim())
      .filter(Boolean),
    publicIps: publicInterfaceAddresses(),
    agentVersion: '0.1.0',
    publicKey: config.AGENT_PUBLIC_KEY,
  });
  const request = new Request(`${config.CONTROL_PLANE_URL}/api/registrations`, { method: 'POST' });
  const response = await fetch(request, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      ...(config.REGISTRATION_TOKEN
        ? { authorization: `Bearer ${config.REGISTRATION_TOKEN}` }
        : {}),
      ...(await createAgentSignatureHeaders(config, request, body)),
    },
    body,
  });
  const responseBody = (await response.json().catch(() => undefined)) as
    { status?: unknown } | undefined;
  if (!response.ok) throw new Error(`Control-plane registration returned HTTP ${response.status}.`);
  return typeof responseBody?.status === 'string' ? responseBody.status : undefined;
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
  const body = JSON.stringify(telemetry);
  const request = new Request(`${config.CONTROL_PLANE_URL}/api/telemetry`, { method: 'POST' });
  const response = await fetch(request, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      ...(await createAgentSignatureHeaders(config, request, body)),
    },
    body,
  });
  const responseBody = await response.json().catch(() => undefined);
  if (!response.ok) throw new Error(`Control-plane telemetry returned HTTP ${response.status}.`);
  const parsed = telemetrySettingsSchema.safeParse(responseBody);
  return parsed.success ? parsed.data.intervalSeconds : undefined;
}
