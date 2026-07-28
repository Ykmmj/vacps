/**
 * Map Schema v2 MCP task/schedule tool inputs → internal CreateTaskInput / CreateScheduleInput.
 * Keeps nested optional-branch tools (create_command / create_shell / create_agent) model-friendly.
 */
import {
  createScheduleSchema,
  createTaskSchema,
  schedulePolicySchema,
  type CreateScheduleInput,
  type CreateTaskInput,
  type PatchScheduleInput,
  type SchedulePolicy,
} from '@vacps/contracts';
import { z } from 'zod';

import { AppError } from '../lib/http.js';
import {
  argumentsSchema,
  backendIdSchema,
  commandSchema,
  cursorSchema,
  environmentSchema,
  idempotencyKeySchema,
  labelsSchema,
  pageLimitSchema,
  pathSchema,
  programSchema,
  scheduleIdSchema,
  taskIdSchema,
} from './schema/defs.js';

const outputOptionsMcp = z
  .strictObject({
    capture_stdout: z.boolean().optional(),
    capture_stderr: z.boolean().optional(),
    preview_max_bytes: z.number().int().min(0).max(1_048_576).optional(),
    retention_seconds: z.number().int().min(60).max(2_592_000).optional(),
    hard_max_bytes: z.number().int().min(0).max(1_073_741_824).optional(),
  })
  .optional();

const taskSharedFields = {
  backend_id: backendIdSchema,
  name: z.string().min(1).max(200).optional(),
  working_directory: pathSchema.optional(),
  timeout_seconds: z.number().int().min(1).max(86_400),
  environment: environmentSchema.optional(),
  labels: labelsSchema.optional(),
  output: outputOptionsMcp,
  idempotency_key: idempotencyKeySchema.optional(),
};

/** Flat argv program task (maps to shell mode=exec). */
export const tasksCreateCommandInputSchema = z.strictObject({
  ...taskSharedFields,
  program: programSchema,
  arguments: argumentsSchema.optional(),
});

/** Shell string task (maps to shell mode=script with bash -lc by default). */
export const tasksCreateShellInputSchema = z.strictObject({
  ...taskSharedFields,
  command: commandSchema,
  shell: z.enum(['/bin/bash', '/bin/sh']).optional(),
  load_user_environment: z.boolean().optional(),
});

/** Agent prompt task. */
export const tasksCreateAgentInputSchema = z.strictObject({
  ...taskSharedFields,
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

/** Legacy combined create — kept for one migration cycle. */
export const tasksCreateLegacyInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  type: z.enum(['shell', 'agent']),
  name: z.string().min(1).max(200).optional(),
  working_directory: pathSchema.optional(),
  timeout_seconds: z.number().int().min(1).max(86_400),
  profile: z.string().min(1).max(64).optional(),
  idempotency_key: idempotencyKeySchema.optional(),
  labels: labelsSchema.optional(),
  environment: environmentSchema.optional(),
  shell: z
    .strictObject({
      mode: z.enum(['exec', 'script']),
      program: programSchema.optional(),
      arguments: argumentsSchema.optional(),
      interpreter: programSchema.optional(),
      interpreter_arguments: z.array(z.string().max(4096)).max(20).optional(),
      content: z.string().max(1_048_576).optional(),
    })
    .optional(),
  agent: z
    .strictObject({
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
    })
    .optional(),
  output: outputOptionsMcp,
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
  expected_stream_version: z.string().min(1).max(128).optional(),
});

export const tasksIdInputSchema = z.strictObject({
  task_id: taskIdSchema,
  idempotency_key: idempotencyKeySchema.optional(),
});

const scheduleTaskCommand = z.strictObject({
  kind: z.literal('command'),
  name: z.string().min(1).max(200).optional(),
  program: programSchema,
  arguments: argumentsSchema.optional(),
  working_directory: pathSchema.optional(),
  environment: environmentSchema.optional(),
  timeout_seconds: z.number().int().min(1).max(86_400),
  output: outputOptionsMcp,
  labels: labelsSchema.optional(),
});

const scheduleTaskShell = z.strictObject({
  kind: z.literal('shell'),
  name: z.string().min(1).max(200).optional(),
  command: commandSchema,
  shell: z.enum(['/bin/bash', '/bin/sh']).optional(),
  load_user_environment: z.boolean().optional(),
  working_directory: pathSchema.optional(),
  environment: environmentSchema.optional(),
  timeout_seconds: z.number().int().min(1).max(86_400),
  output: outputOptionsMcp,
  labels: labelsSchema.optional(),
});

