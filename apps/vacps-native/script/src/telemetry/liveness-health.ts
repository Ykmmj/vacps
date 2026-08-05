import type { BackendHealth } from '@vacps/contracts';

/**
 * Live process signals used by /health and telemetry health fields.
 * Contract: Narrow after Application has established lifecycle flags.
 */
export interface LiveHealthState {
  ok: boolean;
  workerRunning: boolean;
}

export interface DeriveLiveHealthStateInput {
  ready: boolean;
  stopping: boolean;
  hasControlLoop: boolean;
  hasWorkerLoop: boolean;
  hasLoopFailure: boolean;
  closing: boolean;
}

export interface LivenessHealthInput extends LiveHealthState {
  backendId: string;
  version: string;
  uptimeSeconds: number;
}

/** Derive truthful ok/worker.running from Application lifecycle flags. */
export function deriveLiveHealthState(input: DeriveLiveHealthStateInput): LiveHealthState {
  const loopsInstalled = input.hasControlLoop && input.hasWorkerLoop;
  const workerRunning =
    input.ready &&
    !input.stopping &&
    loopsInstalled &&
    !input.hasLoopFailure &&
    !input.closing;
  // ok tracks the same live-worker contract (stopping or missing loops ⇒ not ok).
  const ok = workerRunning;
  return { ok, workerRunning };
}

/** Cheap public /health body — no telemetry probes or shell diagnostics. */
export function buildLivenessHealth(input: LivenessHealthInput): BackendHealth {
  return {
    ok: input.ok,
    backendId: input.backendId,
    version: input.version,
    uptimeSeconds: input.uptimeSeconds,
    worker: { running: input.workerRunning, concurrency: 1 },
    redis: { connected: false },
    pi: { available: false },
  };
}
