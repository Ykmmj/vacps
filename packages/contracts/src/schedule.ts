import { z } from 'zod';

import { backendIdSchema } from './backend.js';
import { createTaskSchema, scheduleTaskSchema, withBackendId } from './task.js';

export const scheduleConcurrencyPolicies = ['allow', 'forbid', 'replace', 'queue'] as const;
export const scheduleMisfirePolicies = ['skip', 'run_once', 'catch_up'] as const;

export const schedulePolicySchema = z.object({
  concurrency: z.enum(scheduleConcurrencyPolicies).default('forbid'),
  misfire: z.enum(scheduleMisfirePolicies).default('run_once'),
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

export type Schedule = z.infer<typeof scheduleSchema>;
export type CreateScheduleInput = z.infer<typeof createScheduleSchema>;
export type UpdateScheduleInput = z.infer<typeof updateScheduleSchema>;
export type PatchScheduleInput = z.infer<typeof patchScheduleSchema>;
export type SchedulePolicy = z.infer<typeof schedulePolicySchema>;
export type ScheduleTrigger = z.infer<typeof scheduleTriggerSchema>;

/** Normalize schedule task with inherited backend_id for storage/dispatch. */
export function scheduleTaskForBackend(
  backendId: string,
  task: z.infer<typeof scheduleTaskSchema>,
): z.infer<typeof createTaskSchema> {
  return withBackendId(task, backendId);
}

// Re-export for callers that previously imported createTaskSchema via schedule.
export { createTaskSchema, scheduleTaskSchema };