const scheduleTaskAgent = z.strictObject({
  kind: z.literal('agent'),
  name: z.string().min(1).max(200).optional(),
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
  working_directory: pathSchema.optional(),
  environment: environmentSchema.optional(),
  timeout_seconds: z.number().int().min(1).max(86_400),
  output: outputOptionsMcp,
  labels: labelsSchema.optional(),
});

const scheduleTaskSchema = z.discriminatedUnion('kind', [
  scheduleTaskCommand,
  scheduleTaskShell,
  scheduleTaskAgent,
]);

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
  /** Task template — must NOT include backend_id (inherits schedule.backend_id). */
  task: scheduleTaskSchema,
  idempotency_key: idempotencyKeySchema.optional(),
});

/** Legacy flat create still accepted during migration. */
export const schedulesCreateLegacyInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  name: z.string().min(1).max(120),
  cron: z.string().min(1).max(120),
  timezone: z.string().min(1).max(120).optional(),
  enabled: z.boolean().optional(),
  task_template: z.record(z.string(), z.unknown()),
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
      // Legacy field names still accepted inside changes.
      cron: z.string().min(1).max(120).optional(),
      timezone: z.string().min(1).max(120).optional(),
      task_template: z.record(z.string(), z.unknown()).optional(),
    })
    .refine((value) => Object.keys(value).length > 0, {
      message: 'changes must include at least one field',
    }),
  idempotency_key: idempotencyKeySchema.optional(),
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

function mapOutput(output: z.infer<typeof outputOptionsMcp>) {
  if (!output) return undefined;
  return {
    ...(typeof output.capture_stdout === 'boolean' ? { captureStdout: output.capture_stdout } : {}),
    ...(typeof output.capture_stderr === 'boolean' ? { captureStderr: output.capture_stderr } : {}),
    ...(typeof output.preview_max_bytes === 'number'
      ? { previewMaxBytes: output.preview_max_bytes }
      : {}),
    ...(typeof output.retention_seconds === 'number'
      ? { retentionSeconds: output.retention_seconds }
      : {}),
    ...(typeof output.hard_max_bytes === 'number' ? { hardMaxBytes: output.hard_max_bytes } : {}),
  };
}

function baseTaskFields(input: {
  backend_id: string;
  name?: string | undefined;
  working_directory?: string | undefined;
  timeout_seconds: number;
  environment?: Record<string, string> | undefined;
  labels?: Record<string, string> | undefined;
  output?: z.infer<typeof outputOptionsMcp>;
  idempotency_key?: string | undefined;
  profile?: string | undefined;
}) {
  return {
    backendId: input.backend_id,
    ...(input.name ? { name: input.name } : {}),
    cwd: input.working_directory ?? '/tmp',
    timeoutSeconds: input.timeout_seconds,
    ...(input.profile ? { profile: input.profile } : {}),
    ...(input.idempotency_key ? { idempotencyKey: input.idempotency_key } : {}),
    ...(input.labels ? { labels: input.labels } : {}),
    ...(input.environment ? { environment: input.environment } : {}),
    ...(input.output ? { output: mapOutput(input.output) } : {}),
  };
}

export function toCreateCommandTask(
  input: z.infer<typeof tasksCreateCommandInputSchema>,
): CreateTaskInput {
  return createTaskSchema.parse({
    ...baseTaskFields(input),
    type: 'shell',
    shell: {
      mode: 'exec',
      program: input.program,
      arguments: input.arguments ?? [],
    },
  });
}

export function toCreateShellTask(
  input: z.infer<typeof tasksCreateShellInputSchema>,
): CreateTaskInput {
  const shell = input.shell ?? '/bin/bash';
  const loadUser = input.load_user_environment !== false;
  // bash -lc loads login + rc; sh -c does not.
  const interpreterArguments =
    shell === '/bin/bash' ? (loadUser ? ['-lc'] : ['-c']) : loadUser ? ['-lc'] : ['-c'];
  return createTaskSchema.parse({
    ...baseTaskFields(input),
    type: 'shell',
    shell: {
      mode: 'script',
      interpreter: shell,
      interpreterArguments,
      content: input.command,
    },
  });
}

