import { describe, expect, it } from 'vitest';

import { buildLivenessHealth, deriveLiveHealthState } from '../../src/telemetry/liveness-health';

describe('deriveLiveHealthState', () => {
  it('reports ok and worker running only when fully ready with both loops', () => {
    expect(
      deriveLiveHealthState({
        ready: true,
        stopping: false,
        hasControlLoop: true,
        hasWorkerLoop: true,
        hasLoopFailure: false,
        closing: false,
      }),
    ).toEqual({ ok: true, workerRunning: true });
  });

  it('is not ok before ready even if loops exist', () => {
    expect(
      deriveLiveHealthState({
        ready: false,
        stopping: false,
        hasControlLoop: true,
        hasWorkerLoop: true,
        hasLoopFailure: false,
        closing: false,
      }),
    ).toEqual({ ok: false, workerRunning: false });
  });

  it('marks worker stopped and not ok after loop failure', () => {
    expect(
      deriveLiveHealthState({
        ready: false,
        stopping: true,
        hasControlLoop: true,
        hasWorkerLoop: true,
        hasLoopFailure: true,
        closing: true,
      }),
    ).toEqual({ ok: false, workerRunning: false });
  });

  it('marks not ok while product close is in progress', () => {
    expect(
      deriveLiveHealthState({
        ready: true,
        stopping: true,
        hasControlLoop: true,
        hasWorkerLoop: true,
        hasLoopFailure: false,
        closing: true,
      }),
    ).toEqual({ ok: false, workerRunning: false });
  });

  it('requires both loops for ok and worker.running', () => {
    expect(
      deriveLiveHealthState({
        ready: true,
        stopping: false,
        hasControlLoop: true,
        hasWorkerLoop: false,
        hasLoopFailure: false,
        closing: false,
      }),
    ).toEqual({ ok: false, workerRunning: false });
  });

  it('is not ok while stopping even if ready and loops remain', () => {
    expect(
      deriveLiveHealthState({
        ready: true,
        stopping: true,
        hasControlLoop: true,
        hasWorkerLoop: true,
        hasLoopFailure: false,
        closing: false,
      }),
    ).toEqual({ ok: false, workerRunning: false });
  });
});

describe('buildLivenessHealth', () => {
  it('builds a cheap BackendHealth snapshot without probes', () => {
    expect(
      buildLivenessHealth({
        ok: true,
        workerRunning: true,
        backendId: 'agent-1',
        version: '0.1.0-test',
        uptimeSeconds: 42,
      }),
    ).toEqual({
      ok: true,
      backendId: 'agent-1',
      version: '0.1.0-test',
      uptimeSeconds: 42,
      worker: { running: true, concurrency: 1 },
      redis: { connected: false },
      pi: { available: false },
    });
  });

  it('reflects not-ready / stopped worker honestly', () => {
    const health = buildLivenessHealth({
      ok: false,
      workerRunning: false,
      backendId: 'agent-1',
      version: '0.1.0-test',
      uptimeSeconds: 0,
    });
    expect(health.ok).toBe(false);
    expect(health.worker.running).toBe(false);
  });
});
