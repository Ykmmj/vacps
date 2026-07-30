/**
 * Frozen schedule semantics (Slice D).
 *
 * Layering (not optional):
 *
 * ```
 * cron + timezone          rule expression (human/API)
 *       ↓  IANA / Intl
 * next_run_at (absolute)   execution cursor + wire + CAS
 *       ↓
 * now_ms >= next_run_at    fire decision (no tzdb)
 * ```
 *
 * Absolute time is the execution protocol. IANA projection is only how rules
 * become absolute instants. Control plane is authoritative for that projection;
 * backends may advance offline with the same helper, then reconcile via
 * revision + occurrence ack.
 */

import type { SchedulePolicy } from './schedule.js';

/** Default policy values (must match schedulePolicySchema defaults). */
export const DEFAULT_SCHEDULE_POLICY: SchedulePolicy = {
  concurrency: 'forbid',
  misfire: 'run_once',
  max_catchup_runs: 1,
};

/**
 * Hard cap on cron steps per single pump/ack advance, regardless of
 * policy.max_catchup_runs. Prevents event-loop starvation.
 */
export const MAX_SCHEDULE_ADVANCE_STEPS = 32;

/**
 * Misfire when `now` is past one or more due absolute cursors.
 *
 * | Policy     | Tasks enqueued                         | Cursor after claim                          |
 * |------------|----------------------------------------|---------------------------------------------|
 * | run_once   | One occurrence = first due slot        | Advanced past backlog until next > now      |
 * | skip       | None                                   | Advanced past backlog until next > now      |
 * | catch_up   | Up to max_catchup_runs missed slots    | After last enqueued slot; remainder later   |
 *
 * Advance is always chained from **scheduled_for** (claimed cursor), never
 * from wall-clock `now` as the cron base. That preserves phase under late starts.
 *
 * Already-queued tasks are never cancelled when a schedule is disabled; disable
 * only prevents further claims (`enabled = 1` in CAS).
 */
export const MISFIRE_POLICY_SPEC = {
  run_once:
    'Enqueue the first due occurrence only; advance next_run_at past the entire backlog until next > now.',
  skip: 'Enqueue nothing; advance next_run_at past the backlog until next > now.',
  catch_up:
    'Enqueue up to max_catchup_runs consecutive due occurrences; leave remaining backlog for later ticks.',
} as const;

/**
 * DST / civil-time rules for cron → absolute conversion
 * (`nextCronRunAfter` / Intl projection).
 *
 * Algorithm: walk UTC minutes forward; project each instant into the IANA zone;
 * match 5-field cron against projected wall fields.
 *
 * | Situation                         | Behavior                                      |
 * |-----------------------------------|-----------------------------------------------|
 * | Spring gap (local time missing)   | No UTC minute projects to that wall clock → **skip** (no occurrence) |
 * | Fall overlap (local time twice)   | **Both** UTC instants are valid if fields match → may fire twice |
 * | Fixed offsets / no DST zones      | Single mapping; no special case               |
 *
 * Rationale for overlap = both: the walk is absolute-time ordered; each distinct
 * UTC minute is a real world instant. Forcing “once per civil day” would need
 * extra policy and is not implied by 5-field cron alone.
 *
 * ICU/tzdb version skew between CP and backend: CP ack path recomputes
 * authoritative next; local advance is offline continuity only.
 */
export const DST_POLICY_SPEC = {
  gap: 'skip_missing_local_time',
  overlap: 'fire_each_matching_utc_instant',
  authority: 'control_plane_intl',
} as const;

/**
 * Revision-gated merge of schedule wire (backend upsert).
 *
 * - incoming.rev > local: accept full config; next may move earlier or clear
 * - incoming.rev == local: same rule; next = later of both epochs; empty
 *   incoming next does **not** clear local cursor
 * - incoming.rev < local: ignore stale sync
 */
export const REVISION_MERGE_SPEC = {
  higher_rev: 'accept_config_and_next',
  same_rev: 'keep_later_next_cursor',
  lower_rev: 'ignore',
} as const;

/** Occurrence identity for task_id / CAS correlation. */
export function scheduleOccurrenceId(
  scheduleId: string,
  revision: number,
  scheduledForMs: number,
): string {
  return `${scheduleId}:${revision}:${scheduledForMs}`;
}
