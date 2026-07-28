import { describe, expect, it } from 'vitest';

import {
  createManagedBackendId,
  isManagedBackendId,
  MANAGED_TUNNEL_ID_PATTERN,
  parseBackendIdFromTunnelName,
} from '../src/tunnels/managed-tunnel-service.js';

describe('managed tunnel naming', () => {
  it('creates vacps- ids with 12 hex characters', () => {
    const id = createManagedBackendId();
    expect(id).toMatch(MANAGED_TUNNEL_ID_PATTERN);
    expect(isManagedBackendId(id)).toBe(true);
  });

  it('parses vacps and legacy vps names', () => {
    expect(parseBackendIdFromTunnelName('vacps-715f765653e6')).toBe('vacps-715f765653e6');
    expect(parseBackendIdFromTunnelName('vps-715f765653e6')).toBe('vps-715f765653e6');
    expect(parseBackendIdFromTunnelName('VACPS vps-715f765653e6')).toBe('vps-715f765653e6');
    expect(parseBackendIdFromTunnelName('edge-node')).toBeUndefined();
  });
});
