import { webcrypto } from 'node:crypto';

import { describe, expect, it } from 'vitest';

import {
  createAgentSignatureHeaders,
  requestTargetOf,
  verifyControlPlaneRequest,
} from '../../vacps/src/security/request-signatures.js';
import {
  createControlPlaneSignatureHeaders,
  requestTargetOf as workerRequestTargetOf,
  verifyAgentRequestSignature,
} from '../src/security/request-signatures.js';

async function identity() {
  const pair = await webcrypto.subtle.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']);
  return {
    privateKey: Buffer.from(await webcrypto.subtle.exportKey('pkcs8', pair.privateKey)).toString(
      'base64url',
    ),
    publicKey: Buffer.from(await webcrypto.subtle.exportKey('raw', pair.publicKey)).toString(
      'base64url',
    ),
  };
}

describe('Agent request signatures (v2)', () => {
  it('verifies an Agent registration request in the Worker', async () => {
    const agent = await identity();
    const body = JSON.stringify({ backendId: 'test-node', publicKey: agent.publicKey });
    const request = new Request('https://control.example/api/registrations', { method: 'POST' });
    const headers = await createAgentSignatureHeaders(
      { BACKEND_ID: 'test-node', AGENT_PRIVATE_KEY: agent.privateKey },
      request,
      body,
    );
    const signedRequest = new Request(request, { headers, body });

    await expect(
      verifyAgentRequestSignature(signedRequest, agent.publicKey, body),
    ).resolves.toMatchObject({
      backendId: 'test-node',
      nonce: expect.any(String),
    });
    await expect(
      verifyAgentRequestSignature(
        new Request(request, { headers, body: `${body} ` }),
        agent.publicKey,
        `${body} `,
      ),
    ).rejects.toMatchObject({ code: 'invalid_agent_signature' });
  });

  it('verifies a control-plane task request in the Agent', async () => {
    const controlPlane = await identity();
    const body = JSON.stringify({ taskId: 'task-1' });
    const request = new Request('https://agent.example/tasks', { method: 'POST' });
    const headers = await createControlPlaneSignatureHeaders(
      controlPlane.privateKey,
      request,
      body,
      'test-node',
    );

    expect(headers['x-vps-control-backend-id']).toBe('test-node');

    await expect(
      verifyControlPlaneRequest(
        { CONTROL_PLANE_PUBLIC_KEY: controlPlane.publicKey, BACKEND_ID: 'test-node' },
        { method: 'POST', url: '/tasks', headers, body },
      ),
    ).resolves.toMatchObject({ nonce: expect.any(String), backendId: 'test-node' });
  });

  it('rejects control-plane signatures aimed at a different backend audience', async () => {
    const controlPlane = await identity();
    const body = JSON.stringify({ taskId: 'task-1' });
    const request = new Request('https://agent.example/tasks', { method: 'POST' });
    const headers = await createControlPlaneSignatureHeaders(
      controlPlane.privateKey,
      request,
      body,
      'backend-a',
    );

    await expect(
      verifyControlPlaneRequest(
        { CONTROL_PLANE_PUBLIC_KEY: controlPlane.publicKey, BACKEND_ID: 'backend-b' },
        { method: 'POST', url: '/tasks', headers, body },
      ),
    ).rejects.toThrow(/different backend/);

    // Header spoofed to match local BACKEND_ID but signature still binds backend-a.
    const spoofed = { ...headers, 'x-vps-control-backend-id': 'backend-b' };
    await expect(
      verifyControlPlaneRequest(
        { CONTROL_PLANE_PUBLIC_KEY: controlPlane.publicKey, BACKEND_ID: 'backend-b' },
        { method: 'POST', url: '/tasks', headers: spoofed, body },
      ),
    ).rejects.toThrow(/invalid/i);
  });

  it('rejects query string tampering on signed request targets', async () => {
    const controlPlane = await identity();
    const body = '';
    const request = new Request('https://agent.example/fs/read?path=%2Fetc%2Fpasswd', {
      method: 'GET',
    });
    const headers = await createControlPlaneSignatureHeaders(
      controlPlane.privateKey,
      request,
      body,
      'test-node',
    );

    await expect(
      verifyControlPlaneRequest(
        { CONTROL_PLANE_PUBLIC_KEY: controlPlane.publicKey, BACKEND_ID: 'test-node' },
        { method: 'GET', url: '/fs/read?path=%2Fetc%2Fpasswd', headers, body },
      ),
    ).resolves.toMatchObject({ backendId: 'test-node' });

    await expect(
      verifyControlPlaneRequest(
        { CONTROL_PLANE_PUBLIC_KEY: controlPlane.publicKey, BACKEND_ID: 'test-node' },
        { method: 'GET', url: '/fs/read?path=%2Ftmp%2Fother', headers, body },
      ),
    ).rejects.toThrow(/invalid/i);

    // Pathname-only verification must not succeed for a query-bound signature.
    await expect(
      verifyControlPlaneRequest(
        { CONTROL_PLANE_PUBLIC_KEY: controlPlane.publicKey, BACKEND_ID: 'test-node' },
        { method: 'GET', url: '/fs/read', headers, body },
      ),
    ).rejects.toThrow(/invalid/i);
  });

  it('agrees on request target normalization (pathname + search, no fragment)', () => {
    expect(requestTargetOf('https://agent.example/fs/read?path=a#frag')).toBe('/fs/read?path=a');
    expect(workerRequestTargetOf('https://agent.example/fs/read?path=a#frag')).toBe(
      '/fs/read?path=a',
    );
    expect(requestTargetOf('/tasks')).toBe('/tasks');
    expect(workerRequestTargetOf('https://x.example/')).toBe('/');
  });

  it('rejects agent signatures when the path or query is altered', async () => {
    const agent = await identity();
    const body = JSON.stringify({ ok: true });
    const request = new Request('https://control.example/api/telemetry?x=1', { method: 'POST' });
    const headers = await createAgentSignatureHeaders(
      { BACKEND_ID: 'test-node', AGENT_PRIVATE_KEY: agent.privateKey },
      request,
      body,
    );

    await expect(
      verifyAgentRequestSignature(
        new Request('https://control.example/api/telemetry?x=1', {
          method: 'POST',
          headers,
          body,
        }),
        agent.publicKey,
        body,
      ),
    ).resolves.toMatchObject({ backendId: 'test-node' });

    await expect(
      verifyAgentRequestSignature(
        new Request('https://control.example/api/telemetry?x=2', {
          method: 'POST',
          headers,
          body,
        }),
        agent.publicKey,
        body,
      ),
    ).rejects.toMatchObject({ code: 'invalid_agent_signature' });
  });
});
