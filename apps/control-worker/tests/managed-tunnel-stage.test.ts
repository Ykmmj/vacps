import { describe, expect, it } from 'vitest';

import {
  deriveManagedTunnelStage,
  managedTunnelStageIndex,
} from '../ui/src/managed-tunnel-stage.js';

describe('deriveManagedTunnelStage', () => {
  it('starts unconfigured when OAuth client secrets are missing', () => {
    expect(deriveManagedTunnelStage({ configured: false, connected: false }, undefined)).toBe(
      'unconfigured',
    );
    expect(deriveManagedTunnelStage(undefined, undefined)).toBe('unconfigured');
  });

  it('needs connect when tokens are missing or unusable', () => {
    expect(deriveManagedTunnelStage({ configured: true, connected: false }, undefined)).toBe(
      'needs_connect',
    );
  });

  it('needs zone after a successful connect without a selected domain', () => {
    expect(deriveManagedTunnelStage({ configured: true, connected: true }, undefined)).toBe(
      'needs_zone',
    );
    expect(
      deriveManagedTunnelStage(
        { configured: true, connected: true, zoneId: 'z', baseDomain: '' },
        undefined,
      ),
    ).toBe('needs_zone');
  });

  it('needs tunnel once a zone is selected', () => {
    expect(
      deriveManagedTunnelStage(
        {
          configured: true,
          connected: true,
          zoneId: '0123456789abcdef0123456789abcdef',
          baseDomain: 'example.com',
        },
        undefined,
      ),
    ).toBe('needs_tunnel');
  });

  it('is ready after a tunnel is provisioned for the install command', () => {
    expect(
      deriveManagedTunnelStage(
        {
          configured: true,
          connected: true,
          zoneId: '0123456789abcdef0123456789abcdef',
          baseDomain: 'example.com',
        },
        { backendId: 'vacps-715f765653e6' },
      ),
    ).toBe('ready');
  });

  it('maps stages to progress-rail indices', () => {
    expect(managedTunnelStageIndex('unconfigured')).toBe(0);
    expect(managedTunnelStageIndex('needs_connect')).toBe(1);
    expect(managedTunnelStageIndex('needs_zone')).toBe(2);
    expect(managedTunnelStageIndex('needs_tunnel')).toBe(3);
    expect(managedTunnelStageIndex('ready')).toBe(4);
  });
});
