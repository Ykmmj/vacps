/**
 * Pure schedule merge / misfire / occurrence helpers (no SQLite).
 * Absolute-time cursor model: revision-gated merge + scheduled_for-based advance.
 *
 * Frozen product rules: @vacps/contracts schedule-semantics
 * (misfire, DST gap/overlap, revision merge).
 */
import type { SchedulePolicy } from "@vacps/contracts";
import {
  DEFAULT_SCHEDULE_POLICY as CONTRACT_DEFAULT_POLICY,
  MAX_SCHEDULE_ADVANCE_STEPS,
  canonicalUtcIso,
  laterUtcIso,
  nextCronRunAtIso,
  scheduleOccurrenceId,
} from "@vacps/contracts";

export const DEFAULT_SCHEDULE_POLICY: SchedulePolicy = { ...CONTRACT_DEFAULT_POLICY };

/** Hard cap per pump tick regardless of policy.max_catchup_runs. */
export const MAX_CATCHUP_STEPS_PER_PUMP = MAX_SCHEDULE_ADVANCE_STEPS;

export function parseSchedulePolicy(raw: unknown): SchedulePolicy {
  if (!raw || typeof raw !== "object") return { ...DEFAULT_SCHEDULE_POLICY };
  const o = raw as Record<string, unknown>;
  const misfire =
    o.misfire === "skip" || o.misfire === "run_once" || o.misfire === "catch_up"
      ? o.misfire
      : DEFAULT_SCHEDULE_POLICY.misfire;
  const concurrency =
    o.concurrency === "allow" ||
    o.concurrency === "forbid" ||
    o.concurrency === "replace" ||
    o.concurrency === "queue"
      ? o.concurrency
      : DEFAULT_SCHEDULE_POLICY.concurrency;
  const max =
    typeof o.max_catchup_runs === "number" && Number.isInteger(o.max_catchup_runs)
      ? Math.min(100, Math.max(0, o.max_catchup_runs))
      : DEFAULT_SCHEDULE_POLICY.max_catchup_runs;
  return { concurrency, misfire, max_catchup_runs: max };
}

/**
 * Deterministic occurrence id: schedule_id:revision:scheduled_for_ms
 * (ms epoch — never raw ISO, so formatting variants cannot split IDs).
 */
export function occurrenceId(
  scheduleId: string,
  revision: number,
  scheduledForMs: number,
): string {
  return scheduleOccurrenceId(scheduleId, revision, scheduledForMs);
}

export interface SchedulerMergeLocal {
  revision: number;
  cron: string;
  timezone: string;
  enabled: boolean;
  task: unknown;
  policy: SchedulePolicy;
  /** Raw SQLite next_run_at (may be non-canonical until rewrite). */
  nextRunAt?: string;
}

export interface SchedulerMergeIncoming {
  revision: number;
  cron: string;
  timezone: string;
  enabled: boolean;
  task: unknown;
  policy: SchedulePolicy;
  /** Undefined = field omitted; null = explicit clear (only honored on higher rev). */
  nextRunAt?: string | null;
}

export type SchedulerMergeResult =
  | { action: "ignore" }
  | {
      action: "apply";
      revision: number;
      cron: string;
      timezone: string;
      enabled: boolean;
      task: unknown;
      policy: SchedulePolicy;
      /** Canonical ISO or undefined when disabled / cleared. */
      nextRunAt?: string;
    };

/**
 * Revision-gated merge:
 * - rev >  local: accept full config (next may move earlier)
 * - rev == local: same rule; next = later of both; empty incoming does not clear local
 * - rev <  local: ignore
 */
export function mergeSchedulerWire(
  local: SchedulerMergeLocal | undefined,
  incoming: SchedulerMergeIncoming,
): SchedulerMergeResult {
  if (!local) {
    const next =
      incoming.enabled && incoming.nextRunAt
        ? canonicalUtcIso(incoming.nextRunAt)
        : undefined;
    const base = {
      action: "apply" as const,
      revision: Math.max(1, incoming.revision),
      cron: incoming.cron,
      timezone: incoming.timezone,
      enabled: incoming.enabled,
      task: incoming.task,
      policy: incoming.policy,
    };
    return next ? { ...base, nextRunAt: next } : base;
  }

  if (incoming.revision < local.revision) {
    return { action: "ignore" };
  }

  if (incoming.revision > local.revision) {
    // Full config replace. Empty next is accepted (caller may recompute).
    const next =
      incoming.enabled && incoming.nextRunAt
        ? canonicalUtcIso(incoming.nextRunAt)
        : undefined;
    const base = {
      action: "apply" as const,
      revision: incoming.revision,
      cron: incoming.cron,
      timezone: incoming.timezone,
      enabled: incoming.enabled,
      task: incoming.task,
      policy: incoming.policy,
    };
    return next ? { ...base, nextRunAt: next } : base;
  }

  // Same revision: config from incoming (should match); next = later epoch merge.
  let nextRunAt: string | undefined;
  if (!incoming.enabled) {
    nextRunAt = undefined;
  } else if (
    incoming.nextRunAt === null ||
    incoming.nextRunAt === undefined ||
    incoming.nextRunAt === ""
  ) {
    // Do not clear local cursor on empty incoming.
    nextRunAt = local.nextRunAt ? canonicalUtcIso(local.nextRunAt) : undefined;
  } else if (!local.nextRunAt) {
    nextRunAt = canonicalUtcIso(incoming.nextRunAt);
  } else {
    nextRunAt = laterUtcIso(local.nextRunAt, incoming.nextRunAt);
  }

  const base = {
    action: "apply" as const,
    revision: local.revision,
    cron: incoming.cron,
    timezone: incoming.timezone,
    enabled: incoming.enabled,
    task: incoming.task,
    policy: incoming.policy,
  };
  return nextRunAt ? { ...base, nextRunAt } : base;
}

