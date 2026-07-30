import { describe, expect, it } from 'vitest';

import { backendTelemetrySchema, telemetrySettingsSchema } from '../src/backend.js';

describe('backend telemetry contracts', () => {
  const telemetry = {
    backendId: 'vps-la-01',
    agentVersion: '0.1.0',
    observedAt: '2026-07-26T09:00:00.000Z',
    health: {
      ok: true,
      backendId: 'vps-la-01',
      version: '0.1.0',
      uptimeSeconds: 42,
      worker: { running: true, concurrency: 1 },
      redis: { connected: true },
      pi: { available: false },
    },
    metrics: {
      cpu: { usagePercent: 17.4, load1: 0.42, cores: 2 },
      memory: { totalBytes: 2_147_483_648, usedBytes: 536_870_912 },
      disk: { totalBytes: 21_474_836_480, usedBytes: 5_368_709_120 },
      network: { receivedBytesPerSecond: 12_345, transmittedBytesPerSecond: 678 },
      queue: { waiting: 0, active: 1, failed: 0 },
    },
    system: {
      platform: 'linux',
      distribution: 'Ubuntu 24.04 LTS',
      version: '24.04',
      kernel: '6.8.0-31-generic',
      architecture: 'x64',
    },
  };

  it('accepts a chart-ready node snapshot', () => {
    expect(backendTelemetrySchema.parse(telemetry)).toMatchObject({
      backendId: 'vps-la-01',
      metrics: { network: { receivedBytesPerSecond: 12_345 } },
      system: { distribution: 'Ubuntu 24.04 LTS' },
    });
  });

  it('retains compatibility with status responses that predate network telemetry', () => {
    const { network: _network, ...metricsWithoutNetwork } = telemetry.metrics;
    expect(
      backendTelemetrySchema.parse({ ...telemetry, metrics: metricsWithoutNetwork }),
    ).toMatchObject({ metrics: { cpu: { cores: 2 } } });
  });

  it('enforces the safe global reporting cadence range', () => {
    expect(telemetrySettingsSchema.parse({ intervalSeconds: 15 })).toEqual({ intervalSeconds: 15 });
    expect(telemetrySettingsSchema.parse({ intervalSeconds: 3600 })).toEqual({
      intervalSeconds: 3600,
    });
    expect(() => telemetrySettingsSchema.parse({ intervalSeconds: 14 })).toThrow();
    expect(() => telemetrySettingsSchema.parse({ intervalSeconds: 3601 })).toThrow();
  });
});
