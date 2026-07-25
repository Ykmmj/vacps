import { z } from 'zod';

import { backendIdSchema } from './backend.js';

export const taskStatuses = [
  'created',
  'dispatching',
  'queued',
  'running',
  'waiting_for_approval',
  'succeeded',
  'failed',
  'cancelled',
  'timed_out',
  'dispatch_failed',
] as const;
export const taskStatusSchema = z.enum(taskStatuses);
export const taskTypeSchema = z.enum(['shell', 'agent']);
export const profileSchema = z.string().trim().min(1).max(64).default('full');
export const taskSourceSchema = z.enum(['mcp', 'web', 'schedule', 'api']);

export const verifySchema = z.discriminatedUnion('mode', [
  z.object({ mode: z.literal('none') }),
  z.object({ mode: z.literal('exit_code') }),
  z.object({ mode: z.literal('command'), command: z.string().trim().min(1).max(16_384) }),
]);

export const retrySchema = z.object({
  attempts: z.number().int().min(1).max(10),
  backoffSeconds: z.number().int().min(0).max(86_400),
});

const taskBaseSchema = z.object({
  backendId: backendIdSchema,
  profile: profileSchema,
  cwd: z.string().startsWith('/').max(4096),
  timeoutSeconds: z.number().int().min(1).max(86_400),
  verify: verifySchema.optional(),
  retry: retrySchema.optional(),
});

export const createTaskSchema = z.discriminatedUnion('type', [
  taskBaseSchema.extend({
    type: z.literal('shell'),
    command: z.string().trim().min(1).max(65_536),
  }),
  taskBaseSchema.extend({ type: z.literal('agent'), prompt: z.string().trim().min(1).max(65_536) }),
]);

export const taskDispatchSchema = createTaskSchema.and(
  z.object({
    taskId: z.uuid(),
    source: taskSourceSchema,
    scheduleId: z.uuid().optional(),
  }),
);

export const taskSchema = taskDispatchSchema.and(
  z.object({
    status: taskStatusSchema,
    createdAt: z.iso.datetime(),
    updatedAt: z.iso.datetime(),
    startedAt: z.iso.datetime().optional(),
    finishedAt: z.iso.datetime().optional(),
  }),
);

export interface CommandExecution {
  id: string;
  sequence: number;
  command: string;
  cwd: string;
  status: 'running' | 'succeeded' | 'failed' | 'cancelled' | 'timed_out';
  exitCode?: number | null;
  stdoutPath?: string;
  stderrPath?: string;
  startedAt: string;
  finishedAt?: string;
}

export interface TaskError {
  code: string;
  message: string;
}

export type TaskStatus = z.infer<typeof taskStatusSchema>;
export type TaskType = z.infer<typeof taskTypeSchema>;
export type TaskSource = z.infer<typeof taskSourceSchema>;
export type VerifyConfig = z.infer<typeof verifySchema>;
export type CreateTaskInput = z.infer<typeof createTaskSchema>;
export type TaskDispatch = z.infer<typeof taskDispatchSchema>;
export type Task = z.infer<typeof taskSchema>;
