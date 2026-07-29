/**
 * Schema v3 MCP schedule tool inputs — same wire shape as @vacps/contracts.
 */
import {
  createScheduleSchema,
  patchScheduleSchema,
  scheduleTaskSchema,
  withBackendId,
  type CreateScheduleInput,
  type PatchScheduleInput,
} from '@vacps/contracts';
import { z } from 'zod';

import {
  backendIdSchema,
  cursorSchema,
  idempotencyKeySchema,
  pageLimitSchema,
  scheduleIdSchema,
} from './defs.js';

export const schedulesCreateInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  name: z.string().min(1).max(120),
  trigger: z.strictObject({
    type: z.literal('cron'),
    expression: z.string().min(1).max(120),
    timezone: z.string().min(1).max(120).optional(),
  }),
  policy: z
    .strictObject({
      concurrency: z.enum(['allow', 'forbid', 'replace', 'queue']).optional(),
      misfire: z.enum(['skip', 'run_once', 'catch_up']).optional(),
      max_catchup_runs: z.number().int().min(0).max(100).optional(),
    })
    .optional(),
  enabled: z.boolean().optional(),
  task: scheduleTaskSchema,
  idempotency_key: idempotencyKeySchema.optional(),
});

export const schedulesUpdateInputSchema = z.strictObject({
  schedule_id: scheduleIdSchema,
  expected_revision: z.number().int().min(1).max(2_147_483_647).optional(),
  changes: z
    .strictObject({
      enabled: z.boolean().optional(),
      name: z.string().min(1).max(120).optional(),
      trigger: z
        .strictObject({
          type: z.literal('cron').optional(),
          expression: z.string().min(1).max(120).optional(),
          timezone: z.string().min(1).max(120).optional(),
        })
        .optional(),
      policy: z
        .strictObject({
          concurrency: z.enum(['allow', 'forbid', 'replace', 'queue']).optional(),
          misfire: z.enum(['skip', 'run_once', 'catch_up']).optional(),
          max_catchup_runs: z.number().int().min(0).max(100).optional(),
        })
        .optional(),
      task: scheduleTaskSchema.optional(),
    })
    .refine((value) => Object.keys(value).length > 0, {
      message: 'changes must include at least one field',
    }),
  idempotency_key: idempotencyKeySchema.optional(),
});

export const schedulesGetInputSchema = z.strictObject({
  schedule_id: scheduleIdSchema,
});

export const schedulesIdInputSchema = z.strictObject({
  schedule_id: scheduleIdSchema,
  idempotency_key: idempotencyKeySchema.optional(),
});

export const schedulesListInputSchema = z.strictObject({
  backend_id: backendIdSchema.optional(),
  enabled: z.boolean().optional(),
  limit: pageLimitSchema.optional(),
  cursor: cursorSchema.optional(),
});

export function parseScheduleCreate(
  input: z.infer<typeof schedulesCreateInputSchema>,
): CreateScheduleInput {
  return createScheduleSchema.parse({
    backend_id: input.backend_id,
    name: input.name,
    trigger: {
      type: 'cron' as const,
      expression: input.trigger.expression,
      timezone: input.trigger.timezone ?? 'UTC',
    },
    ...(input.policy
      ? {
          policy: {
            ...(input.policy.concurrency ? { concurrency: input.policy.concurrency } : {}),
            ...(input.policy.misfire ? { misfire: input.policy.misfire } : {}),
            ...(input.policy.max_catchup_runs !== undefined
              ? { max_catchup_runs: input.policy.max_catchup_runs }
              : {}),
          },
        }
      : {}),
    enabled: input.enabled ?? true,
    task: input.task,
    ...(input.idempotency_key ? { idempotency_key: input.idempotency_key } : {}),
  });
}

export function parseSchedulePatch(
  input: z.infer<typeof schedulesUpdateInputSchema>,
  _backendId: string,
): PatchScheduleInput {
  return patchScheduleSchema.parse({
    ...(input.expected_revision !== undefined
      ? { expected_revision: input.expected_revision }
      : {}),
    changes: {
      ...(input.changes.name !== undefined ? { name: input.changes.name } : {}),
      ...(input.changes.enabled !== undefined ? { enabled: input.changes.enabled } : {}),
      ...(input.changes.trigger
        ? {
            trigger: {
              ...(input.changes.trigger.type ? { type: input.changes.trigger.type } : {}),
              ...(input.changes.trigger.expression
                ? { expression: input.changes.trigger.expression }
                : {}),
              ...(input.changes.trigger.timezone
                ? { timezone: input.changes.trigger.timezone }
                : {}),
            },
          }
        : {}),
      ...(input.changes.policy
        ? {
            policy: {
              ...(input.changes.policy.concurrency
                ? { concurrency: input.changes.policy.concurrency }
                : {}),
              ...(input.changes.policy.misfire ? { misfire: input.changes.policy.misfire } : {}),
              ...(input.changes.policy.max_catchup_runs !== undefined
                ? { max_catchup_runs: input.changes.policy.max_catchup_runs }
                : {}),
            },
          }
        : {}),
      ...(input.changes.task ? { task: input.changes.task } : {}),
    },
    ...(input.idempotency_key ? { idempotency_key: input.idempotency_key } : {}),
  });
}

export { withBackendId };
