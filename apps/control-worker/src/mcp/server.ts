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
import {
  capabilitiesGetInputSchema,
  commandExecInputSchema,
  filesWriteInputSchema,
  MCP_PROTOCOL_VERSION,
  processStartInputSchema,
  processStartListJsonSchema,
  shellExecInputSchema,
  TOOL_SCHEMA_REVISION,
} from './tool-schemas.js';

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
  // Bump version when tools/list contracts change so MCP clients refresh caches.
  const server = new McpServer({
    name: 'vacps',
    version: MCP_PROTOCOL_VERSION,
  });

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
      description: 'List Vacps backends (nodes).',
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

  const requireBackend = async (backendId: unknown) => {
    const backend = await backends.get(String(backendId));
    if (!backend.enabled)
      throw new AppError('backend_disabled', `Backend '${backend.id}' is disabled.`, 409);
    return backend;
  };

  // ── Command / shell / process (sug.md P0+P1) ───────────────────────
  server.registerTool(
    'vacps.command.exec',
    {
      description:
        'Run a non-interactive program on a backend (argv form, no shell). Uses yield_time_ms for sync wait.',
      inputSchema: commandExecInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
      _meta: { tool_schema_revision: TOOL_SCHEMA_REVISION },
    },
    wrap(
      (value) => `command ${String(value.status)} ${String(value.process_id)}`,
      async (args) => {
        const parsed = commandExecInputSchema.parse(args);
        const backend = await requireBackend(parsed.backend_id);
        return (await client.execCommand(backend, parsed)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.shell.exec',
    {
      description:
        'Run a shell command as the agent user with full login environment (bash -lc, sources ~/.bashrc). Prefer vacps.command.exec for non-shell work. Set load_user_environment=false only for a clean --noprofile --norc shell.',
      inputSchema: shellExecInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
      _meta: { tool_schema_revision: TOOL_SCHEMA_REVISION },
    },
    wrap(
      (value) => `shell ${String(value.status)} ${String(value.process_id)}`,
      async (args) => {
        const parsed = shellExecInputSchema.parse(args);
        const backend = await requireBackend(parsed.backend_id);
        return (await client.execShell(backend, parsed)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.process.start',
    {
      description:
        'Start a long-running or interactive process on a backend. Provide exactly one of program or command (not both, not neither). stdout/stderr_hard_max_bytes are 0..1073741824.',
      inputSchema: processStartInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
      _meta: { tool_schema_revision: TOOL_SCHEMA_REVISION },
    },
    wrap(
      (value) => `started ${String(value.process_id)}`,
      async (args) => {
        const parsed = processStartInputSchema.parse(args);
        const backend = await requireBackend(parsed.backend_id);
        return (await client.processStart(backend, parsed)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.process.read',
    {
      description: 'Read stdout/stderr chunks from a process started on a backend.',
      inputSchema: {
        backend_id: z.string(),
        process_id: z.string(),
        cursor: z.string().optional(),
        max_bytes: z.number().int().min(1).max(1_048_576).optional(),
        wait_ms: z.number().int().min(0).max(60_000).optional(),
      },
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    },
    wrap(
      (value) => `process ${String(value.process_id)} ${String(value.status)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.processRead(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.process.write',
    {
      description: 'Write to process stdin on a backend.',
      inputSchema: {
        backend_id: z.string(),
        process_id: z.string(),
        data: z.string(),
        close_stdin: z.boolean().optional(),
      },
      outputSchema: okEnvelope.extend({ process_id: z.string() }).shape,
    },
    wrap(
      (value) => `wrote to ${String(value.process_id)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.processWrite(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.process.terminate',
    {
      description: 'Terminate a process on a backend.',
      inputSchema: {
        backend_id: z.string(),
        process_id: z.string(),
        signal: z.enum(['sigterm', 'sigint', 'sigkill']).optional(),
        grace_period_ms: z.number().int().min(0).max(60_000).optional(),
      },
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    },
    wrap(
      (value) => `terminated ${String(value.process_id)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.processTerminate(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  // ── Files (sug.md P0+P1) ───────────────────────────────────────────
  server.registerTool(
    'vacps.files.read',
    {
      description:
        'Read a file on a backend by absolute path with optional line range and byte cap.',
      inputSchema: {
        backend_id: z.string(),
        path: z.string(),
        start_line: z.number().int().min(1).optional(),
        end_line: z.number().int().min(1).optional(),
        max_bytes: z.number().int().min(1).max(262_144).default(32_768),
        encoding: z.enum(['utf-8', 'base64']).optional(),
      },
      outputSchema: okEnvelope.extend({ path: z.string(), content: z.string() }).shape,
    },
    wrap(
      (value) => `Read ${String(value.path)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.readFile(backend, {
          path: String(args.path),
          startLine: typeof args.start_line === 'number' ? args.start_line : undefined,
          endLine: typeof args.end_line === 'number' ? args.end_line : undefined,
          maxBytes: typeof args.max_bytes === 'number' ? args.max_bytes : 32_768,
          encoding: args.encoding === 'base64' ? 'base64' : 'utf-8',
        })) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.stat',
    {
      description: 'Stat a file or directory on a backend.',
      inputSchema: { backend_id: z.string(), path: z.string() },
      outputSchema: okEnvelope.extend({ path: z.string(), type: z.string() }).shape,
    },
    wrap(
      (value) => `Stat ${String(value.path)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.statFile(backend, String(args.path))) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.list',
    {
      description: 'List directory entries on a backend.',
      inputSchema: {
        backend_id: z.string(),
        path: z.string(),
        limit: z.number().int().min(1).max(2000).optional(),
        include_hidden: z.boolean().optional(),
      },
      outputSchema: okEnvelope.extend({ matches: z.array(z.unknown()) }).shape,
    },
    wrap(
      (value) => `Listed ${(value.matches as unknown[])?.length ?? 0} entries`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.listDir(backend, {
          path: String(args.path),
          limit: typeof args.limit === 'number' ? args.limit : 200,
          includeHidden: args.include_hidden === true,
        })) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.glob',
    {
      description:
        'Glob files on a backend. Dialect is bash-like globstar: * is one segment, ** matches zero or more segments (so **/*.txt matches example.txt at the root). Uses rg when available.',
      inputSchema: {
        backend_id: z.string(),
        pattern: z.string(),
        path: z.string().optional(),
        include_hidden: z.boolean().optional(),
        respect_gitignore: z.boolean().optional(),
        limit: z.number().int().min(1).max(2000).optional(),
      },
      outputSchema: okEnvelope.extend({ matches: z.array(z.unknown()) }).shape,
    },
    wrap(
      (value) => `Glob ${(value.matches as unknown[])?.length ?? 0} matches`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.glob(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.grep',
    {
      description:
        'Search file contents on a backend (ripgrep when available). path may be a file or a directory; when a file is given, only that file is searched.',
      inputSchema: {
        backend_id: z.string(),
        pattern: z.string(),
        path: z.string().optional(),
        file_pattern: z.string().optional(),
        case_sensitive: z.boolean().optional(),
        fixed_string: z.boolean().optional(),
        context_before: z.number().int().min(0).max(10).optional(),
        context_after: z.number().int().min(0).max(10).optional(),
        max_matches: z.number().int().min(1).max(500).optional(),
        max_bytes: z.number().int().min(1).max(262_144).optional(),
      },
      outputSchema: okEnvelope.extend({ matches: z.array(z.unknown()) }).shape,
    },
    wrap(
      (value) =>
        `Grep ${String(value.match_count ?? (value.matches as unknown[])?.length ?? 0)} hits`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.grep(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.edit',
    {
      description:
        'Exact string replacement in a file on a backend (old_text must be unique unless replace_all).',
      inputSchema: {
        backend_id: z.string(),
        path: z.string(),
        old_text: z.string(),
        new_text: z.string(),
        replace_all: z.boolean().optional(),
        expected_sha256: z.string().optional(),
        idempotency_key: z.string().optional(),
      },
      outputSchema: okEnvelope.extend({ path: z.string(), replacement_count: z.number() }).shape,
    },
    wrap(
      (value) => `Edited ${String(value.path)} ×${String(value.replacement_count)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.editFile(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.write',
    {
      description:
        'Write a file on a backend (atomic temp+rename). mode is required: create (fail if exists), overwrite (fail if missing), or create_or_overwrite. Supports idempotency_key + expected_sha256.',
      inputSchema: filesWriteInputSchema,
      outputSchema: okEnvelope.extend({ path: z.string(), operation: z.string() }).shape,
      _meta: { tool_schema_revision: TOOL_SCHEMA_REVISION },
    },
    wrap(
      (value) => `Wrote ${String(value.path)} (${String(value.operation)})`,
      async (args) => {
        const parsed = filesWriteInputSchema.parse(args);
        const backend = await requireBackend(parsed.backend_id);
        return (await client.writeFile(backend, parsed)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.apply_patch',
    {
      description: 'Apply a Codex-style multi-file patch on a backend.',
      inputSchema: {
        backend_id: z.string(),
        patch: z.string(),
        workspace_path: z.string().optional(),
        dry_run: z.boolean().optional(),
        atomic: z.boolean().optional(),
        idempotency_key: z.string().optional(),
      },
      outputSchema: okEnvelope.extend({ files: z.array(z.unknown()) }).shape,
    },
    wrap(
      (value) => `Patch ${(value.files as unknown[])?.length ?? 0} files`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.applyPatch(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.move',
    {
      description:
        'Move/rename a file on a backend. Optional expected_sha256 for optimistic concurrency.',
      inputSchema: {
        backend_id: z.string(),
        from: z.string(),
        to: z.string(),
        overwrite: z.boolean().optional(),
        expected_sha256: z.string().optional(),
        idempotency_key: z.string().optional(),
      },
      outputSchema: okEnvelope.extend({ from: z.string(), to: z.string() }).shape,
    },
    wrap(
      (value) => `Moved ${String(value.from)} → ${String(value.to)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.moveFile(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.delete',
    {
      description:
        'Delete a file or directory on a backend. Use dry_run to preview size/count; expected_sha256 / expected_type for safety.',
      inputSchema: {
        backend_id: z.string(),
        path: z.string(),
        recursive: z.boolean().optional(),
        expected_sha256: z.string().optional(),
        expected_type: z.enum(['file', 'directory']).optional(),
        dry_run: z.boolean().optional(),
        idempotency_key: z.string().optional(),
      },
      outputSchema: okEnvelope.extend({ path: z.string(), operation: z.string() }).shape,
    },
    wrap(
      (value) => `Deleted ${String(value.path)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.deleteFile(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.capabilities.get',
    {
      description:
        'Report backend tool capabilities (ripgrep availability, grep engine, glob dialect, shell environment, limits).',
      inputSchema: capabilitiesGetInputSchema,
      outputSchema: okEnvelope.extend({ features: z.unknown(), executables: z.unknown() }).shape,
      _meta: { tool_schema_revision: TOOL_SCHEMA_REVISION },
    },
    wrap(
      () => 'capabilities',
      async (args) => {
        const parsed = capabilitiesGetInputSchema.parse(args);
        const backend = await requireBackend(parsed.backend_id);
        return (await client.getCapabilities(backend)) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.files.mkdir',
    {
      description: 'Create a directory on a backend.',
      inputSchema: {
        backend_id: z.string(),
        path: z.string(),
        recursive: z.boolean().optional(),
      },
      outputSchema: okEnvelope.extend({ path: z.string(), operation: z.string() }).shape,
    },
    wrap(
      (value) => `mkdir ${String(value.path)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.mkdir(backend, args)) as Record<string, unknown>;
      },
    ),
  );

  // ── Git helpers (P1; thin shell wrappers) ───────────────────────────
  server.registerTool(
    'vacps.git.status',
    {
      description: 'git status --short on a backend working tree.',
      inputSchema: {
        backend_id: z.string(),
        working_directory: z.string().optional(),
      },
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    },
    wrap(
      (value) => `git status ${String(value.status)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        return (await client.execCommand(backend, {
          program: 'git',
          arguments: ['status', '--short'],
          working_directory: args.working_directory,
          timeout_ms: 60_000,
          yield_time_ms: 30_000,
        })) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.git.diff',
    {
      description: 'git diff on a backend working tree.',
      inputSchema: {
        backend_id: z.string(),
        working_directory: z.string().optional(),
        staged: z.boolean().optional(),
      },
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    },
    wrap(
      (value) => `git diff ${String(value.status)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        const arguments_ = args.staged === true ? ['diff', '--cached'] : ['diff'];
        return (await client.execCommand(backend, {
          program: 'git',
          arguments: arguments_,
          working_directory: args.working_directory,
          timeout_ms: 60_000,
          yield_time_ms: 30_000,
          stdout_max_bytes: 65_536,
        })) as Record<string, unknown>;
      },
    ),
  );

  server.registerTool(
    'vacps.git.apply',
    {
      description: 'Apply a unified diff with git apply on a backend.',
      inputSchema: {
        backend_id: z.string(),
        patch: z.string(),
        working_directory: z.string().optional(),
        check: z.boolean().optional(),
      },
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    },
    wrap(
      (value) => `git apply ${String(value.status)}`,
      async (args) => {
        const backend = await requireBackend(args.backend_id);
        const path = `/tmp/vacps-git-apply-${crypto.randomUUID()}.patch`;
        await client.writeFile(backend, {
          path,
          content: String(args.patch),
          mode: 'create_or_overwrite',
          create_parent_directories: true,
        });
        try {
          return (await client.execCommand(backend, {
            program: 'git',
            arguments: args.check === true ? ['apply', '--check', path] : ['apply', path],
            working_directory: args.working_directory,
            timeout_ms: 60_000,
            yield_time_ms: 30_000,
          })) as Record<string, unknown>;
        } finally {
          await client.deleteFile(backend, { path }).catch(() => undefined);
        }
      },
    ),
  );

  // MCP SDK only emits object schemas from Zod; inject process.start oneOf for discoverability.
  patchProcessStartListSchema(server);

  return server;
}

/**
 * Wrap tools/list so vacps.process.start advertises program XOR command via oneOf.
 * CallTool still validates with processStartInputSchema (Zod superRefine).
 */
function patchProcessStartListSchema(server: McpServer): void {
  const protocol = server.server as unknown as {
    _requestHandlers: Map<
      string,
      (request: unknown, extra: unknown) => unknown | Promise<unknown>
    >;
  };
  const previous = protocol._requestHandlers.get('tools/list');
  if (!previous) return;

  protocol._requestHandlers.set('tools/list', async (request, extra) => {
    const result = (await previous(request, extra)) as {
      tools?: Array<{ name?: string; inputSchema?: Record<string, unknown> }>;
    };
    for (const tool of result.tools ?? []) {
      if (tool.name === 'vacps.process.start') {
        tool.inputSchema = { ...processStartListJsonSchema };
      }
    }
    return result;
  });
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
  if (
    code.includes('validation') ||
    code === 'invalid_request' ||
    code.startsWith('invalid_') ||
    code === 'path_not_allowed' ||
    code === 'path_not_directory' ||
    code === 'path_not_found'
  )
    return 'validation';
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
