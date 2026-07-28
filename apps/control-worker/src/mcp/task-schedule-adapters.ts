/**
 * Schema v3 MCP task/schedule inputs — same wire shape as @vacps/contracts.
 * No type/mode translation; only page/list helpers and thin result builders.
 */
import {
  createScheduleSchema,
  createTaskSchema,
  patchScheduleSchema,
  scheduleTaskSchema,
  withBackendId,
  type CreateScheduleInput,
  type CreateTaskInput,
  type PatchScheduleInput,
} from '@vacps/contracts';
import { z } from 'zod';

import {
  backendIdSchema,
  cursorSchema,
  idempotencyKeySchema,
  pageLimitSchema,
  scheduleIdSchema,
  sha256Schema,
  taskIdSchema,
} from './schema/defs.js';

// ── Task create tools (identical to contracts createTaskSchema branches) ─

const taskCreateShared = {
  backend_id: backendIdSchema,
  name: z.string().min(1).max(200).optional(),
  working_directory: z.string().startsWith('/').max(4096).optional(),
  timeout_seconds: z.number().int().min(1).max(86_400),
  environment: z.record(z.string(), z.string().max(32_768)).optional(),
  labels: z.record(z.string(), z.string().max(500)).optional(),
  output: z
    .strictObject({
      capture_stdout: z.boolean().optional(),
      capture_stderr: z.boolean().optional(),
      preview_max_bytes: z.number().int().min(0).max(1_048_576).optional(),
      retention_seconds: z.number().int().min(60).max(2_592_000).optional(),
      hard_max_bytes: z.number().int().min(0).max(1_073_741_824).optional(),
    })
    .optional(),
  idempotency_key: idempotencyKeySchema.optional(),
};

export const tasksCreateCommandInputSchema = z.strictObject({
  ...taskCreateShared,
  program: z.string().min(1).max(4096),
  arguments: z.array(z.string().max(100_000)).max(1000).optional(),
});

export const tasksCreateShellInputSchema = z.strictObject({
  ...taskCreateShared,
  command: z.string().min(1).max(1_048_576),
  shell: z.enum(['/bin/bash', '/bin/sh']).optional(),
  load_user_environment: z.boolean().optional(),
});

export const tasksCreateAgentInputSchema = z.strictObject({
  ...taskCreateShared,
  prompt: z.string().min(1).max(1_048_576),
  profile: z.enum(['restricted', 'diagnostic', 'standard', 'privileged']).optional(),
  max_steps: z.number().int().min(1).max(1000).optional(),
  permissions: z
    .strictObject({
      shell: z.boolean().optional(),
      network: z.boolean().optional(),
      file_write: z.boolean().optional(),
    })
    .optional(),
});

export const tasksListInputSchema = z.strictObject({
  backend_id: backendIdSchema.optional(),
  kind: z.enum(['shell', 'agent', 'command']).optional(),
  status: z.string().min(1).max(64).optional(),
  created_after: z.string().min(1).max(64).optional(),
  limit: pageLimitSchema.optional(),
  cursor: cursorSchema.optional(),
});

export const tasksGetInputSchema = z.strictObject({
  task_id: taskIdSchema,
  include_commands: z.boolean().optional(),
  include_output_preview: z.boolean().optional(),
  preview_max_bytes: z.number().int().min(0).max(1_048_576).optional(),
});

export const tasksOutputReadInputSchema = z.strictObject({
  task_id: taskIdSchema,
  stream: z.enum(['stdout', 'stderr']).optional(),
  offset: z.number().int().min(0).optional(),
  max_bytes: z.number().int().min(1).max(1_048_576).optional(),
  expected_stream_version: sha256Schema.optional(),
});

export const tasksIdInputSchema = z.strictObject({
  task_id: taskIdSchema,
  idempotency_key: idempotencyKeySchema.optional(),
});

// ── Schedules (same as createScheduleSchema / patchScheduleSchema) ──────

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
  expected_revision: z.number().int().min(1).optional(),
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

