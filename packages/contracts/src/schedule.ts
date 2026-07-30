import { z } from 'zod';

import { backendIdSchema } from './backend.js';
import { createTaskSchema, scheduleTaskSchema, withBackendId } from './task.js';

export const scheduleConcurrencyPolicies = ['allow', 'forbid', 'replace', 'queue'] as const;
export const scheduleMisfirePolicies = ['skip', 'run_once', 'catch_up'] as const;

/**
 * Schedule execution policy. See `schedule-semantics.ts` for frozen misfire/DST rules.
 *
 * - misfire=run_once (default): one task for first due slot; jump cursor past backlog
 * - misfire=skip: no task; jump cursor past backlog
 * - misfire=catch_up: up to max_catchup_runs tasks; remainder on later ticks
 */
export const schedulePolicySchema = z.object({
  concurrency: z.enum(scheduleConcurrencyPolicies).default('forbid'),
  misfire: z.enum(scheduleMisfirePolicies).default('run_once'),
  /** Cap for catch_up enqueues per claim (also hard-capped by MAX_SCHEDULE_ADVANCE_STEPS). */
  max_catchup_runs: z.number().int().min(0).max(100).default(1),
});

export const scheduleTriggerSchema = z.object({
  type: z.literal('cron'),
  expression: z.string().trim().min(1).max(120),
  timezone: z.string().trim().min(1).max(120).default('UTC'),
});

/** Schema v3 schedule wire shape (snake_case, trigger/policy/task). */
export const scheduleSchema = z.object({
  id: z.uuid(),
  backend_id: backendIdSchema,
  name: z.string().trim().min(1).max(120),
  trigger: scheduleTriggerSchema,
  policy: schedulePolicySchema.default({
    concurrency: 'forbid',
    misfire: 'run_once',
    max_catchup_runs: 1,
  }),
  enabled: z.boolean().default(true),
  /** Optimistic concurrency token; increments on every update. */
  revision: z.number().int().min(1).default(1),
  /** Task template; backend_id is forced to schedule.backend_id on write. */
  task: scheduleTaskSchema,
  idempotency_key: z.string().trim().min(1).max(200).optional(),
  last_run_at: z.iso.datetime().optional(),
  next_run_at: z.iso.datetime().optional(),
  created_at: z.iso.datetime(),
  updated_at: z.iso.datetime(),
});

const createScheduleBaseSchema = z.object({
  backend_id: backendIdSchema,
  name: z.string().trim().min(1).max(120),
  trigger: scheduleTriggerSchema,
  policy: schedulePolicySchema.default({
    concurrency: 'forbid',
    misfire: 'run_once',
    max_catchup_runs: 1,
  }),
  enabled: z.boolean().default(true),
  task: scheduleTaskSchema,
  idempotency_key: z.string().trim().min(1).max(200).optional(),
});

export const createScheduleSchema = createScheduleBaseSchema;

/** Partial update of schedule fields (backend_id is immutable). */
export const updateScheduleSchema = createScheduleBaseSchema.omit({ backend_id: true }).partial();

/**
 * Schema v3 patch: only `changes` fields are applied.
 * expected_revision enforces optimistic concurrency when provided.
 */
export const patchScheduleSchema = z.object({
  expected_revision: z.number().int().min(1).optional(),
  changes: z
    .object({
      name: z.string().trim().min(1).max(120).optional(),
      enabled: z.boolean().optional(),
      trigger: scheduleTriggerSchema.partial().optional(),
      policy: schedulePolicySchema.partial().optional(),
      task: scheduleTaskSchema.optional(),
    })
    .refine((value) => Object.keys(value).length > 0, {
      message: 'changes must include at least one field',
    }),
  idempotency_key: z.string().trim().min(1).max(200).optional(),
});

/**
 * Backend → control-plane occurrence ack after local CAS claim.
 * CP recomputes authoritative next_run_at; locally_advanced_to is diagnostic only.
 */
export const scheduleOccurrenceAckSchema = z.object({
  backend_id: backendIdSchema,
  schedule_id: z.uuid(),
  revision: z.number().int().min(1),
  /** Canonical UTC ISO of the claimed cursor (CAS token on CP). */
  scheduled_for: z.iso.datetime(),
  /** Backend's local advance result (observability / drift detection only). */
  locally_advanced_to: z.iso.datetime().optional(),
  /** Deterministic occurrence id when a task was enqueued (first slot if multi). */
  occurrence_id: z.string().trim().min(1).max(200).optional(),
  /**
   * How many occurrences the backend enqueued in this claim batch (catch_up).
   * CP advances this many cron steps from scheduled_for when misfire=catch_up.
   */
  enqueued_count: z.number().int().min(0).max(32).optional(),
  /** Wall-clock when the backend claimed (optional diagnostics). */
  claimed_at: z.iso.datetime().optional(),
});

export const scheduleOccurrenceAckResultSchema = z.object({
  accepted: z.boolean(),
  /**
   * cas_applied | already_advanced | revision_mismatch | schedule_disabled |
   * not_found | cursor_mismatch
   */
  status: z.enum([
    'cas_applied',
    'already_advanced',
    'revision_mismatch',
    'schedule_disabled',
    'cursor_mismatch',
  ]),
  schedule_id: z.uuid(),
  revision: z.number().int().min(1),
  next_run_at: z.iso.datetime().optional(),
  last_run_at: z.iso.datetime().optional(),
  /** Present when local advance differs from CP-computed next (drift signal). */
  local_advance_drift: z.boolean().optional(),
});

export type Schedule = z.infer<typeof scheduleSchema>;
export type CreateScheduleInput = z.infer<typeof createScheduleSchema>;
export type UpdateScheduleInput = z.infer<typeof updateScheduleSchema>;
export type PatchScheduleInput = z.infer<typeof patchScheduleSchema>;
export type SchedulePolicy = z.infer<typeof schedulePolicySchema>;
export type ScheduleTrigger = z.infer<typeof scheduleTriggerSchema>;
export type ScheduleOccurrenceAck = z.infer<typeof scheduleOccurrenceAckSchema>;
export type ScheduleOccurrenceAckResult = z.infer<typeof scheduleOccurrenceAckResultSchema>;

/** Normalize schedule task with inherited backend_id for storage/dispatch. */
export function scheduleTaskForBackend(
  backendId: string,
  task: z.infer<typeof scheduleTaskSchema>,
): z.infer<typeof createTaskSchema> {
  return withBackendId(task, backendId);
}

// Re-export for callers that previously imported createTaskSchema via schedule.
export { createTaskSchema, scheduleTaskSchema };
