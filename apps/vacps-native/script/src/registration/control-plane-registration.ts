import {
  backendTelemetrySchema,
  registerBackendSchema,
  telemetrySettingsSchema,
  type BackendStatus,
} from '@vacps/contracts';
import * as host from 'vacps:host';
import * as http from 'vacps:http';
import * as log from 'vacps:log';

import type { AgentConfig } from '../config';
import { registrationConfigured, telemetryConfigured } from '../config';
import { createAgentSignatureHeaders } from '../security/request-signatures';

export type RegistrationStatus = 'pending' | 'approved' | 'rejected' | 'unknown' | 'disabled';

function bodyText(body: ArrayBuffer): string {
  return new TextDecoder('utf-8').decode(new Uint8Array(body));
}

/** POST /api/registrations — parity with apps/vacps registration module. */
export async function registerWithControlPlane(config: AgentConfig): Promise<string | undefined> {
  // Skip until PUBLIC_BASE_URL is a real absolute URL (managed --public-url or quick-tunnel).
  if (!registrationConfigured(config) || !config.PUBLIC_BASE_URL) {
    log.info(
      'registration skipped: waiting for PUBLIC_BASE_URL (managed --public-url or quick-tunnel)',
    );
    return undefined;
  }

  const payload = registerBackendSchema.parse({
    backendId: config.BACKEND_ID,
    name: config.BACKEND_NAME,
    baseUrl: config.PUBLIC_BASE_URL,
    tags: config.BACKEND_TAGS,
    publicIps: [],
    agentVersion: host.version().slice(0, 48) || '0.1.0',
    publicKey: config.AGENT_PUBLIC_KEY!,
  });
  const body = JSON.stringify(payload);
  const url = `${config.CONTROL_PLANE_URL}/api/registrations`;
  const sig = createAgentSignatureHeaders(
    config.BACKEND_ID,
    config.AGENT_PRIVATE_KEY!,
    'POST',
    url,
    body,
  );
  const headers: Record<string, string> = {
    'content-type': 'application/json',
    ...sig,
  };
  if (config.REGISTRATION_TOKEN) {
    headers.authorization = `Bearer ${config.REGISTRATION_TOKEN}`;
  }

  const res = await http.request({
    method: 'POST',
    url,
    headers,
    body,
    timeoutMs: 30_000,
  });
  const text = bodyText(res.body);
  if (res.status < 200 || res.status >= 300) {
    throw new Error(
      `Control-plane registration returned HTTP ${res.status}${text ? `: ${text.slice(0, 200)}` : ''}`,
    );
  }
  try {
    const json = JSON.parse(text) as { status?: unknown };
    if (typeof json.status === 'string') {
      log.info(`Control-plane registration status: ${json.status}`);
      return json.status;
    }
  } catch {
    /* ignore */
  }
  return undefined;
}

export async function reportTelemetry(
  config: AgentConfig,
  status: BackendStatus,
): Promise<number | undefined> {
  if (!telemetryConfigured(config)) return undefined;

  const telemetry = backendTelemetrySchema.parse({
    backendId: config.BACKEND_ID,
    agentVersion: status.health.version,
    observedAt: new Date(host.nowMs()).toISOString(),
    ...status,
  });
  const body = JSON.stringify(telemetry);
  const url = `${config.CONTROL_PLANE_URL}/api/telemetry`;
  const sig = createAgentSignatureHeaders(
    config.BACKEND_ID,
    config.AGENT_PRIVATE_KEY!,
    'POST',
    url,
    body,
  );
  const res = await http.request({
    method: 'POST',
    url,
    headers: {
      'content-type': 'application/json',
      ...sig,
    },
    body,
    timeoutMs: 30_000,
  });
  const text = bodyText(res.body);
  if (res.status < 200 || res.status >= 300) {
    throw new Error(`Control-plane telemetry returned HTTP ${res.status}: ${text.slice(0, 200)}`);
  }
  try {
    const parsed = telemetrySettingsSchema.safeParse(JSON.parse(text));
    if (parsed.success) return parsed.data.intervalSeconds;
  } catch {
    /* ignore */
  }
  return undefined;
}

/** @deprecated Prefer NativeTelemetryCollector.collect() — kept for tests. */
export function collectNativeStatus(config: AgentConfig): BackendStatus {
  const version = host.version().slice(0, 48) || '0.1.0';
  return {
    health: {
      ok: true,
      backendId: config.BACKEND_ID,
      version,
      uptimeSeconds: 0,
      worker: { running: true, concurrency: 1 },
      redis: { connected: false },
      pi: { available: false },
    },
    system: {
      platform: 'linux',
      kernel: 'unknown',
      architecture: 'x86_64',
    },
  };
}
