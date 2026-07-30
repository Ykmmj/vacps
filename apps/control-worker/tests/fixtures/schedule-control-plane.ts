/**
 * Control-plane schedule e2e fixtures (wire shapes + expected merge/ack outcomes).
 *
 * These are the shared fixtures for:
 * - CP unit tests (ack CAS)
 * - future Worker route e2e
 * - native agent wire parity checks
 *
 * Flow under test:
 *
 * ```
 * CP create/update → compute next_run_at → PUT /schedulers/:id
 * native claim → enqueue occurrence → POST /api/schedules/:id/occurrences/ack
 * CP CAS + authoritative next → re-sync PUT
 * ```
 */

import type { ScheduleOccurrenceAck } from '@vacps/contracts';

export const FIXTURE_BACKEND_ID = 'e2e-backend-1';
export const FIXTURE_SCHEDULE_ID = '22222222-2222-4222-8222-222222222222';

/** Minimal command task template used across fixtures. */
export const fixtureTaskTemplate = {
  kind: 'command' as const,
  backend_id: FIXTURE_BACKEND_ID,
  program: '/bin/true',
  arguments: [] as string[],
  working_directory: '/tmp',
  timeout_seconds: 30,
  profile: 'full' as const,
  output: {
    capture_stdout: true,
    capture_stderr: true,
    preview_max_bytes: 8192,
    retention_seconds: 86_400,
    hard_max_bytes: 10_485_760,
  },
};

/**
 * CP → backend scheduler upsert body (absolute next_run_at + revision).
 * Matches ScheduleService.sync / BackendClient.upsertScheduler payload.
 */
export function fixtureSchedulerPutBody(opts?: {
  revision?: number;
  next_run_at?: string;
  enabled?: boolean;
  cron?: string;
  timezone?: string;
  misfire?: 'skip' | 'run_once' | 'catch_up';
}) {
  return {
    cron: opts?.cron ?? '0 9 * * *',
    timezone: opts?.timezone ?? 'Asia/Shanghai',
    enabled: opts?.enabled ?? true,
    task: fixtureTaskTemplate,
    policy: {
      concurrency: 'forbid' as const,
      misfire: opts?.misfire ?? ('run_once' as const),
      max_catchup_runs: 1,
    },
    revision: opts?.revision ?? 1,
    // Shanghai 09:00 = 01:00 UTC (no DST)
    next_run_at: opts?.next_run_at ?? '2026-07-31T01:00:00.000Z',
  };
}

/**
 * Backend → CP occurrence ack after successful local claim.
 * Matches scheduleOccurrenceAckSchema.
 */
export function fixtureOccurrenceAck(opts?: {
  revision?: number;
  scheduled_for?: string;
  locally_advanced_to?: string;
  enqueued_count?: number;
  occurrence_id?: string;
}): ScheduleOccurrenceAck {
  const scheduled_for = opts?.scheduled_for ?? '2026-07-31T01:00:00.000Z';
  const revision = opts?.revision ?? 1;
  const ms = Date.parse(scheduled_for);
  return {
    backend_id: FIXTURE_BACKEND_ID,
    schedule_id: FIXTURE_SCHEDULE_ID,
    revision,
    scheduled_for,
    enqueued_count: opts?.enqueued_count ?? 1,
    occurrence_id: opts?.occurrence_id ?? `${FIXTURE_SCHEDULE_ID}:${revision}:${ms}`,
    ...(opts?.locally_advanced_to
      ? { locally_advanced_to: opts.locally_advanced_to }
      : { locally_advanced_to: '2026-08-01T01:00:00.000Z' }),
  };
}

/** Expected Shanghai 09:00 mapping for documentation / parity asserts. */
export const FIXTURE_SHANGHAI_0900_UTC = '2026-07-31T01:00:00.000Z';

/**
 * Multi-step e2e scenario checklist (asserted by schedule-e2e-fixture.test.ts).
 */
export const SCHEDULE_E2E_STEPS = [
  'cp_sync_puts_absolute_next_run_at',
  'native_stores_revision_and_next',
  'native_cas_claim_enqueues_occurrence',
  'native_acks_scheduled_for',
  'cp_cas_advances_authoritative_next',
  'cp_resync_does_not_rewind_same_revision_later_cursor',
  'stale_ack_is_already_advanced',
  'old_revision_ack_is_ignored',
] as const;

export type ScheduleE2EStep = (typeof SCHEDULE_E2E_STEPS)[number];
