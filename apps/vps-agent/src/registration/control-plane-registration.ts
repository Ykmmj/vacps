import type { AgentConfig } from '../config.js';

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
      ...(config.BACKEND_REGION ? { region: config.BACKEND_REGION } : {}),
      tags: config.BACKEND_TAGS.split(',')
        .map((tag) => tag.trim())
        .filter(Boolean),
      agentVersion: '0.1.0',
    }),
  });
  const body = (await response.json().catch(() => undefined)) as { status?: unknown } | undefined;
  if (!response.ok) throw new Error(`Control-plane registration returned HTTP ${response.status}.`);
  return typeof body?.status === 'string' ? body.status : undefined;
}