/** MCP create_command → contracts CreateTaskInput (kind already V3). */
export function toCreateCommandTask(
  input: z.infer<typeof tasksCreateCommandInputSchema>,
): CreateTaskInput {
  return createTaskSchema.parse({
    kind: 'command',
    backend_id: input.backend_id,
    program: input.program,
    arguments: input.arguments ?? [],
    ...(input.name ? { name: input.name } : {}),
    ...(input.working_directory ? { working_directory: input.working_directory } : {}),
    timeout_seconds: input.timeout_seconds,
    ...(input.environment ? { environment: input.environment } : {}),
    ...(input.labels ? { labels: input.labels } : {}),
    ...(input.output ? { output: input.output } : {}),
    ...(input.idempotency_key ? { idempotency_key: input.idempotency_key } : {}),
  });
}

export function toCreateShellTask(
  input: z.infer<typeof tasksCreateShellInputSchema>,
): CreateTaskInput {
  return createTaskSchema.parse({
    kind: 'shell',
    backend_id: input.backend_id,
    command: input.command,
    ...(input.shell ? { shell: input.shell } : {}),
    ...(typeof input.load_user_environment === 'boolean'
      ? { load_user_environment: input.load_user_environment }
      : {}),
    ...(input.name ? { name: input.name } : {}),
    ...(input.working_directory ? { working_directory: input.working_directory } : {}),
    timeout_seconds: input.timeout_seconds,
    ...(input.environment ? { environment: input.environment } : {}),
    ...(input.labels ? { labels: input.labels } : {}),
    ...(input.output ? { output: input.output } : {}),
    ...(input.idempotency_key ? { idempotency_key: input.idempotency_key } : {}),
  });
}

export function toCreateAgentTask(
  input: z.infer<typeof tasksCreateAgentInputSchema>,
): CreateTaskInput {
  return createTaskSchema.parse({
    kind: 'agent',
    backend_id: input.backend_id,
    prompt: input.prompt,
    ...(input.profile ? { profile: input.profile } : {}),
    ...(input.max_steps !== undefined ? { max_steps: input.max_steps } : {}),
    ...(input.permissions
      ? {
          permissions: {
            shell: Boolean(input.permissions.shell),
            network: Boolean(input.permissions.network),
            file_write: Boolean(input.permissions.file_write),
          },
        }
      : {}),
    ...(input.name ? { name: input.name } : {}),
    ...(input.working_directory ? { working_directory: input.working_directory } : {}),
    timeout_seconds: input.timeout_seconds,
    ...(input.environment ? { environment: input.environment } : {}),
    ...(input.labels ? { labels: input.labels } : {}),
    ...(input.output ? { output: input.output } : {}),
    ...(input.idempotency_key ? { idempotency_key: input.idempotency_key } : {}),
  });
}

/** MCP schedules.create → contracts CreateScheduleInput (same shape). */
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

/** MCP schedules.update → contracts PatchScheduleInput. */
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

export function taskCreateResult(
  created: {
    id: string;
    backendId: string;
    kind?: string;
    status: string;
    createdAt: string;
    name?: string;
    summary?: string;
    reusedExistingTask?: boolean;
    idempotencyKey?: string;
    requestHash?: string;
  },
  inputKey?: string | null,
  publicKind?: 'command' | 'shell' | 'agent',
) {
  const kind = publicKind ?? created.kind ?? 'command';
  return {
    task: {
      id: created.id,
      backend_id: created.backendId,
      kind,
      name: created.name ?? null,
      summary: created.summary ?? null,
      status: created.status,
      created_at: created.createdAt,
      cancellable: !['succeeded', 'failed', 'cancelled', 'timed_out', 'dispatch_failed'].includes(
        created.status,
      ),
    },
    output: {
      stdout: { available: false, bytes: 0, complete: false },
      stderr: { available: false, bytes: 0, complete: false },
    },
    poll: { tool: 'vacps.tasks.get', recommended_after_ms: 500 },
    idempotency: {
      key: inputKey ?? created.idempotencyKey ?? null,
      replayed: Boolean(created.reusedExistingTask),
      request_hash: created.requestHash ?? null,
    },
  };
}

export { withBackendId, createTaskSchema };