export function toCreateAgentTask(
  input: z.infer<typeof tasksCreateAgentInputSchema>,
): CreateTaskInput {
  const permissions = input.permissions ?? {};
  return createTaskSchema.parse({
    ...baseTaskFields({
      ...input,
      profile: input.profile ?? 'standard',
    }),
    type: 'agent',
    agent: {
      prompt: input.prompt,
      ...(input.profile ? { profile: input.profile } : {}),
      ...(input.max_steps !== undefined ? { maxSteps: input.max_steps } : {}),
      permissions: {
        shell: Boolean(permissions.shell),
        network: Boolean(permissions.network),
        fileWrite: Boolean(permissions.file_write),
      },
    },
  });
}

/** Legacy vacps.tasks.create parser. */
export function parseLegacyTaskCreate(raw: z.infer<typeof tasksCreateLegacyInputSchema>): CreateTaskInput {
  const base = baseTaskFields(raw);
  if (raw.type === 'shell') {
    const shell = raw.shell;
    if (!shell) throw new AppError('validation_error', 'shell is required for type=shell.', 400);
    if (shell.mode === 'exec') {
      return createTaskSchema.parse({
        ...base,
        type: 'shell',
        shell: {
          mode: 'exec',
          program: shell.program ?? '',
          arguments: shell.arguments ?? [],
        },
      });
    }
    return createTaskSchema.parse({
      ...base,
      type: 'shell',
      shell: {
        mode: 'script',
        interpreter: shell.interpreter ?? '/bin/bash',
        interpreterArguments: shell.interpreter_arguments ?? ['-c'],
        content: shell.content ?? '',
      },
    });
  }
  const agent = raw.agent;
  if (!agent) throw new AppError('validation_error', 'agent is required for type=agent.', 400);
  const permissions = agent.permissions ?? {};
  return createTaskSchema.parse({
    ...base,
    type: 'agent',
    ...(typeof raw.profile === 'string' ? { profile: raw.profile } : {}),
    agent: {
      prompt: agent.prompt,
      ...(agent.profile ? { profile: agent.profile } : {}),
      ...(agent.max_steps !== undefined ? { maxSteps: agent.max_steps } : {}),
      permissions: {
        shell: Boolean(permissions.shell),
        network: Boolean(permissions.network),
        fileWrite: Boolean(permissions.file_write),
      },
    },
  });
}

function scheduleTaskToCreateTask(
  backendId: string,
  task: z.infer<typeof scheduleTaskSchema>,
): CreateTaskInput {
  if (task.kind === 'command') {
    return toCreateCommandTask({
      backend_id: backendId,
      name: task.name,
      program: task.program,
      arguments: task.arguments,
      working_directory: task.working_directory,
      environment: task.environment,
      timeout_seconds: task.timeout_seconds,
      output: task.output,
      labels: task.labels,
    });
  }
  if (task.kind === 'shell') {
    return toCreateShellTask({
      backend_id: backendId,
      name: task.name,
      command: task.command,
      shell: task.shell,
      load_user_environment: task.load_user_environment,
      working_directory: task.working_directory,
      environment: task.environment,
      timeout_seconds: task.timeout_seconds,
      output: task.output,
      labels: task.labels,
    });
  }
  return toCreateAgentTask({
    backend_id: backendId,
    name: task.name,
    prompt: task.prompt,
    profile: task.profile,
    max_steps: task.max_steps,
    permissions: task.permissions,
    working_directory: task.working_directory,
    environment: task.environment,
    timeout_seconds: task.timeout_seconds,
    output: task.output,
    labels: task.labels,
  });
}

function mapPolicy(
  policy?: {
    concurrency?: 'allow' | 'forbid' | 'replace' | 'queue' | undefined;
    misfire?: 'skip' | 'run_once' | 'catch_up' | undefined;
    max_catchup_runs?: number | undefined;
  },
): SchedulePolicy {
  return schedulePolicySchema.parse({
    concurrency: policy?.concurrency ?? 'forbid',
    misfire: policy?.misfire ?? 'run_once',
    maxCatchupRuns: policy?.max_catchup_runs ?? 1,
  });
}

