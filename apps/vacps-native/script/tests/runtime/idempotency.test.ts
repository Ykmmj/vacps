import { describe, expect, it } from 'vitest';

import { hashRequest, IdempotencyStore } from '../../src/runtime/idempotency';

describe('IdempotencyStore', () => {
  it('runs the first keyed operation', async () => {
    const s = new IdempotencyStore();
    await expect(
      s.execute('files.write', 'k1', 'h1', async () => ({ path: '/a' })),
    ).resolves.toEqual({ result: { path: '/a' }, replayed: false });
  });

  it('does not retain operations without a key', async () => {
    const s = new IdempotencyStore();
    let runs = 0;
    const run = async () => ({ run: ++runs });
    await expect(s.execute('files.write', undefined, 'h1', run)).resolves.toEqual({
      result: { run: 1 },
      replayed: false,
    });
    await expect(s.execute('files.write', undefined, 'h1', run)).resolves.toEqual({
      result: { run: 2 },
      replayed: false,
    });
  });

  it('stores and replays matching key+hash', async () => {
    const s = new IdempotencyStore();
    let runs = 0;
    const run = async () => ({ path: `/a/${++runs}` });
    await s.execute('files.write', 'k1', 'h1', run);
    await expect(s.execute('files.write', 'k1', 'h1', run)).resolves.toEqual({
      result: { path: '/a/1' },
      replayed: true,
    });
    expect(runs).toBe(1);
  });

  it('throws on key reuse with different hash', async () => {
    const s = new IdempotencyStore();
    await s.execute('files.write', 'k1', 'h1', async () => ({ path: '/a' }));
    try {
      await s.execute('files.write', 'k1', 'h2', async () => ({ path: '/b' }));
      throw new Error('expected conflict');
    } catch (e) {
      expect((e as { code: string }).code).toBe('idempotency_conflict');
      expect((e as { statusCode: number }).statusCode).toBe(409);
    }
  });

  it('withIdempotencyMeta attaches meta when key present', () => {
    const s = new IdempotencyStore();
    const body = s.withIdempotencyMeta('k', 'hash', true, { ok: true });
    expect(body).toMatchObject({
      ok: true,
      idempotency: { key: 'k', replayed: true, request_hash: 'hash' },
    });
  });

  it('withIdempotencyMeta leaves body when no key', () => {
    const s = new IdempotencyStore();
    expect(s.withIdempotencyMeta(undefined, 'h', false, { ok: true })).toEqual({ ok: true });
  });
});

describe('hashRequest', () => {
  it('is stable under key order', () => {
    const a = hashRequest({ b: 1, a: 2 });
    const b = hashRequest({ a: 2, b: 1 });
    expect(a).toBe(b);
    expect(a.startsWith('sha256:')).toBe(true);
  });

  it('differs for different payloads', () => {
    expect(hashRequest({ x: 1 })).not.toBe(hashRequest({ x: 2 }));
  });
});