export interface MisfirePlan {
  /** Canonical ISO times of occurrences to enqueue (may be empty for skip). */
  enqueueSlots: string[];
  /**
   * New next_run_at after claim (canonical), or null if none within horizon.
   * Always advanced past claimed work / skipped backlog per policy.
   */
  advancedNext: string | null;
  /** The raw scheduled_for string used as CAS token (from DB). */
  scheduledForRaw: string;
  /** Epoch ms of scheduledForRaw. */
  scheduledForMs: number;
}

/**
 * Plan misfire handling from a due cursor.
 * Advance is always based on scheduled_for chain, never wall-clock-as-cron-after.
 */
export function planMisfire(input: {
  cron: string;
  timezone: string;
  policy: SchedulePolicy;
  /** Raw next_run_at from SQLite (CAS token). */
  nextRunAtRaw: string;
  nowMs: number;
  maxSteps?: number;
}): MisfirePlan | undefined {
  const scheduledForMs = Date.parse(input.nextRunAtRaw);
  if (!Number.isFinite(scheduledForMs) || scheduledForMs > input.nowMs) {
    return undefined;
  }

  const maxSteps = Math.min(
    input.maxSteps ?? MAX_CATCHUP_STEPS_PER_PUMP,
    MAX_CATCHUP_STEPS_PER_PUMP,
  );
  const maxCatchup = Math.min(
    Math.max(0, input.policy.max_catchup_runs),
    maxSteps,
  );

  // Walk missed slots starting at stored next (inclusive).
  const missed: string[] = [];
  let cursorIso = canonicalUtcIso(scheduledForMs);
  if (!cursorIso) return undefined;

  for (let i = 0; i < maxSteps; i++) {
    const ms = Date.parse(cursorIso);
    if (!Number.isFinite(ms) || ms > input.nowMs) break;
    missed.push(cursorIso);
    const next = nextCronRunAtIso(input.cron, input.timezone, new Date(ms));
    if (!next) break;
    cursorIso = next;
  }

  if (missed.length === 0) return undefined;

  const first = missed[0]!;
  let enqueueSlots: string[] = [];
  let advancedNext: string | null = null;

  switch (input.policy.misfire) {
    case "skip": {
      enqueueSlots = [];
      // Advance to first slot strictly > now.
      advancedNext = advancePastNow(input.cron, input.timezone, first, input.nowMs, maxSteps);
      break;
    }
    case "catch_up": {
      const n = Math.max(1, maxCatchup);
      enqueueSlots = missed.slice(0, n);
      const last = enqueueSlots[enqueueSlots.length - 1]!;
      const afterLast = nextCronRunAtIso(
        input.cron,
        input.timezone,
        new Date(Date.parse(last)),
      );
      // If still behind now and we hit cap, leave cursor at afterLast (remaining backlog next tick).
      // If afterLast still <= now and we consumed all missed in plan walk, jump past now.
      if (afterLast && Date.parse(afterLast) > input.nowMs) {
        advancedNext = afterLast;
      } else if (enqueueSlots.length >= missed.length) {
        advancedNext =
          advancePastNow(input.cron, input.timezone, last, input.nowMs, maxSteps) ??
          afterLast ??
          null;
      } else {
        advancedNext = afterLast ?? null;
      }
      break;
    }
    case "run_once":
    default: {
      enqueueSlots = [first];
      advancedNext = advancePastNow(
        input.cron,
        input.timezone,
        first,
        input.nowMs,
        maxSteps,
      );
      break;
    }
  }

  return {
    enqueueSlots,
    advancedNext,
    scheduledForRaw: input.nextRunAtRaw,
    scheduledForMs,
  };
}

/** Chain nextCron from `afterIso` until result > nowMs (or horizon). */
function advancePastNow(
  cron: string,
  timezone: string,
  afterIso: string,
  nowMs: number,
  maxSteps: number,
): string | null {
  let cursor = afterIso;
  for (let i = 0; i < maxSteps; i++) {
    const next = nextCronRunAtIso(cron, timezone, new Date(Date.parse(cursor)));
    if (!next) return null;
    if (Date.parse(next) > nowMs) return next;
    cursor = next;
  }
  // Hit step cap still behind: return last computed next (bounded).
  return nextCronRunAtIso(cron, timezone, new Date(Date.parse(cursor))) ?? null;
}

export { canonicalUtcIso, laterUtcIso, nextCronRunAtIso };