export function parseScheduleCreateV2(
  input: z.infer<typeof schedulesCreateInputSchema>,
): CreateScheduleInput {
  // Task template must not re-specify backend; inherit schedule.backend_id.
  const taskTemplate = scheduleTaskToCreateTask(input.backend_id, input.task);
  return createScheduleSchema.parse({
    backendId: input.backend_id,
    name: input.name,
    cron: input.trigger.expression,
    timezone: input.trigger.timezone ?? 'UTC',
    enabled: input.enabled ?? true,
    policy: mapPolicy(input.policy ?? undefined),
    taskTemplate,
    ...(input.idempotency_key ? { idempotencyKey: input.idempotency_key } : {}),
  });
}

export function parseScheduleCreateLegacy(
  raw: z.infer<typeof schedulesCreateLegacyInputSchema>,
): CreateScheduleInput {
  const templateRaw = { ...raw.task_template } as Record<string, unknown>;
  // Strip nested backend_id if present; always use top-level.
  if (templateRaw.backend_id && String(templateRaw.backend_id) !== raw.backend_id) {
    throw new AppError(
      'validation_error',
      'task_template.backend_id must match schedule backend_id or be omitted.',
      400,
    );
  }
  templateRaw.backend_id = raw.backend_id;
  const taskTemplate = parseLegacyTaskCreate(
    tasksCreateLegacyInputSchema.parse({
      ...templateRaw,
      backend_id: raw.backend_id,
      timeout_seconds: Number(templateRaw.timeout_seconds ?? 600),
      type: templateRaw.type ?? 'shell',
    }),
  );
  return createScheduleSchema.parse({
    backendId: raw.backend_id,
    name: raw.name,
    cron: raw.cron,
    timezone: raw.timezone ?? 'UTC',
    enabled: raw.enabled ?? true,
    taskTemplate,
    ...(raw.idempotency_key ? { idempotencyKey: raw.idempotency_key } : {}),
  });
}

export function parseSchedulePatch(
  input: z.infer<typeof schedulesUpdateInputSchema>,
  backendId: string,
): PatchScheduleInput {
  const changes: PatchScheduleInput['changes'] = {};
  if (input.changes.name !== undefined) changes.name = input.changes.name;
  if (input.changes.enabled !== undefined) changes.enabled = input.changes.enabled;
  if (input.changes.trigger?.expression) changes.cron = input.changes.trigger.expression;
  if (input.changes.trigger?.timezone) changes.timezone = input.changes.trigger.timezone;
  if (input.changes.cron) changes.cron = input.changes.cron;
  if (input.changes.timezone) changes.timezone = input.changes.timezone;
  if (input.changes.policy) {
    changes.policy = {
      ...(input.changes.policy.concurrency
        ? { concurrency: input.changes.policy.concurrency }
        : {}),
      ...(input.changes.policy.misfire ? { misfire: input.changes.policy.misfire } : {}),
      ...(input.changes.policy.max_catchup_runs !== undefined
        ? { maxCatchupRuns: input.changes.policy.max_catchup_runs }
        : {}),
    };
  }
  if (input.changes.task) {
    changes.taskTemplate = scheduleTaskToCreateTask(backendId, input.changes.task);
  } else if (input.changes.task_template) {
    const templateRaw = { ...input.changes.task_template } as Record<string, unknown>;
    if (templateRaw.backend_id && String(templateRaw.backend_id) !== backendId) {
      throw new AppError(
        'validation_error',
        'task_template.backend_id must match schedule backend_id or be omitted.',
        400,
      );
    }
    changes.taskTemplate = parseLegacyTaskCreate(
      tasksCreateLegacyInputSchema.parse({
        ...templateRaw,
        backend_id: backendId,
        timeout_seconds: Number(templateRaw.timeout_seconds ?? 600),
        type: templateRaw.type ?? 'shell',
      }),
    );
  }
  return {
    ...(input.expected_revision !== undefined
      ? { expectedRevision: input.expected_revision }
      : {}),
    changes,
    ...(input.idempotency_key ? { idempotencyKey: input.idempotency_key } : {}),
  };
}

export function taskCreateResult(
  created: {
    id: string;
    backendId: string;
    type: string;
    status: string;
    createdAt: string;
    name?: string;
    summary?: string;
    reusedExistingTask?: boolean;
    idempotencyKey?: string;
    requestHash?: string;
  },
  inputKey?: string | null,
) {
  return {
    task: {
      id: created.id,
      backend_id: created.backendId,
      kind: created.type,
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
