import { describe, expect, it } from 'vitest';

import { isActingOnNode } from '../ui/src/is-acting.js';

describe('isActingOnNode', () => {
  const pending = {
    registration: { id: 'reg-1', backendId: 'vacps-abc', status: 'pending' },
  };

  it('is false when idle even if the node has no backend (pending registration)', () => {
    expect(isActingOnNode(undefined, pending)).toBe(false);
    expect(isActingOnNode(null, pending)).toBe(false);
    expect(isActingOnNode('', pending)).toBe(false);
  });

  it('is true only for the registration currently being acted on', () => {
    expect(isActingOnNode('reg-1', pending)).toBe(true);
    expect(isActingOnNode('other', pending)).toBe(false);
    expect(isActingOnNode('vacps-abc', pending)).toBe(true);
  });

  it('matches backend id for approved nodes', () => {
    const approved = {
      registration: { id: 'reg-2', backendId: 'vacps-xyz' },
      backend: { id: 'vacps-xyz' },
    };
    expect(isActingOnNode(undefined, approved)).toBe(false);
    expect(isActingOnNode('vacps-xyz', approved)).toBe(true);
    expect(isActingOnNode('reg-2', approved)).toBe(true);
  });
});
