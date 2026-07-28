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
import { annotationsFor } from './schema/annotations.js';
import {
  categoryFor,
  isRetryable,
  SCHEMA_VERSION,
  successMeta,
  toolResultContent,
} from './schema/envelope.js';
import {
  backendsGetStatusInputSchema,
  backendsListInputSchema,
  capabilitiesGetInputSchema,
  commandExecInputSchema,
  filesApplyPatchInputSchema,
  filesDeleteInputSchema,
  filesEditInputSchema,
  filesGlobInputSchema,
  filesGrepInputSchema,
  filesListInputSchema,
  filesMkdirInputSchema,
  filesMoveInputSchema,
  filesReadInputSchema,
  filesStatInputSchema,
  filesWriteInputSchema,
  gitApplyInputSchema,
  gitDiffInputSchema,
  gitStatusInputSchema,
  MCP_PROTOCOL_VERSION,
  processReadInputSchema,
  processStartInputSchema,
  processStartListJsonSchema,
  processTerminateInputSchema,
  processWriteInputSchema,
  shellExecInputSchema,
  TOOL_SCHEMA_REVISION,
} from './tool-schemas.js';

const okEnvelope = z.looseObject({
  ok: z.literal(true),
  schema_version: z.string(),
  request_id: z.string(),
  trace_id: z.string(),
  generated_at: z.string(),
  warnings: z.array(z.string()),
});

function toolConfig(
  name: string,
  config: {
    description: string;
    inputSchema?: unknown;
    outputSchema?: unknown;
    _meta?: Record<string, unknown>;
  },
) {
  return {
    ...config,
    annotations: annotationsFor(name),
    _meta: { tool_schema_revision: TOOL_SCHEMA_REVISION, ...(config._meta ?? {}) },
  } as never;
}

