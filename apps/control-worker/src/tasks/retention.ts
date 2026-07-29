/**
 * Task retention policy helpers (Phase 0–1).
 * Defaults match docs recommendation; policy table comes later.
 */

import { isTerminalTaskStatus, type TaskStatus } from '@vacps/contracts';

/**
 * Conservative defaults for current Vacps scale (no long dry-run program).
 * test 3d · success 14d · fail/timeout/cancel 30d · output 7d · soft then 24h hard.
 */
export const RETENTION_DAYS = {
  test: 3,
  succeeded: 14,
  cancelled: 30,
  failed: 30,
  dispatch_failed: 30,
  timed_out: 30,
  default: 30,
} as const;

/** Agent/output TTL target (control plane records output_expires_at; agent purge later). */
export const OUTPUT_RETENTION_DAYS = 7;

export const SOFT_DELETE_GRACE_HOURS = 24;
export const CLEANUP_BATCH_SIZE = 500;
export const CLEANUP_MAX_PER_RUN = 5_000;

/** Create-idempotency tombstone TTL (days) after create / hard-delete mark. */
export const IDEMPOTENCY_TTL_DAYS = {
  test: 7,
  default: 30,
} as const;

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

export function retentionDaysFor(
  retentionClass: RetentionClass,
  status: TaskStatus | string,
): number {
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

export function computeOutputExpiresAt(terminalAtIso: string): string {
  return addHours(terminalAtIso, OUTPUT_RETENTION_DAYS * 24);
}

/** Protected rows skip automatic retention purge (and default bulk cleanup). */
export function isRetentionProtected(row: {
  legal_hold?: number | boolean | null;
  pinned_at?: string | null;
}): boolean {
  if (row.pinned_at) return true;
  if (row.legal_hold === true || row.legal_hold === 1) return true;
  return false;
}

export function addHours(iso: string, hours: number): string {
  const base = Date.parse(iso);
  const t = Number.isNaN(base) ? Date.now() : base;
  return new Date(t + hours * 3_600_000).toISOString();
}

/**
 * Preview confirmation guard: expected_matched_count must match exactly.
 * Any drift (more or fewer rows) aborts cleanup before mutation.
 */
export function scopeCountAcceptable(expected: number, actual: number): boolean {
  return expected === actual;
}

export function idempotencyTtlDays(
  labels: Record<string, string> | null | undefined,
  environment: string | null | undefined,
): number {
  return isTestTask(labels, environment) ? IDEMPOTENCY_TTL_DAYS.test : IDEMPOTENCY_TTL_DAYS.default;
}

export function computeIdempotencyExpiresAt(
  fromIso: string,
  labels: Record<string, string> | null | undefined,
  environment: string | null | undefined,
): string {
  const days = idempotencyTtlDays(labels, environment);
  return addHours(fromIso, days * 24);
}

export { isTerminalTaskStatus };
