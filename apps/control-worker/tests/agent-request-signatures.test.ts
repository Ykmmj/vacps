import { webcrypto } from 'node:crypto';

import { describe, expect, it } from 'vitest';

import {
  base64UrlEncode,
  createControlPlaneSignatureHeaders,
  requestTargetOf,
  sha256Base64Url,
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

async function createAgentSignatureHeaders(
  privateKey: string,
  request: Pick<Request, 'method' | 'url'>,
  body: string,
  backendId: string,
): Promise<Record<string, string>> {
  const timestamp = String(Math.floor(Date.now() / 1000));
  const nonce = base64UrlEncode(webcrypto.getRandomValues(new Uint8Array(16)));
  const canonical = [
    'vacps-request-v2',
    'agent',
    request.method.toUpperCase(),
    requestTargetOf(request.url),
    backendId,
    timestamp,
    nonce,
    await sha256Base64Url(body),
  ].join('\n');
  const key = await webcrypto.subtle.importKey(
    'pkcs8',
    Buffer.from(privateKey, 'base64url'),
    { name: 'Ed25519' },
    false,
    ['sign'],
  );
  const signature = await webcrypto.subtle.sign(
    'Ed25519',
    key,
    new TextEncoder().encode(canonical),
  );
  return {
    'x-vacps-id': backendId,
    'x-vacps-timestamp': timestamp,
    'x-vacps-nonce': nonce,
    'x-vacps-signature': Buffer.from(signature).toString('base64url'),
  };
}

describe('Agent request signatures (v2)', () => {
  it('verifies an Agent registration request in the Worker', async () => {
    const agent = await identity();
    const body = JSON.stringify({ backendId: 'test-node', publicKey: agent.publicKey });
    const request = new Request('https://control.example/api/registrations', { method: 'POST' });
    const headers = await createAgentSignatureHeaders(agent.privateKey, request, body, 'test-node');
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

  it('binds control-plane signatures to backend audience and request target', async () => {
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

    expect(headers['x-vps-control-backend-id']).toBe('test-node');
    expect(headers['x-vps-control-signature']).toMatch(/^[A-Za-z0-9_-]{86}$/);
  });

  it('normalizes request targets as pathname + search without fragments', () => {
    expect(requestTargetOf('https://agent.example/fs/read?path=a#frag')).toBe('/fs/read?path=a');
    expect(requestTargetOf('/tasks')).toBe('/tasks');
    expect(requestTargetOf('https://x.example/')).toBe('/');
  });

  it('rejects agent signatures when the path or query is altered', async () => {
    const agent = await identity();
    const body = JSON.stringify({ ok: true });
    const request = new Request('https://control.example/api/telemetry?x=1', { method: 'POST' });
    const headers = await createAgentSignatureHeaders(agent.privateKey, request, body, 'test-node');

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
