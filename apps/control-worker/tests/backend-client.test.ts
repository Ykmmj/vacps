import { webcrypto } from 'node:crypto';

import { afterEach, describe, expect, it, vi } from 'vitest';

import { BackendClient } from '../src/registry/backend-client.js';

async function controlPlanePrivateKey(): Promise<string> {
  const pair = await webcrypto.subtle.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']);
  return Buffer.from(await webcrypto.subtle.exportKey('pkcs8', pair.privateKey)).toString(
    'base64url',
  );
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('BackendClient', () => {
  it('signs registration-shaped targets for the Agent backend ID, not the registration row ID', async () => {
    const headers: Headers[] = [];
    const fetchMock = vi.fn(
      async (_input: Parameters<typeof fetch>[0], init?: Parameters<typeof fetch>[1]) => {
        headers.push(new Headers(init?.headers));
        return Response.json({ status: 'healthy' });
      },
    );
    vi.stubGlobal('fetch', fetchMock);

    const registration = {
      id: 'registration-row-id',
      backendId: 'vacps-agent-id',
      baseUrl: 'https://agent.example',
    };

    await new BackendClient(await controlPlanePrivateKey()).health(registration);

    expect(fetchMock).toHaveBeenCalledOnce();
    expect(headers[0]?.get('x-vps-control-backend-id')).toBe('vacps-agent-id');
  });
});
