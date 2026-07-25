import { z } from 'zod';

import { backendIdSchema } from './backend.js';
import { createTaskSchema } from './task.js';

export const scheduleSchema = z.object({
  id: z.uuid(),
  backendId: backendIdSchema,
  name: z.string().trim().min(1).max(120),
  cron: z.string().trim().min(1).max(120),
  timezone: z.string().trim().min(1).max(120).default('UTC'),
  enabled: z.boolean().default(true),
  taskTemplate: createTaskSchema,
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
});
export const updateScheduleSchema = createScheduleSchema.omit({ backendId: true }).partial();

export type Schedule = z.infer<typeof scheduleSchema>;
export type CreateScheduleInput = z.infer<typeof createScheduleSchema>;
export type UpdateScheduleInput = z.infer<typeof updateScheduleSchema>;
