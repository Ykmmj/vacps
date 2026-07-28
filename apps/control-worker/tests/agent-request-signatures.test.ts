import { webcrypto } from 'node:crypto';

import { describe, expect, it } from 'vitest';

import {
  createAgentSignatureHeaders,
  verifyControlPlaneRequest,
} from '../../vacps/src/security/request-signatures.js';
import {
  createControlPlaneSignatureHeaders,
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

describe('Agent request signatures', () => {
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
    );

    await expect(
      verifyControlPlaneRequest(
        { CONTROL_PLANE_PUBLIC_KEY: controlPlane.publicKey },
        { method: 'POST', url: '/tasks', headers, body },
      ),
    ).resolves.toMatchObject({ nonce: expect.any(String) });
  });
});
