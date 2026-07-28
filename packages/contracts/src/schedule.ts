import { z } from 'zod';

import { backendIdSchema } from './backend.js';
import { createTaskSchema } from './task.js';

export const scheduleConcurrencyPolicies = ['allow', 'forbid', 'replace', 'queue'] as const;
export const scheduleMisfirePolicies = ['skip', 'run_once', 'catch_up'] as const;

export const schedulePolicySchema = z.object({
  concurrency: z.enum(scheduleConcurrencyPolicies).default('forbid'),
  misfire: z.enum(scheduleMisfirePolicies).default('run_once'),
  maxCatchupRuns: z.number().int().min(0).max(100).default(1),
});

export const scheduleSchema = z.object({
  id: z.uuid(),
  backendId: backendIdSchema,
  name: z.string().trim().min(1).max(120),
  cron: z.string().trim().min(1).max(120),
  timezone: z.string().trim().min(1).max(120).default('UTC'),
  enabled: z.boolean().default(true),
  /** Optimistic concurrency token; increments on every update. */
  revision: z.number().int().min(1).default(1),
  policy: schedulePolicySchema.default({
    concurrency: 'forbid',
    misfire: 'run_once',
    maxCatchupRuns: 1,
  }),
  taskTemplate: createTaskSchema,
  idempotencyKey: z.string().trim().min(1).max(200).optional(),
  lastRunAt: z.iso.datetime().optional(),
  nextRunAt: z.iso.datetime().optional(),
  createdAt: z.iso.datetime(),
  updatedAt: z.iso.datetime(),
});

export const createScheduleSchema = scheduleSchema.omit({
  id: true,
  lastRunAt: true,
  nextRunAt: true,
  createdAt: true,
  updatedAt: true,
  revision: true,
});

/** Partial update of schedule fields (backendId is immutable). */
export const updateScheduleSchema = createScheduleSchema.omit({ backendId: true }).partial();

/**
 * Schema v2 patch update: only `changes` fields are applied.
 * expectedRevision enforces optimistic concurrency when provided.
 */
export const patchScheduleSchema = z.object({
  expectedRevision: z.number().int().min(1).optional(),
  changes: z
    .object({
      name: z.string().trim().min(1).max(120).optional(),
      enabled: z.boolean().optional(),
      cron: z.string().trim().min(1).max(120).optional(),
      timezone: z.string().trim().min(1).max(120).optional(),
      policy: schedulePolicySchema.partial().optional(),
      taskTemplate: createTaskSchema.optional(),
    })
    .refine((value) => Object.keys(value).length > 0, {
      message: 'changes must include at least one field',
    }),
  idempotencyKey: z.string().trim().min(1).max(200).optional(),
});

export type Schedule = z.infer<typeof scheduleSchema>;
export type CreateScheduleInput = z.infer<typeof createScheduleSchema>;
export type UpdateScheduleInput = z.infer<typeof updateScheduleSchema>;
export type PatchScheduleInput = z.infer<typeof patchScheduleSchema>;
export type SchedulePolicy = z.infer<typeof schedulePolicySchema>;
