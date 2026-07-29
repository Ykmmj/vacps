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

/** Public + internal task kind (Schema v3). Replaces legacy type=shell|agent + shell.mode. */
export const taskKindSchema = z.enum(['command', 'shell', 'agent']);
export const taskSourceSchema = z.enum(['mcp', 'web', 'schedule', 'api']);

export const verifySchema = z.discriminatedUnion('mode', [
  z.object({ mode: z.literal('none') }),
  z.object({ mode: z.literal('exit_code') }),
  z.object({ mode: z.literal('command'), command: z.string().trim().min(1).max(16_384) }),
]);

export const retrySchema = z.object({
  attempts: z.number().int().min(1).max(10),
  backoff_seconds: z.number().int().min(0).max(86_400),
});

export const taskOutputOptionsSchema = z
  .object({
    capture_stdout: z.boolean().default(true),
    capture_stderr: z.boolean().default(true),
    preview_max_bytes: z.number().int().min(0).max(1_048_576).default(8192),
    retention_seconds: z.number().int().min(60).max(2_592_000).default(86_400),
    hard_max_bytes: z.number().int().min(0).max(1_073_741_824).default(10_485_760),
  })
  .default({
    capture_stdout: true,
    capture_stderr: true,
    preview_max_bytes: 8192,
    retention_seconds: 86_400,
    hard_max_bytes: 10_485_760,
  });

export const agentPermissionsSchema = z
  .object({
    shell: z.boolean().default(false),
    network: z.boolean().default(false),
    file_write: z.boolean().default(false),
  })
  .default({ shell: false, network: false, file_write: false });

const taskSharedSchema = z.object({
  backend_id: backendIdSchema,
  name: z.string().trim().min(1).max(200).optional(),
  working_directory: z.string().startsWith('/').max(4096).default('/tmp'),
  timeout_seconds: z.number().int().min(1).max(86_400),
  /** Policy profile for non-agent tasks; agent uses its own profile field. */
  profile: z.string().trim().min(1).max(64).default('full'),
  verify: verifySchema.optional(),
  retry: retrySchema.optional(),
  labels: z.record(z.string(), z.string().max(500)).optional(),
  environment: z.record(z.string(), z.string().max(32_768)).optional(),
  idempotency_key: z.string().trim().min(1).max(200).optional(),
  output: taskOutputOptionsSchema,
});

const commandTaskSchema = taskSharedSchema.extend({
  kind: z.literal('command'),
  program: z.string().trim().min(1).max(4096),
  arguments: z.array(z.string().max(65_536)).max(1000).default([]),
});

const shellTaskSchema = taskSharedSchema.extend({
  kind: z.literal('shell'),
  command: z.string().min(1).max(1_048_576),
  shell: z.enum(['/bin/bash', '/bin/sh']).default('/bin/bash'),
  load_user_environment: z.boolean().default(true),
});

const agentTaskSchema = taskSharedSchema.extend({
  kind: z.literal('agent'),
  prompt: z.string().trim().min(1).max(1_048_576),
  profile: z.enum(['restricted', 'diagnostic', 'standard', 'privileged']).default('standard'),
  max_steps: z.number().int().min(1).max(1000).default(20),
  permissions: agentPermissionsSchema,
});

/** Schema v3 create-task wire shape (snake_case, kind-discriminated). */
export const createTaskSchema = z.discriminatedUnion('kind', [
  commandTaskSchema,
  shellTaskSchema,
  agentTaskSchema,
]);

/**
 * Schedule embedded task — inherits schedule.backend_id.
 * No nested backend_id or idempotency_key (schedule-level only).
 */
export const scheduleTaskSchema = z.discriminatedUnion('kind', [
  commandTaskSchema.omit({ backend_id: true, idempotency_key: true }),
  shellTaskSchema.omit({ backend_id: true, idempotency_key: true }),
  agentTaskSchema.omit({ backend_id: true, idempotency_key: true }),
]);

export const taskDispatchSchema = createTaskSchema.and(
  z.object({
    task_id: z.uuid(),
    source: taskSourceSchema,
    schedule_id: z.uuid().optional(),
  }),
);

export const taskSchema = taskDispatchSchema.and(
  z.object({
    status: taskStatusSchema,
    created_at: z.iso.datetime(),
    updated_at: z.iso.datetime(),
    started_at: z.iso.datetime().optional(),
    finished_at: z.iso.datetime().optional(),
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
export type TaskKind = z.infer<typeof taskKindSchema>;
/** @deprecated Use TaskKind — kept as alias during migration of type column. */
export type TaskType = TaskKind;
export type TaskSource = z.infer<typeof taskSourceSchema>;
export type VerifyConfig = z.infer<typeof verifySchema>;
export type TaskOutputOptions = z.infer<typeof taskOutputOptionsSchema>;
export type CreateTaskInput = z.infer<typeof createTaskSchema>;
export type ScheduleTaskInput = z.infer<typeof scheduleTaskSchema>;
export type TaskDispatch = z.infer<typeof taskDispatchSchema>;
export type Task = z.infer<typeof taskSchema>;

/** Resolve working directory for any task kind. */
export function taskWorkingDirectory(task: { working_directory?: string }): string {
  return task.working_directory ?? '/tmp';
}

/** Human-readable summary for indexes / UI. */
export function taskSummary(input: CreateTaskInput | ScheduleTaskInput): string {
  if (input.kind === 'command') {
    return [input.program, ...(input.arguments ?? [])]
      .map(shellQuote)
      .join(' ')
      .replace(/\s+/g, ' ')
      .slice(0, 240);
  }
  if (input.kind === 'shell') {
    return input.command.replace(/\s+/g, ' ').slice(0, 240);
  }
  return input.prompt.replace(/\s+/g, ' ').slice(0, 240);
}

/** Build a display / policy command string for authorize checks. */
export function taskToCommand(input: CreateTaskInput | ScheduleTaskInput): string {
  if (input.kind === 'command') {
    return [input.program, ...(input.arguments ?? [])].map(shellQuote).join(' ');
  }
  if (input.kind === 'shell') {
    return input.command;
  }
  return '(Pi agent task)';
}

/** Attach backend_id to a schedule task template for dispatch. */
export function withBackendId(
  task: ScheduleTaskInput | CreateTaskInput,
  backendId: string,
): CreateTaskInput {
  return createTaskSchema.parse({
    ...task,
    backend_id: backendId,
  });
}

function shellQuote(value: string): string {
  return `'${value.replaceAll("'", `'\\''`)}'`;
}
