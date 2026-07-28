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

export const shellSpecSchema = z.discriminatedUnion('mode', [
  z.object({
    mode: z.literal('exec'),
    program: z.string().trim().min(1).max(4096),
    arguments: z.array(z.string().max(65_536)).max(1000).default([]),
  }),
  z.object({
    mode: z.literal('script'),
    interpreter: z.string().trim().min(1).max(4096),
    interpreterArguments: z.array(z.string().max(4096)).max(20).default(['-c']),
    content: z.string().min(1).max(1_048_576),
  }),
]);

export const agentSpecSchema = z.object({
  prompt: z.string().trim().min(1).max(1_048_576),
  profile: z.enum(['restricted', 'diagnostic', 'standard', 'privileged']).default('standard'),
  maxSteps: z.number().int().min(1).max(1000).default(20),
  permissions: z
    .object({
      shell: z.boolean().default(false),
      network: z.boolean().default(false),
      fileWrite: z.boolean().default(false),
    })
    .default({ shell: false, network: false, fileWrite: false }),
});

export const taskOutputOptionsSchema = z
  .object({
    captureStdout: z.boolean().default(true),
    captureStderr: z.boolean().default(true),
    previewMaxBytes: z.number().int().min(0).max(65_536).default(8192),
    retentionSeconds: z.number().int().min(60).max(2_592_000).default(86_400),
    hardMaxBytes: z.number().int().min(1024).max(104_857_600).default(10_485_760),
  })
  .default({
    captureStdout: true,
    captureStderr: true,
    previewMaxBytes: 8192,
    retentionSeconds: 86_400,
    hardMaxBytes: 10_485_760,
  });

const taskBaseSchema = z.object({
  backendId: backendIdSchema,
  name: z.string().trim().min(1).max(200).optional(),
  cwd: z.string().startsWith('/').max(4096).default('/tmp'),
  timeoutSeconds: z.number().int().min(1).max(86_400),
  profile: profileSchema,
  verify: verifySchema.optional(),
  retry: retrySchema.optional(),
  labels: z.record(z.string(), z.string().max(500)).optional(),
  environment: z.record(z.string(), z.string().max(32_768)).optional(),
  idempotencyKey: z.string().trim().min(1).max(200).optional(),
  output: taskOutputOptionsSchema,
});

export const createTaskSchema = z.discriminatedUnion('type', [
  taskBaseSchema.extend({
    type: z.literal('shell'),
    shell: shellSpecSchema,
  }),
  taskBaseSchema.extend({
    type: z.literal('agent'),
    agent: agentSpecSchema,
  }),
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
export type ShellSpec = z.infer<typeof shellSpecSchema>;
export type AgentSpec = z.infer<typeof agentSpecSchema>;
export type TaskOutputOptions = z.infer<typeof taskOutputOptionsSchema>;
export type CreateTaskInput = z.infer<typeof createTaskSchema>;
export type TaskDispatch = z.infer<typeof taskDispatchSchema>;
export type Task = z.infer<typeof taskSchema>;

/** Build a bash -lc command string from a structured shell spec. */
export function shellToCommand(shell: ShellSpec): string {
  if (shell.mode === 'exec') {
    return [shell.program, ...shell.arguments].map(shellQuote).join(' ');
  }
  const args = shell.interpreterArguments.map(shellQuote).join(' ');
  return `${shellQuote(shell.interpreter)} ${args} ${shellQuote(shell.content)}`.trim();
}

export function taskSummary(input: CreateTaskInput): string {
  if (input.type === 'shell') {
    return shellToCommand(input.shell).replace(/\s+/g, ' ').slice(0, 240);
  }
  return input.agent.prompt.replace(/\s+/g, ' ').slice(0, 240);
}

function shellQuote(value: string): string {
  return `'${value.replaceAll("'", `'\\''`)}'`;
}