export function createMcpServer(env: Env): McpServer {
  const backends = new BackendRepository(env.DB);
  const client = new BackendClient(env.CONTROL_PLANE_SIGNING_PRIVATE_KEY);
  const tasks = new TaskService(env.DB, backends, client);
  const schedules = new ScheduleService(env.DB, backends, client, tasks);
  // Bump version when tools/list contracts change so MCP clients refresh caches.
  const server = new McpServer({
    name: 'vacps',
    version: MCP_PROTOCOL_VERSION,
  });

  const ok = (value: Record<string, unknown>) => {
    const backendId =
      typeof value.backend_id === 'string'
        ? value.backend_id
        : typeof (value.backend as { id?: string } | undefined)?.id === 'string'
          ? (value.backend as { id: string }).id
          : undefined;
    const structured = {
      ...successMeta(backendId ? { backend_id: backendId } : undefined),
      ...value,
      ok: true as const,
      schema_version: SCHEMA_VERSION,
    };
    return {
      structuredContent: structured,
      content: toolResultContent(structured),
      isError: false as const,
    };
  };

  const fail = (error: unknown, backendId?: string) => {
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
    const meta = successMeta(backendId ? { backend_id: backendId } : undefined);
    const payload = {
      ok: false as const,
      schema_version: SCHEMA_VERSION,
      request_id: meta.request_id,
      trace_id: meta.trace_id,
      generated_at: meta.generated_at,
      ...(backendId ? { backend_id: backendId } : {}),
      error: {
        code: app.code,
        message: app.message,
        category: categoryFor(app.code),
        retryable: isRetryable(app.code),
        retry_after_ms: null as number | null,
        details: {} as Record<string, unknown>,
      },
    };
    return {
      structuredContent: payload,
      content: toolResultContent(payload),
      isError: true as const,
    };
  };

  /** Supports wrap(run) or legacy wrap(summary, run). Summary is ignored (content is JSON). */
  function wrap<T extends Record<string, unknown>>(
    run: (args: Record<string, unknown>) => Promise<T>,
  ): (args: Record<string, unknown>) => Promise<ReturnType<typeof ok> | ReturnType<typeof fail>>;
  function wrap<T extends Record<string, unknown>>(
    summary: (value: T) => string,
    run: (args: Record<string, unknown>) => Promise<T>,
  ): (args: Record<string, unknown>) => Promise<ReturnType<typeof ok> | ReturnType<typeof fail>>;
  function wrap<T extends Record<string, unknown>>(
    summaryOrRun:
      | ((value: T) => string)
      | ((args: Record<string, unknown>) => Promise<T>),
    maybeRun?: (args: Record<string, unknown>) => Promise<T>,
  ) {
    const run = (maybeRun ?? summaryOrRun) as (args: Record<string, unknown>) => Promise<T>;
    return async (args: Record<string, unknown>) => {
      try {
        const value = await run(args);
        return ok(value);
      } catch (error) {
        const backendId = typeof args.backend_id === 'string' ? args.backend_id : undefined;
        return fail(error, backendId);
      }
    };
  }

  // ── Layer B: backends ──────────────────────────────────────────────
  server.registerTool(
    'vacps.backends.list',
    toolConfig('vacps.backends.list', {
      description:
        'List Vacps backends (nodes) as a slim summary. Use backends.get_status for health/metrics detail.',
      inputSchema: backendsListInputSchema,
      outputSchema: okEnvelope.extend({
        backends: z.array(z.unknown()),
        returned_count: z.number(),
        next_cursor: z.string().nullable(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = backendsListInputSchema.parse(args);
      return pageBackends(await backends.list(), parsed);
    }),
  );

  server.registerTool(
    'vacps.backends.get_status',
    toolConfig('vacps.backends.get_status', {
      description: 'Get live health and metrics for one backend.',
      inputSchema: backendsGetStatusInputSchema,
      outputSchema: okEnvelope.extend({
        backend: z.unknown(),
        health: z.unknown().optional(),
        metrics: z.unknown().optional(),
        status: z.unknown().optional(),
      }).shape,
    }),
    wrap(async (args) => {
      const { backend_id } = backendsGetStatusInputSchema.parse(args);
      const backend = await backends.get(backend_id);
      const status = await client.status(backend);
      await backends.recordStatus(backend.id, status, { preserveSystem: true });
      return {
        backend_id: backend.id,
        backend: {
          id: backend.id,
          name: backend.name,
          enabled: backend.enabled,
        },
        status: snakeStatus(status),
      };
    }),
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

  const requireBackend = async (backendId: unknown) => {
    const backend = await backends.get(String(backendId));
    if (!backend.enabled)
      throw new AppError('backend_disabled', `Backend '${backend.id}' is disabled.`, 409);
    return backend;
  };

  // ── Command / shell / process (Schema v2) ──────────────────────────
  server.registerTool(
    'vacps.command.exec',
    toolConfig('vacps.command.exec', {
      description:
        'Run a non-interactive program on a backend (argv form, no shell). Uses yield_time_ms for sync wait.',
      inputSchema: commandExecInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = commandExecInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.execCommand(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.shell.exec',
    toolConfig('vacps.shell.exec', {
      description:
        'Run a shell command as the agent user with full login environment (bash -lc, sources ~/.bashrc). Prefer vacps.command.exec for non-shell work. Set load_user_environment=false only for a clean --noprofile --norc shell.',
      inputSchema: shellExecInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = shellExecInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.execShell(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.process.start',
    toolConfig('vacps.process.start', {
      description:
        'Start a long-running or interactive process on a backend. Provide exactly one of program or command (not both, not neither). stdout/stderr_hard_max_bytes are 0..1073741824.',
      inputSchema: processStartInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = processStartInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.processStart(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.process.read',
    toolConfig('vacps.process.read', {
      description: 'Read stdout/stderr chunks from a process started on a backend.',
      inputSchema: processReadInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = processReadInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.processRead(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.process.write',
    toolConfig('vacps.process.write', {
      description: 'Write to process stdin on a backend.',
      inputSchema: processWriteInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = processWriteInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.processWrite(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.process.terminate',
    toolConfig('vacps.process.terminate', {
      description: 'Terminate a process on a backend.',
      inputSchema: processTerminateInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = processTerminateInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.processTerminate(backend, parsed)) as Record<string, unknown>;
    }),
  );

  // ── Files (Schema v2) ──────────────────────────────────────────────
  server.registerTool(
    'vacps.files.read',
    toolConfig('vacps.files.read', {
      description:
        'Read a file on a backend by absolute path with optional line range and byte cap.',
      inputSchema: filesReadInputSchema,
      outputSchema: okEnvelope.extend({ path: z.string(), content: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesReadInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.readFile(backend, {
        path: parsed.path,
        startLine: parsed.start_line,
        endLine: parsed.end_line,
        maxBytes: parsed.max_bytes ?? 32_768,
        encoding: parsed.encoding === 'base64' ? 'base64' : 'utf-8',
      })) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.stat',
    toolConfig('vacps.files.stat', {
      description: 'Stat a file or directory on a backend.',
      inputSchema: filesStatInputSchema,
      outputSchema: okEnvelope.extend({ path: z.string(), type: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesStatInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.statFile(backend, parsed.path)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.list',
    toolConfig('vacps.files.list', {
      description: 'List directory entries on a backend. Supports limit + opaque cursor.',
      inputSchema: filesListInputSchema,
      outputSchema: okEnvelope.extend({
        matches: z.array(z.unknown()),
        next_cursor: z.string().nullable().optional(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesListInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.listDir(backend, {
        path: parsed.path,
        limit: parsed.limit ?? 200,
        includeHidden: parsed.include_hidden === true,
        ...(parsed.cursor ? { cursor: parsed.cursor } : {}),
      })) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.glob',
    toolConfig('vacps.files.glob', {
      description:
        'Glob files on a backend. Dialect is bash-like globstar: * is one segment, ** matches zero or more segments (so **/*.txt matches example.txt at the root). Uses rg when available.',
      inputSchema: filesGlobInputSchema,
      outputSchema: okEnvelope.extend({
        matches: z.array(z.unknown()),
        next_cursor: z.string().nullable().optional(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesGlobInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.glob(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.grep',
    toolConfig('vacps.files.grep', {
      description:
        'Search file contents on a backend (ripgrep when available). path may be a file or a directory; when a file is given, only that file is searched.',
      inputSchema: filesGrepInputSchema,
      outputSchema: okEnvelope.extend({
        matches: z.array(z.unknown()),
        next_cursor: z.string().nullable().optional(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesGrepInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.grep(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.edit',
    toolConfig('vacps.files.edit', {
      description:
        'Exact string replacement in a file on a backend (old_text must be unique unless replace_all).',
      inputSchema: filesEditInputSchema,
      outputSchema: okEnvelope.extend({ path: z.string(), replacement_count: z.number() }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesEditInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.editFile(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.write',
    toolConfig('vacps.files.write', {
      description:
        'Write a file on a backend (atomic temp+rename). mode is required: create (fail if exists), overwrite (fail if missing), or create_or_overwrite. Supports idempotency_key + expected_sha256.',
      inputSchema: filesWriteInputSchema,
      outputSchema: okEnvelope.extend({ path: z.string(), operation: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesWriteInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.writeFile(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.apply_patch',
    toolConfig('vacps.files.apply_patch', {
      description: 'Apply a Codex-style multi-file patch on a backend.',
      inputSchema: filesApplyPatchInputSchema,
      outputSchema: okEnvelope.extend({ files: z.array(z.unknown()) }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesApplyPatchInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.applyPatch(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.move',
    toolConfig('vacps.files.move', {
      description:
        'Move/rename a file on a backend. Optional expected_sha256 for optimistic concurrency.',
      inputSchema: filesMoveInputSchema,
      outputSchema: okEnvelope.extend({ from: z.string(), to: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesMoveInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.moveFile(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.files.delete',
    toolConfig('vacps.files.delete', {
      description:
        'Delete a file or directory on a backend. Use dry_run to preview size/count; expected_sha256 / expected_type for safety.',
      inputSchema: filesDeleteInputSchema,
      outputSchema: okEnvelope.extend({ path: z.string(), operation: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesDeleteInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.deleteFile(backend, parsed)) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.capabilities.get',
    toolConfig('vacps.capabilities.get', {
      description:
        'Report backend tool capabilities (nested features, engines, limits; no dotted feature keys).',
      inputSchema: capabilitiesGetInputSchema,
      outputSchema: okEnvelope.extend({
        features: z.unknown(),
        engines: z.unknown(),
        limits: z.unknown(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = capabilitiesGetInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      const raw = (await client.getCapabilities(backend)) as Record<string, unknown>;
      return normalizeCapabilities(raw);
    }),
  );

  server.registerTool(
    'vacps.files.mkdir',
    toolConfig('vacps.files.mkdir', {
      description: 'Create a directory on a backend.',
      inputSchema: filesMkdirInputSchema,
      outputSchema: okEnvelope.extend({ path: z.string(), operation: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesMkdirInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.mkdir(backend, parsed)) as Record<string, unknown>;
    }),
  );

  // ── Git helpers ────────────────────────────────────────────────────
  server.registerTool(
    'vacps.git.status',
    toolConfig('vacps.git.status', {
      description: 'git status --short on a backend working tree.',
      inputSchema: gitStatusInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = gitStatusInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.execCommand(backend, {
        program: 'git',
        arguments: ['status', '--short'],
        working_directory: parsed.working_directory,
        timeout_ms: 60_000,
        yield_time_ms: 30_000,
      })) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.git.diff',
    toolConfig('vacps.git.diff', {
      description: 'git diff on a backend working tree.',
      inputSchema: gitDiffInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = gitDiffInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      const arguments_ = parsed.staged === true ? ['diff', '--cached'] : ['diff'];
      return (await client.execCommand(backend, {
        program: 'git',
        arguments: arguments_,
        working_directory: parsed.working_directory,
        timeout_ms: 60_000,
        yield_time_ms: 30_000,
        stdout_max_bytes: 65_536,
      })) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.git.apply',
    toolConfig('vacps.git.apply', {
      description: 'Apply a unified diff with git apply on a backend.',
      inputSchema: gitApplyInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = gitApplyInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      const path = `/tmp/vacps-git-apply-${crypto.randomUUID()}.patch`;
      await client.writeFile(backend, {
        path,
        content: parsed.patch,
        mode: 'create_or_overwrite',
        create_parent_directories: true,
      });
      try {
        return (await client.execCommand(backend, {
          program: 'git',
          arguments: parsed.check === true ? ['apply', '--check', path] : ['apply', path],
          working_directory: parsed.working_directory,
          timeout_ms: 60_000,
          yield_time_ms: 30_000,
        })) as Record<string, unknown>;
      } finally {
        await client.deleteFile(backend, { path }).catch(() => undefined);
      }
    }),
  );

  // Schema v2 list patches: process.start oneOf + fill annotations for legacy registrations.
  applySchemaV2ListPatches(server);

  return server;
}

/**
 * Schema v2 tools/list patches:
 * - inject process.start oneOf (SDK cannot emit superRefine XOR)
 * - ensure annotations present on every tool
 */
function applySchemaV2ListPatches(server: McpServer): void {
  const host = server as unknown as {
    _registeredTools?: Record<
      string,
      { annotations?: unknown; _meta?: Record<string, unknown>; enabled?: boolean }
    >;
    server: {
      _requestHandlers: Map<
        string,
        (request: unknown, extra: unknown) => unknown | Promise<unknown>
      >;
    };
  };

  for (const [name, tool] of Object.entries(host._registeredTools ?? {})) {
    tool.annotations = annotationsFor(name);
    tool._meta = { tool_schema_revision: TOOL_SCHEMA_REVISION, ...(tool._meta ?? {}) };
  }

  const previous = host.server._requestHandlers.get('tools/list');
  if (!previous) return;

  host.server._requestHandlers.set('tools/list', async (request, extra) => {
    const result = (await previous(request, extra)) as {
      tools?: Array<{
        name?: string;
        inputSchema?: Record<string, unknown>;
        annotations?: unknown;
      }>;
    };
    for (const tool of result.tools ?? []) {
      if (tool.name === 'vacps.process.start') {
        tool.inputSchema = { ...processStartListJsonSchema };
      }
      if (tool.name) {
        tool.annotations = annotationsFor(tool.name);
      }
    }
    return result;
  });
}

function pageBackends(
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
  query: {
    limit?: number | undefined;
    cursor?: string | undefined;
    enabled?: boolean | undefined;
    status?: 'healthy' | 'unhealthy' | 'unknown' | undefined;
    tags?: string[] | undefined;
  },
) {
  let filtered = list.map((backend) => slimBackend(backend));
  if (typeof query.enabled === 'boolean') {
    filtered = filtered.filter((item) => item.enabled === query.enabled);
  }
  if (query.status) {
    filtered = filtered.filter((item) => item.status === query.status);
  }
  if (query.tags && query.tags.length > 0) {
    filtered = filtered.filter((item) => query.tags!.every((tag) => item.tags.includes(tag)));
  }

  const limit = query.limit ?? 50;
  let offset = 0;
  if (query.cursor) {
    try {
      offset = decodeOffsetCursor(query.cursor);
    } catch {
      throw new AppError('validation_error', 'Invalid cursor.', 400);
    }
  }

  const page = filtered.slice(offset, offset + limit);
  const nextOffset = offset + page.length;
  const next_cursor =
    nextOffset < filtered.length ? encodeOffsetCursor(nextOffset) : null;

  return {
    backends: page,
    returned_count: page.length,
    next_cursor,
  };
}

function slimBackend(backend: {
  id: string;
  name: string;
  tags: string[];
  enabled: boolean;
  lastStatus?: unknown;
  lastCheckedAt?: string;
}) {
  return {
    id: backend.id,
    name: backend.name,
    enabled: backend.enabled,
    status: deriveBackendStatus(backend.lastStatus),
    tags: backend.tags,
    ...(backend.lastCheckedAt ? { last_checked_at: backend.lastCheckedAt } : {}),
  };
}

function deriveBackendStatus(lastStatus: unknown): 'healthy' | 'unhealthy' | 'unknown' {
  if (!lastStatus || typeof lastStatus !== 'object') return 'unknown';
  const health = (lastStatus as { health?: { ok?: boolean } }).health;
  if (health && typeof health.ok === 'boolean') return health.ok ? 'healthy' : 'unhealthy';
  const ok = (lastStatus as { ok?: boolean }).ok;
  if (typeof ok === 'boolean') return ok ? 'healthy' : 'unhealthy';
  return 'unknown';
}

/** Normalize agent capabilities into Schema v2 nested shape (no dotted feature keys). */
function normalizeCapabilities(raw: Record<string, unknown>): Record<string, unknown> {
  const featuresRaw = (raw.features ?? {}) as Record<string, unknown>;
  const enginesRaw = (raw.engines ?? {}) as Record<string, unknown>;
  const grepEngine = (enginesRaw.grep ?? {}) as Record<string, unknown>;
  const globEngine = (enginesRaw.glob ?? {}) as Record<string, unknown>;
  const executables = (raw.executables ?? {}) as Record<string, { available?: boolean; version?: string }>;
  const rg = executables.rg;

  const active =
    featuresRaw['files.grep.engine'] === 'rg' || grepEngine.available === true
      ? 'ripgrep'
      : 'node';

  return {
    backend_id: raw.backend_id,
    features: {
      command_exec: featuresRaw.command_exec !== false,
      shell_exec: featuresRaw.shell_exec !== false,
      interactive_process: featuresRaw.interactive_process !== false,
      file_patch: featuresRaw.file_patch !== false,
      git_tools: featuresRaw.git_tools !== false,
    },
    engines: {
      grep: {
        active,
        available: Boolean(rg?.available ?? grepEngine.available),
        version: rg?.version ?? null,
        fallback: grepEngine.fallback ?? 'node',
        regex_flavor:
          (grepEngine.engine_features as { regex_flavor?: string } | undefined)?.regex_flavor ??
          (active === 'ripgrep' ? 'rust' : 'javascript'),
        respects_gitignore:
          (grepEngine.engine_features as { respects_gitignore?: boolean } | undefined)
            ?.respects_gitignore ?? active === 'ripgrep',
      },
      glob: {
        dialect: featuresRaw['files.glob.dialect'] ?? 'globstar',
        respects_gitignore: globEngine.respect_gitignore !== false,
      },
    },
    limits: raw.limits ?? {
      command_timeout_max_ms: 3_600_000,
      process_read_max_bytes: 1_048_576,
      file_read_max_bytes: 262_144,
    },
    agent_environment: raw.agent_environment,
  };
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

function snakeStatus(status: unknown) {
  return status;
}

function encodeOffsetCursor(offset: number): string {
  const json = JSON.stringify({ o: offset });
  const bytes = new TextEncoder().encode(json);
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/g, '');
}

function decodeOffsetCursor(cursor: string): number {
  const padded = cursor.replace(/-/g, '+').replace(/_/g, '/');
  const pad = padded.length % 4 === 0 ? '' : '='.repeat(4 - (padded.length % 4));
  const binary = atob(padded + pad);
  const bytes = Uint8Array.from(binary, (char) => char.charCodeAt(0));
  const decoded = JSON.parse(new TextDecoder().decode(bytes)) as { o?: number };
  if (typeof decoded.o !== 'number' || decoded.o < 0) {
    throw new Error('invalid cursor');
  }
  return Math.trunc(decoded.o);
}

// Silence unused import warning if taskStatuses only used historically
void taskStatuses;
