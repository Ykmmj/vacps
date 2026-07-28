import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import {
  createScheduleSchema,
  createTaskSchema,
  taskStatuses,
  type CreateScheduleInput,
  type CreateTaskInput,
} from '@vacps/contracts';
import { z } from 'zod';

import type { Env } from '../env.js';
import { AppError } from '../lib/http.js';
import { BackendClient } from '../registry/backend-client.js';
import { BackendRepository } from '../registry/repository.js';
import { ScheduleService } from '../schedules/schedule-service.js';
import { TaskService } from '../tasks/task-service.js';

const SCHEMA_VERSION = '1.0';

const okEnvelope = z.looseObject({
  ok: z.literal(true),
  schema_version: z.string(),
  request_id: z.string(),
  generated_at: z.string(),
});

export function createMcpServer(env: Env): McpServer {
  const backends = new BackendRepository(env.DB);
  const client = new BackendClient(env.CONTROL_PLANE_SIGNING_PRIVATE_KEY);
  const tasks = new TaskService(env.DB, backends, client);
  const schedules = new ScheduleService(env.DB, backends, client, tasks);
  const server = new McpServer({ name: 'vacps', version: '0.1.0' });

  const meta = () => ({
    ok: true as const,
    schema_version: SCHEMA_VERSION,
    request_id: crypto.randomUUID(),
    generated_at: new Date().toISOString(),
  });

  const ok = (value: Record<string, unknown>, summary: string) => ({
    structuredContent: { ...meta(), ...value },
    content: [{ type: 'text' as const, text: summary }],
    isError: false as const,
  });

  const fail = (error: unknown) => {
    const app =
      error instanceof AppError
        ? error
        : error instanceof z.ZodError
          ? new AppError('validation_error', error.issues[0]?.message ?? 'Invalid request.', 400)
          : new AppError(
              'internal_error',
              error instanceof Error ? error.message : String(error),
              500,
            );
    const payload = {
      ok: false as const,
      schema_version: SCHEMA_VERSION,
      request_id: crypto.randomUUID(),
      generated_at: new Date().toISOString(),
      error: {
        code: app.code,
        message: app.message,
        category: categoryFor(app.code),
        retryable: isRetryable(app.code),
        details: {},
      },
    };
    return {
      structuredContent: payload,
      content: [{ type: 'text' as const, text: `${app.code}: ${app.message}` }],
      isError: true as const,
    };
  };

  const wrap =
    <T extends Record<string, unknown>>(
      summary: (value: T) => string,
      run: (args: Record<string, unknown>) => Promise<T>,
    ) =>
    async (args: Record<string, unknown>) => {
      try {
        const value = await run(args);
        return ok(value, summary(value));
      } catch (error) {
        return fail(error);
      }
    };

  // ── Layer B: backends ──────────────────────────────────────────────
  server.registerTool(
    'vacps.backends.list',
    {
      description: 'List VACPS backends (nodes).',
      outputSchema: okEnvelope.extend({ backends: z.array(z.unknown()) }).shape,
    },
    wrap(
      (value) => `Backends: ${(value.backends as unknown[]).length}`,
      async () => ({ backends: snakeBackends(await backends.list()) }),
    ),
  );

  server.registerTool(
    'vacps.backends.get_status',
    {
      description: 'Get live health and metrics for one backend.',
      inputSchema: { backend_id: z.string() },
      outputSchema: okEnvelope.extend({ backend_id: z.string(), status: z.unknown() }).shape,
    },
    wrap(
      (value) => `Status for ${value.backend_id}`,
      async ({ backend_id }) => {
        const backend = await backends.get(String(backend_id));
        const status = await client.status(backend);
        await backends.recordStatus(backend.id, status, { preserveSystem: true });
        return { backend_id: backend.id, status: snakeStatus(status) };
      },
    ),
  );

  // ── Layer B: tasks ─────────────────────────────────────────────────
  server.registerTool(
    'vacps.tasks.create',
    {
      description:
        'Queue a shell or agent task on a backend. Returns immediately with a small task summary.',
      inputSchema: taskCreateInputSchema,
      outputSchema: okEnvelope.extend({
        task: z.unknown(),
        output: z.unknown(),
        poll: z.unknown(),
        idempotency: z.unknown().optional(),
      }).shape,
    },
    wrap(
      (value) => {
        const task = value.task as { id?: string; status?: string };
        return `Task ${task.id ?? '?'} ${task.status ?? 'queued'}`;
      },
      async (raw) => {
        const input = parseTaskCreateInput(raw);
        const created = await tasks.create(input, 'mcp');
        return {
          task: snakeTask(created),
          output: {
            stdout: { available: false, bytes: 0, complete: false },
            stderr: { available: false, bytes: 0, complete: false },
          },
          poll: { tool: 'vacps.tasks.get', recommended_after_ms: 500 },
          idempotency: {
            key: input.idempotencyKey ?? null,
            reused_existing_task: Boolean(created.reusedExistingTask),
          },
        };
      },
    ),
  );

  server.registerTool(
    'vacps.tasks.get',
    {
      description: 'Get task status plus optional command list and output previews.',
      inputSchema: {
        task_id: z.string().uuid(),
        include_commands: z.boolean().default(false),
        include_output_preview: z.boolean().default(true),
        preview_max_bytes: z.number().int().min(0).max(65_536).default(8192),
      },
      outputSchema: okEnvelope.extend({
        task: z.unknown(),
        result: z.unknown().optional(),
        output: z.unknown().optional(),
        commands: z.unknown().optional(),
      }).shape,
    },
    wrap(
      (value) => {
        const task = value.task as { id?: string; status?: string };
        return `Task ${task.id ?? '?'} is ${task.status ?? 'unknown'}`;
      },
      async ({ task_id, include_commands, include_output_preview, preview_max_bytes }) => {
        const detail = await tasks.detail(String(task_id), {
          includeCommands: Boolean(include_commands),
          includeOutputPreview: include_output_preview !== false,
          previewMaxBytes: typeof preview_max_bytes === 'number' ? preview_max_bytes : 8192,
        });
        return {
          task: snakeTask(detail.task),
          ...(detail.result !== undefined ? { result: detail.result } : {}),
          ...(detail.output !== undefined ? { output: detail.output } : {}),
          ...(detail.commands !== undefined ? { commands: detail.commands } : {}),
        };
      },
    ),
  );

  server.registerTool(
    'vacps.tasks.output.read',
    {
      description: 'Read a task stdout/stderr stream by absolute byte offset.',
      inputSchema: {
        task_id: z.string().uuid(),
        stream: z.enum(['stdout', 'stderr']).default('stdout'),
        offset: z.number().int().min(0).default(0),
        max_bytes: z.number().int().min(1).max(1_048_576).default(65_536),
      },
      outputSchema: okEnvelope.extend({
        task_id: z.string(),
        stream: z.string(),
        offset: z.number(),
        next_offset: z.number(),
        eof: z.boolean(),
        data: z.string(),
      }).shape,
    },
    wrap(
      (value) =>
        `Read ${String(value.stream)} for ${String(value.task_id)} (${String(value.data).length} bytes)`,
      async ({ task_id, stream, offset, max_bytes }) => {
        const payload = (await tasks.readOutput(String(task_id), {
          stream: (stream as 'stdout' | 'stderr') || 'stdout',
          offset: typeof offset === 'number' ? offset : 0,
          maxBytes: typeof max_bytes === 'number' ? max_bytes : 65_536,
        })) as Record<string, unknown>;
        return payload;
      },
    ),
  );

  server.registerTool(
    'vacps.tasks.list',
    {
      description: 'List recent task summaries from the control plane index.',
      inputSchema: { limit: z.number().int().min(1).max(200).default(50) },
      outputSchema: okEnvelope.extend({ tasks: z.array(z.unknown()) }).shape,
    },
    wrap(
      (value) => `Tasks: ${(value.tasks as unknown[]).length}`,
      async ({ limit }) => ({
        tasks: (await tasks.list(typeof limit === 'number' ? limit : 50)).map(snakeTask),
      }),
    ),
  );

  server.registerTool(
    'vacps.tasks.cancel',
    {
      description: 'Cancel a queued or running task.',
      inputSchema: { task_id: z.string().uuid() },
      outputSchema: okEnvelope.extend({ task: z.unknown() }).shape,
    },
    wrap(
      (value) => {
        const task = value.task as { id?: string; status?: string };
        return `Cancelled ${task.id ?? '?'} (${task.status ?? 'cancelled'})`;
      },
      async ({ task_id }) => {
        const result = (await tasks.cancel(String(task_id))) as { task: unknown };
        return { task: snakeTask(result.task as never) };
      },
    ),
  );

  server.registerTool(
    'vacps.tasks.retry',
    {
      description: 'Retry a task (creates a new task when possible).',
      inputSchema: { task_id: z.string().uuid() },
      outputSchema: okEnvelope.extend({ task: z.unknown() }).shape,
    },
    wrap(
      (value) => {
        const task = value.task as { id?: string; status?: string };
        return `Retry ${task.id ?? '?'} (${task.status ?? 'queued'})`;
      },
      async ({ task_id }) => {
        const result = (await tasks.retry(String(task_id))) as { task: unknown };
        return { task: snakeTask(result.task as never) };
      },
    ),
  );

  // ── Layer B: schedules ─────────────────────────────────────────────
  server.registerTool(
    'vacps.schedules.create',
    {
      description: 'Create a cron schedule on a backend.',
      inputSchema: scheduleCreateInputSchema,
      outputSchema: okEnvelope.extend({ schedule: z.unknown() }).shape,
    },
    wrap(
      (value) => `Schedule ${(value.schedule as { id?: string }).id ?? '?'} created`,
      async (raw) => ({
        schedule: snakeSchedule(await schedules.create(parseScheduleCreate(raw))),
      }),
    ),
  );
  server.registerTool(
    'vacps.schedules.get',
    {
      description: 'Get a schedule definition.',
      inputSchema: { schedule_id: z.string().uuid() },
      outputSchema: okEnvelope.extend({ schedule: z.unknown() }).shape,
    },
    wrap(
      (value) => `Schedule ${(value.schedule as { id?: string }).id ?? '?'}`,
      async ({ schedule_id }) => ({
        schedule: snakeSchedule(await schedules.get(String(schedule_id))),
      }),
    ),
  );
  server.registerTool(
    'vacps.schedules.list',
    {
      description: 'List cron schedules.',
      outputSchema: okEnvelope.extend({ schedules: z.array(z.unknown()) }).shape,
    },
    wrap(
      (value) => `Schedules: ${(value.schedules as unknown[]).length}`,
      async () => ({ schedules: (await schedules.list()).map(snakeSchedule) }),
    ),
  );
  server.registerTool(
    'vacps.schedules.update',
    {
      description: 'Update a cron schedule.',
      inputSchema: { schedule_id: z.string().uuid(), ...scheduleCreateInputSchema },
      outputSchema: okEnvelope.extend({ schedule: z.unknown() }).shape,
    },
    wrap(
      (value) => `Schedule ${(value.schedule as { id?: string }).id ?? '?'} updated`,
      async ({ schedule_id, ...raw }) => ({
        schedule: snakeSchedule(
          await schedules.update(String(schedule_id), parseScheduleCreate(raw)),
        ),
      }),
    ),
  );
  server.registerTool(
    'vacps.schedules.delete',
    {
      description: 'Delete a cron schedule.',
      inputSchema: { schedule_id: z.string().uuid() },
      outputSchema: okEnvelope.extend({ deleted: z.boolean(), schedule_id: z.string() }).shape,
    },
    wrap(
      (value) => `Deleted schedule ${String(value.schedule_id)}`,
      async ({ schedule_id }) => {
        await schedules.delete(String(schedule_id));
        return { deleted: true, schedule_id: String(schedule_id) };
      },
    ),
  );
  server.registerTool(
    'vacps.schedules.run_now',
    {
      description: 'Immediately queue a schedule once.',
      inputSchema: { schedule_id: z.string().uuid() },
      outputSchema: okEnvelope.extend({ task: z.unknown() }).shape,
    },
    wrap(
      (value) => `Queued ${(value.task as { id?: string }).id ?? 'task'} from schedule`,
      async ({ schedule_id }) => {
        const task = await schedules.runNow(String(schedule_id));
        return { task: snakeTask(task as Parameters<typeof snakeTask>[0]) };
      },
    ),
  );

  // ── Layer A: minimal ───────────────────────────────────────────────
  server.registerTool(
    'vacps.read',
    {
      description: 'Read a file on a backend by absolute path (line offset/limit).',
      inputSchema: {
        backend_id: z.string(),
        file_path: z.string(),
        offset: z.number().int().min(1).default(1),
        limit: z.number().int().min(1).max(5000).default(2000),
      },
      outputSchema: okEnvelope.extend({
        file_path: z.string(),
        content: z.string(),
        truncated: z.boolean(),
      }).shape,
    },
    wrap(
      (value) => `Read ${String(value.file_path)}`,
      async ({ backend_id, file_path, offset, limit }) => {
        const backend = await backends.get(String(backend_id));
        if (!backend.enabled)
          throw new AppError('backend_disabled', `Backend '${backend.id}' is disabled.`, 409);
        return (await client.readFile(backend, {
          filePath: String(file_path),
          offset: typeof offset === 'number' ? offset : 1,
          limit: typeof limit === 'number' ? limit : 2000,
        })) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.bash',
    {
      description: 'Run a shell command on a backend (foreground; output previews capped).',
      inputSchema: {
        backend_id: z.string(),
        command: z.string(),
        description: z.string().optional(),
        timeout_ms: z.number().int().min(1).max(600_000).default(120_000),
        cwd: z.string().optional(),
      },
      outputSchema: okEnvelope.extend({
        status: z.string(),
        exit_code: z.number().nullable(),
        stdout_preview: z.string(),
        stderr_preview: z.string(),
      }).shape,
    },
    wrap(
      (value) => `bash ${String(value.status)} exit=${String(value.exit_code)}`,
      async ({ backend_id, command, description, timeout_ms, cwd }) => {
        const backend = await backends.get(String(backend_id));
        if (!backend.enabled)
          throw new AppError('backend_disabled', `Backend '${backend.id}' is disabled.`, 409);
        // Process non-zero exits are not protocol errors.
        return (await client.bash(backend, {
          command: String(command),
          timeoutMs: typeof timeout_ms === 'number' ? timeout_ms : 120_000,
          ...(typeof cwd === 'string' ? { cwd } : {}),
          ...(typeof description === 'string' ? { description } : {}),
        })) as Record<string, unknown>;
      },
    ),
  );

  return server;
}

// ── MCP snake_case input schemas ─────────────────────────────────────

const shellInputSchema = z.object({
  mode: z.enum(['exec', 'script']),
  program: z.string().optional(),
  arguments: z.array(z.string()).optional(),
  interpreter: z.string().optional(),
  interpreter_arguments: z.array(z.string()).optional(),
  content: z.string().optional(),
});

const agentInputSchema = z.object({
  prompt: z.string(),
  profile: z.enum(['restricted', 'diagnostic', 'standard', 'privileged']).optional(),
  max_steps: z.number().int().optional(),
  permissions: z
    .object({
      shell: z.boolean().optional(),
      network: z.boolean().optional(),
      file_write: z.boolean().optional(),
    })
    .optional(),
});

const taskCreateInputSchema = {
  backend_id: z.string(),
  type: z.enum(['shell', 'agent']),
  name: z.string().optional(),
  working_directory: z.string().optional(),
  timeout_seconds: z.number().int().min(1).max(86_400),
  profile: z.string().optional(),
  idempotency_key: z.string().optional(),
  labels: z.record(z.string(), z.string()).optional(),
  environment: z.record(z.string(), z.string()).optional(),
  shell: shellInputSchema.optional(),
  agent: agentInputSchema.optional(),
  output: z
    .object({
      capture_stdout: z.boolean().optional(),
      capture_stderr: z.boolean().optional(),
      preview_max_bytes: z.number().int().optional(),
      retention_seconds: z.number().int().optional(),
      hard_max_bytes: z.number().int().optional(),
    })
    .optional(),
};

const scheduleCreateInputSchema = {
  backend_id: z.string(),
  name: z.string(),
  cron: z.string(),
  timezone: z.string().default('UTC'),
  enabled: z.boolean().default(true),
  task_template: z.object(taskCreateInputSchema),
};

function parseTaskCreateInput(raw: Record<string, unknown>): CreateTaskInput {
  const base = {
    backendId: String(raw.backend_id),
    type: raw.type,
    ...(typeof raw.name === 'string' ? { name: raw.name } : {}),
    cwd: typeof raw.working_directory === 'string' ? raw.working_directory : '/tmp',
    timeoutSeconds: Number(raw.timeout_seconds),
    ...(typeof raw.profile === 'string' ? { profile: raw.profile } : {}),
    ...(typeof raw.idempotency_key === 'string' ? { idempotencyKey: raw.idempotency_key } : {}),
    ...(raw.labels && typeof raw.labels === 'object'
      ? { labels: raw.labels as Record<string, string> }
      : {}),
    ...(raw.environment && typeof raw.environment === 'object'
      ? { environment: raw.environment as Record<string, string> }
      : {}),
    ...(raw.output && typeof raw.output === 'object'
      ? {
          output: mapOutputOptions(raw.output as Record<string, unknown>),
        }
      : {}),
  };

  if (raw.type === 'shell') {
    const shell = raw.shell as Record<string, unknown> | undefined;
    if (!shell || typeof shell !== 'object')
      throw new AppError('validation_error', 'shell is required for type=shell.', 400);
    if (shell.mode === 'exec') {
      return createTaskSchema.parse({
        ...base,
        type: 'shell',
        shell: {
          mode: 'exec',
          program: String(shell.program ?? ''),
          arguments: Array.isArray(shell.arguments) ? shell.arguments.map(String) : [],
        },
      });
    }
    return createTaskSchema.parse({
      ...base,
      type: 'shell',
      shell: {
        mode: 'script',
        interpreter: String(shell.interpreter ?? '/bin/bash'),
        interpreterArguments: Array.isArray(shell.interpreter_arguments)
          ? shell.interpreter_arguments.map(String)
          : ['-c'],
        content: String(shell.content ?? ''),
      },
    });
  }

  if (raw.type === 'agent') {
    const agent = raw.agent as Record<string, unknown> | undefined;
    if (!agent || typeof agent !== 'object')
      throw new AppError('validation_error', 'agent is required for type=agent.', 400);
    const permissions = (agent.permissions ?? {}) as Record<string, unknown>;
    return createTaskSchema.parse({
      ...base,
      type: 'agent',
      agent: {
        prompt: String(agent.prompt ?? ''),
        ...(typeof agent.profile === 'string' ? { profile: agent.profile } : {}),
        ...(typeof agent.max_steps === 'number' ? { maxSteps: agent.max_steps } : {}),
        permissions: {
          shell: Boolean(permissions.shell),
          network: Boolean(permissions.network),
          fileWrite: Boolean(permissions.file_write),
        },
      },
    });
  }

  throw new AppError('validation_error', 'type must be shell or agent.', 400);
}

function mapOutputOptions(output: Record<string, unknown>) {
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

function parseScheduleCreate(raw: Record<string, unknown>): CreateScheduleInput {
  const templateRaw = (raw.task_template ?? {}) as Record<string, unknown>;
  return createScheduleSchema.parse({
    backendId: String(raw.backend_id),
    name: String(raw.name),
    cron: String(raw.cron),
    timezone: typeof raw.timezone === 'string' ? raw.timezone : 'UTC',
    enabled: typeof raw.enabled === 'boolean' ? raw.enabled : true,
    taskTemplate: parseTaskCreateInput(templateRaw),
  });
}

function snakeTask(task: {
  id: string;
  backendId: string;
  type: string;
  source?: string | undefined;
  profile?: string | undefined;
  summary?: string | undefined;
  status: string;
  scheduleId?: string | undefined;
  idempotencyKey?: string | undefined;
  retryOfTaskId?: string | undefined;
  createdAt: string;
  updatedAt: string;
  finishedAt?: string | undefined;
  reusedExistingTask?: boolean | undefined;
}) {
  return {
    id: task.id,
    backend_id: task.backendId,
    type: task.type,
    ...(task.source ? { source: task.source } : {}),
    ...(task.profile ? { profile: task.profile } : {}),
    ...(task.summary ? { summary: task.summary } : {}),
    status: task.status,
    ...(task.scheduleId ? { schedule_id: task.scheduleId } : {}),
    ...(task.idempotencyKey ? { idempotency_key: task.idempotencyKey } : {}),
    ...(task.retryOfTaskId ? { retry_of_task_id: task.retryOfTaskId } : {}),
    created_at: task.createdAt,
    updated_at: task.updatedAt,
    ...(task.finishedAt ? { finished_at: task.finishedAt } : {}),
    cancellable: !['succeeded', 'failed', 'cancelled', 'timed_out', 'dispatch_failed'].includes(
      task.status,
    ),
  };
}

function snakeSchedule(schedule: {
  id: string;
  backendId: string;
  name: string;
  cron: string;
  timezone: string;
  enabled: boolean;
  taskTemplate: unknown;
  lastRunAt?: string | undefined;
  nextRunAt?: string | undefined;
  createdAt: string;
  updatedAt: string;
}) {
  return {
    id: schedule.id,
    backend_id: schedule.backendId,
    name: schedule.name,
    cron: schedule.cron,
    timezone: schedule.timezone,
    enabled: schedule.enabled,
    task_template: schedule.taskTemplate,
    ...(schedule.lastRunAt ? { last_run_at: schedule.lastRunAt } : {}),
    ...(schedule.nextRunAt ? { next_run_at: schedule.nextRunAt } : {}),
    created_at: schedule.createdAt,
    updated_at: schedule.updatedAt,
  };
}

function snakeBackends(
  list: Array<{
    id: string;
    name: string;
    baseUrl: string;
    tags: string[];
    enabled: boolean;
    createdAt: string;
    updatedAt: string;
    lastStatus?: unknown;
    lastCheckedAt?: string;
  }>,
) {
  return list.map((backend) => ({
    id: backend.id,
    name: backend.name,
    base_url: backend.baseUrl,
    tags: backend.tags,
    enabled: backend.enabled,
    created_at: backend.createdAt,
    updated_at: backend.updatedAt,
    ...(backend.lastStatus ? { last_status: backend.lastStatus } : {}),
    ...(backend.lastCheckedAt ? { last_checked_at: backend.lastCheckedAt } : {}),
  }));
}

function snakeStatus(status: unknown) {
  return status;
}

function categoryFor(code: string): string {
  if (code.includes('validation') || code === 'invalid_request') return 'validation';
  if (code.includes('not_found')) return 'not_found';
  if (code.includes('disabled') || code.includes('conflict') || code.includes('mismatch'))
    return 'conflict';
  if (code.includes('unauthorized') || code.includes('forbidden')) return 'permission';
  if (code.includes('unreachable') || code.includes('failed') || code.includes('unavailable'))
    return 'runtime';
  return 'internal';
}

function isRetryable(code: string): boolean {
  return ['backend_unavailable', 'backend_unreachable', 'backend_request_failed'].includes(code);
}

// Silence unused import warning if taskStatuses only used historically
void taskStatuses;
