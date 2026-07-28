import { describe, expect, it } from 'vitest';

import { hashRequest, IdempotencyStore } from './idempotency.js';

describe('IdempotencyStore', () => {
  it('replays the same request and conflicts on different arguments', () => {
    const store = new IdempotencyStore();
    const hashOne = hashRequest({
      tool_name: 'files.write',
      backend_id: 'b1',
      arguments: { path: '/t', content: 'one\n', mode: 'create' },
    });
    const hashTwo = hashRequest({
      tool_name: 'files.write',
      backend_id: 'b1',
      arguments: { path: '/t', content: 'two\n', mode: 'overwrite' },
    });
    expect(hashOne).not.toBe(hashTwo);

    store.store('files.write', 'k1', hashOne, { ok: true, content: 'one' });
    const hit = store.lookup('files.write', 'k1', hashOne);
    expect(hit).toEqual({ ok: true, content: 'one' });

    expect(() => store.lookup('files.write', 'k1', hashTwo)).toThrowError(
      /previously used with different arguments/,
    );
  });
});
