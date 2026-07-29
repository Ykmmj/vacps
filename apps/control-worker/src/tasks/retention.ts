/**
 * Task retention policy helpers (Phase 0–1).
 * Defaults match docs recommendation; policy table comes later.
 */

import { isTerminalTaskStatus, type TaskStatus } from '@vacps/contracts';

/** Recommended first-ship defaults (days). */
export const RETENTION_DAYS = {
  test: 3,
  succeeded: 14,
  cancelled: 14,
  failed: 30,
  dispatch_failed: 30,
  timed_out: 30,
  default: 30,
} as const;

export const SOFT_DELETE_GRACE_HOURS = 24;
export const CLEANUP_BATCH_SIZE = 500;
export const CLEANUP_MAX_PER_RUN = 5_000;
/** Allow small drift between preview and run without failing. */
export const CLEANUP_COUNT_TOLERANCE = 0.1;
export const CLEANUP_COUNT_ABS_TOLERANCE = 5;

export type RetentionClass = 'test' | 'success' | 'failure' | 'cancelled' | 'default';

export function parseLabelsJson(raw: string | null | undefined): Record<string, string> {
  if (!raw) return {};
  try {
    const parsed = JSON.parse(raw) as unknown;
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return {};
    const out: Record<string, string> = {};
    for (const [key, value] of Object.entries(parsed as Record<string, unknown>)) {
      if (typeof value === 'string') out[key] = value;
    }
    return out;
  } catch {
    return {};
  }
}

/** True when task is tagged as test / regression noise. */
export function isTestTask(
  labels: Record<string, string> | null | undefined,
  environment: string | null | undefined,
): boolean {
  if (environment === 'test') return true;
  if (!labels) return false;
  if (labels.environment === 'test') return true;
  if (labels.suite && labels.suite.trim().length > 0) return true;
  if (labels.purpose === 'regression' || labels.purpose === 'acceptance-test') return true;
  return false;
}

export function environmentFromLabels(
  labels: Record<string, string> | null | undefined,
): string | null {
  const env = labels?.environment?.trim();
  return env ? env.slice(0, 64) : null;
}

export function retentionClassFor(
  status: TaskStatus | string,
  labels: Record<string, string> | null | undefined,
  environment: string | null | undefined,
): RetentionClass {
  if (isTestTask(labels, environment)) return 'test';
  if (status === 'succeeded') return 'success';
  if (status === 'cancelled') return 'cancelled';
  if (status === 'failed' || status === 'dispatch_failed' || status === 'timed_out') {
    return 'failure';
  }
  return 'default';
}

export function retentionDaysFor(retentionClass: RetentionClass, status: TaskStatus | string): number {
  if (retentionClass === 'test') return RETENTION_DAYS.test;
  if (status === 'succeeded') return RETENTION_DAYS.succeeded;
  if (status === 'cancelled') return RETENTION_DAYS.cancelled;
  if (status === 'failed') return RETENTION_DAYS.failed;
  if (status === 'dispatch_failed') return RETENTION_DAYS.dispatch_failed;
  if (status === 'timed_out') return RETENTION_DAYS.timed_out;
  return RETENTION_DAYS.default;
}

export function computeExpiresAt(
  terminalAtIso: string,
  retentionClass: RetentionClass,
  status: TaskStatus | string,
): string {
  const days = retentionDaysFor(retentionClass, status);
  const base = Date.parse(terminalAtIso);
  if (Number.isNaN(base)) {
    return new Date(Date.now() + days * 86_400_000).toISOString();
  }
  return new Date(base + days * 86_400_000).toISOString();
}

export function addHours(iso: string, hours: number): string {
  const base = Date.parse(iso);
  const t = Number.isNaN(base) ? Date.now() : base;
  return new Date(t + hours * 3_600_000).toISOString();
}

export function scopeCountAcceptable(expected: number, actual: number): boolean {
  const abs = Math.abs(actual - expected);
  if (abs <= CLEANUP_COUNT_ABS_TOLERANCE) return true;
  if (expected === 0) return actual === 0;
  return abs / expected <= CLEANUP_COUNT_TOLERANCE;
}

export { isTerminalTaskStatus };
