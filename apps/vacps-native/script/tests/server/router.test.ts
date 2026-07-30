import { describe, expect, it } from 'vitest';

import type { HostRequest } from '../../src/contracts/http';
import { createApp } from '../../src/server/router';

function req(partial: Partial<HostRequest> & Pick<HostRequest, 'method' | 'path'>): HostRequest {
  return {
    method: partial.method,
    path: partial.path,
    query: partial.query ?? '',
    headers: partial.headers ?? {},
    body: partial.body ?? '',
    requestId: partial.requestId ?? 'req-1',
  };
}

describe('createApp router', () => {
  it('routes GET /health', async () => {
    const app = createApp();
    app.get('/health', async () => ({ ok: true }));
    const res = await app.handleRequest(req({ method: 'GET', path: '/health' }));
    expect(res.status).toBe(200);
    expect(JSON.parse(res.body)).toEqual({ ok: true });
  });

  it('strips trailing slash', async () => {
    const app = createApp();
    app.get('/ready', async () => ({ ready: true }));
    const res = await app.handleRequest(req({ method: 'GET', path: '/ready/' }));
    expect(res.status).toBe(200);
    expect(JSON.parse(res.body).ready).toBe(true);
  });

  it('binds :id params', async () => {
    const app = createApp();
    app.get('/tasks/:id', async (r) => ({ id: r.params.id }));
    const res = await app.handleRequest(req({ method: 'GET', path: '/tasks/abc-123' }));
    expect(JSON.parse(res.body)).toEqual({ id: 'abc-123' });
  });

  it('returns 404 for unknown route', async () => {
    const app = createApp();
    const res = await app.handleRequest(req({ method: 'GET', path: '/nope' }));
    expect(res.status).toBe(404);
    expect(JSON.parse(res.body).error.code).toBe('not_found');
  });

  it('parses JSON body', async () => {
    const app = createApp();
    app.post('/tasks', async (r) => ({ got: r.body }));
    const res = await app.handleRequest(
      req({
        method: 'POST',
        path: '/tasks',
        headers: { 'content-type': 'application/json' },
        body: '{"x":1}',
      }),
    );
    expect(JSON.parse(res.body)).toEqual({ got: { x: 1 } });
  });

  it('rejects invalid JSON body', async () => {
    const app = createApp();
    app.post('/tasks', async () => ({ ok: true }));
    const res = await app.handleRequest(
      req({
        method: 'POST',
        path: '/tasks',
        headers: { 'content-type': 'application/json' },
        body: '{not-json',
      }),
    );
    expect(res.status).toBe(400);
    expect(JSON.parse(res.body).error.code).toBe('invalid_json');
  });

  it('reply.code().send() sets status', async () => {
    const app = createApp();
    app.post('/x', async (_r, reply) => reply.code(202).send({ queued: true }));
    const res = await app.handleRequest(req({ method: 'POST', path: '/x', body: '{}' }));
    expect(res.status).toBe(202);
    expect(JSON.parse(res.body)).toEqual({ queued: true });
  });

  it('preValidation can short-circuit', async () => {
    const app = createApp();
    app.addHook('preValidation', async (_r, reply) =>
      reply.code(401).send({ error: { code: 'unauthorized' } }),
    );
    app.get('/secret', async () => ({ ok: true }));
    const res = await app.handleRequest(req({ method: 'GET', path: '/secret' }));
    expect(res.status).toBe(401);
  });

  it('parses query string', async () => {
    const app = createApp();
    app.get('/fs/read', async (r) => ({ q: r.query }));
    const res = await app.handleRequest(
      req({ method: 'GET', path: '/fs/read', query: 'path=%2Ftmp%2Fa&limit=10' }),
    );
    expect(JSON.parse(res.body).q).toEqual({ path: '/tmp/a', limit: '10' });
  });

  it('method mismatch is 404', async () => {
    const app = createApp();
    app.get('/only-get', async () => ({}));
    const res = await app.handleRequest(req({ method: 'POST', path: '/only-get' }));
    expect(res.status).toBe(404);
  });
});
