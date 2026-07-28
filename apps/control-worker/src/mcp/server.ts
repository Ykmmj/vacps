import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { taskStatuses } from '@vacps/contracts';
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
  parseScheduleCreate,
  parseSchedulePatch,
  schedulesCreateInputSchema,
  schedulesGetInputSchema,
  schedulesIdInputSchema,
  schedulesListInputSchema,
  schedulesUpdateInputSchema,
  taskCreateResult,
  tasksCreateAgentInputSchema,
  tasksCreateCommandInputSchema,
  tasksCreateShellInputSchema,
  tasksGetInputSchema,
  tasksIdInputSchema,
  tasksListInputSchema,
  tasksOutputReadInputSchema,
  toCreateAgentTask,
  toCreateCommandTask,
  toCreateShellTask,
} from './task-schedule-adapters.js';
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
  processStartCommandInputSchema,
  processStartShellInputSchema,
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
    // Schema v3: do not opt into experimental MCP protocol tasks yet (vacps.tasks.* is the model).
    execution: { taskSupport: 'forbidden' as const },
    _meta: {
      tool_schema_revision: TOOL_SCHEMA_REVISION,
      tool_schema_version: '3.0',
      ...(config._meta ?? {}),
    },
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
        details: app.details ?? {},
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

  // ── Layer B: tasks (Schema v3 split create tools only) ─────────────
  const taskCreateOutput = okEnvelope.extend({
    task: z.unknown(),
    output: z.unknown(),
    poll: z.unknown(),
    idempotency: z.unknown().optional(),
  }).shape;

  server.registerTool(
    'vacps.tasks.create_command',
    toolConfig('vacps.tasks.create_command', {
      description: 'Queue a non-interactive argv program task on a backend.',
      inputSchema: tasksCreateCommandInputSchema,
      outputSchema: taskCreateOutput,
    }),
    wrap(async (args) => {
      const parsed = tasksCreateCommandInputSchema.parse(args);
      const input = toCreateCommandTask(parsed);
      const created = await tasks.create(input, 'mcp', undefined, 'command');
      return taskCreateResult(created, parsed.idempotency_key, 'command');
    }),
  );

  server.registerTool(
    'vacps.tasks.create_shell',
    toolConfig('vacps.tasks.create_shell', {
      description:
        'Queue a shell-string task on a backend (bash -lc by default). Prefer create_command when no shell is needed.',
      inputSchema: tasksCreateShellInputSchema,
      outputSchema: taskCreateOutput,
    }),
    wrap(async (args) => {
      const parsed = tasksCreateShellInputSchema.parse(args);
      const input = toCreateShellTask(parsed);
      const created = await tasks.create(input, 'mcp', undefined, 'shell');
      return taskCreateResult(created, parsed.idempotency_key, 'shell');
    }),
  );

  server.registerTool(
    'vacps.tasks.create_agent',
    toolConfig('vacps.tasks.create_agent', {
      description: 'Queue an agent prompt task on a backend.',
      inputSchema: tasksCreateAgentInputSchema,
      outputSchema: taskCreateOutput,
    }),
    wrap(async (args) => {
      const parsed = tasksCreateAgentInputSchema.parse(args);
      const input = toCreateAgentTask(parsed);
      const created = await tasks.create(input, 'mcp', undefined, 'agent');
      return taskCreateResult(created, parsed.idempotency_key, 'agent');
    }),
  );

  server.registerTool(
    'vacps.tasks.get',
    toolConfig('vacps.tasks.get', {
      description: 'Get task status plus optional command list and output previews.',
      inputSchema: tasksGetInputSchema,
      outputSchema: okEnvelope.extend({
        task: z.unknown(),
        result: z.unknown().optional(),
        output: z.unknown().optional(),
        commands: z.unknown().optional(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = tasksGetInputSchema.parse(args);
      const detail = await tasks.detail(parsed.task_id, {
        includeCommands: Boolean(parsed.include_commands),
        includeOutputPreview: parsed.include_output_preview !== false,
        previewMaxBytes: parsed.preview_max_bytes ?? 8192,
      });
      return {
        task: snakeTask(detail.task),
        ...(detail.result !== undefined ? { result: detail.result } : {}),
        ...(detail.output !== undefined ? { output: detail.output } : {}),
        ...(detail.commands !== undefined ? { commands: detail.commands } : {}),
      };
    }),
  );

  server.registerTool(
    'vacps.tasks.output.read',
    toolConfig('vacps.tasks.output.read', {
      description: 'Read a task stdout/stderr stream by absolute byte offset.',
      inputSchema: tasksOutputReadInputSchema,
      outputSchema: okEnvelope.extend({
        task_id: z.string().optional(),
        stream: z.string().optional(),
        offset: z.number().optional(),
        next_offset: z.number().optional(),
        eof: z.boolean().optional(),
        content: z.string().optional(),
        encoding: z.string().optional(),
        stream_version: z.string().optional(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = tasksOutputReadInputSchema.parse(args);
      return (await tasks.readOutput(parsed.task_id, {
        stream: parsed.stream ?? 'stdout',
        offset: parsed.offset ?? 0,
        maxBytes: parsed.max_bytes ?? 65_536,
        ...(parsed.expected_stream_version
          ? { expectedStreamVersion: parsed.expected_stream_version }
          : {}),
      })) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.tasks.list',
    toolConfig('vacps.tasks.list', {
      description: 'List task summaries with optional filters and opaque cursor pagination.',
      inputSchema: tasksListInputSchema,
      outputSchema: okEnvelope.extend({
        tasks: z.array(z.unknown()),
        returned_count: z.number(),
        next_cursor: z.string().nullable(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = tasksListInputSchema.parse(args);
      const offset = parsed.cursor ? decodeOffsetCursor(parsed.cursor) : 0;
      const page = await tasks.listPage({
        limit: parsed.limit ?? 50,
        offset,
        ...(parsed.backend_id ? { backendId: parsed.backend_id } : {}),
        ...(parsed.kind ? { kind: parsed.kind } : {}),
        ...(parsed.status ? { status: parsed.status } : {}),
        ...(parsed.created_after ? { createdAfter: parsed.created_after } : {}),
      });
      return {
        tasks: page.tasks.map(snakeTask),
        returned_count: page.returned_count,
        next_cursor:
          page.next_offset !== null ? encodeOffsetCursor(page.next_offset) : null,
      };
    }),
  );

  server.registerTool(
    'vacps.tasks.cancel',
    toolConfig('vacps.tasks.cancel', {
      description: 'Cancel a queued or running task.',
      inputSchema: tasksIdInputSchema,
      outputSchema: okEnvelope.extend({ task: z.unknown() }).shape,
    }),
    wrap(async (args) => {
      const parsed = tasksIdInputSchema.parse(args);
      const result = (await tasks.cancel(parsed.task_id)) as { task: unknown };
      return { task: snakeTask(result.task as never) };
    }),
  );

  server.registerTool(
    'vacps.tasks.retry',
    toolConfig('vacps.tasks.retry', {
      description: 'Retry a task (creates a new task when possible).',
      inputSchema: tasksIdInputSchema,
      outputSchema: okEnvelope.extend({ task: z.unknown() }).shape,
    }),
    wrap(async (args) => {
      const parsed = tasksIdInputSchema.parse(args);
      const result = (await tasks.retry(parsed.task_id)) as { task: unknown };
      return { task: snakeTask(result.task as never) };
    }),
  );

  // ── Layer B: schedules (Schema v3 trigger/policy/task only) ────────
  server.registerTool(
    'vacps.schedules.create',
    toolConfig('vacps.schedules.create', {
      description:
        'Create a cron schedule with trigger, policy, and task (kind command|shell|agent). Task inherits schedule backend_id.',
      inputSchema: schedulesCreateInputSchema,
      outputSchema: okEnvelope.extend({ schedule: z.unknown() }).shape,
    }),
    wrap(async (raw) => {
      const parsed = schedulesCreateInputSchema.parse(raw);
      const created = await schedules.create(parseScheduleCreate(parsed));
      return {
        schedule: snakeSchedule(created),
        idempotency: {
          key: parsed.idempotency_key ?? created.idempotency_key ?? null,
          replayed: Boolean(created.reused),
          request_hash: created.requestHash ?? null,
        },
      };
    }),
  );
  server.registerTool(
    'vacps.schedules.get',
    toolConfig('vacps.schedules.get', {
      description: 'Get a schedule definition including revision, trigger, and policy.',
      inputSchema: schedulesGetInputSchema,
      outputSchema: okEnvelope.extend({ schedule: z.unknown() }).shape,
    }),
    wrap(async (args) => {
      const parsed = schedulesGetInputSchema.parse(args);
      return { schedule: snakeSchedule(await schedules.get(parsed.schedule_id)) };
    }),
  );
  server.registerTool(
    'vacps.schedules.list',
    toolConfig('vacps.schedules.list', {
      description: 'List cron schedules with optional backend/enabled filters and cursor.',
      inputSchema: schedulesListInputSchema,
      outputSchema: okEnvelope.extend({
        schedules: z.array(z.unknown()),
        returned_count: z.number(),
        next_cursor: z.string().nullable(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = schedulesListInputSchema.parse(args);
      const offset = parsed.cursor ? decodeOffsetCursor(parsed.cursor) : 0;
      const page = await schedules.list({
        limit: parsed.limit ?? 50,
        offset,
        ...(parsed.backend_id ? { backendId: parsed.backend_id } : {}),
        ...(typeof parsed.enabled === 'boolean' ? { enabled: parsed.enabled } : {}),
      });
      return {
        schedules: page.schedules.map(snakeSchedule),
        returned_count: page.returned_count,
        next_cursor:
          page.next_offset !== null ? encodeOffsetCursor(page.next_offset) : null,
      };
    }),
  );
  server.registerTool(
    'vacps.schedules.update',
    toolConfig('vacps.schedules.update', {
      description:
        'Patch a schedule (Schema v3). Pass expected_revision for optimistic concurrency; only name/enabled/trigger/policy/task in changes are applied.',
      inputSchema: schedulesUpdateInputSchema,
      outputSchema: okEnvelope.extend({ schedule: z.unknown() }).shape,
    }),
    wrap(async (args) => {
      const parsed = schedulesUpdateInputSchema.parse(args);
      const current = await schedules.get(parsed.schedule_id);
      const patch = parseSchedulePatch(parsed, current.backend_id);
      return { schedule: snakeSchedule(await schedules.patch(parsed.schedule_id, patch)) };
    }),
  );
  server.registerTool(
    'vacps.schedules.delete',
    toolConfig('vacps.schedules.delete', {
      description:
        'Delete a cron schedule. Natural idempotent when absent (already_absent). With idempotency_key, replays the stored result.',
      inputSchema: schedulesIdInputSchema,
      outputSchema: okEnvelope.extend({
        deleted: z.boolean(),
        schedule_id: z.string(),
        already_absent: z.boolean().optional(),
        idempotency: z.unknown().optional(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = schedulesIdInputSchema.parse(args);
      return (await schedules.delete(parsed.schedule_id, {
        ...(parsed.idempotency_key ? { idempotencyKey: parsed.idempotency_key } : {}),
      })) as Record<string, unknown>;
    }),
  );
  server.registerTool(
    'vacps.schedules.run_now',
    toolConfig('vacps.schedules.run_now', {
      description: 'Immediately queue a schedule once. Supports idempotency_key.',
      inputSchema: schedulesIdInputSchema,
      outputSchema: okEnvelope.extend({
        schedule_id: z.string(),
        task_id: z.string().optional(),
        task: z.unknown(),
        queued: z.boolean(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = schedulesIdInputSchema.parse(args);
      const result = await schedules.runNow(parsed.schedule_id, {
        ...(parsed.idempotency_key ? { idempotencyKey: parsed.idempotency_key } : {}),
      });
      return {
        schedule_id: result.scheduleId,
        task_id: result.task.id,
        task: snakeTask(result.task),
        queued: true,
      };
    }),
  );

  const requireBackend = async (backendId: unknown) => {
    const backend = await backends.get(String(backendId));
    if (!backend.enabled)
      throw new AppError('backend_disabled', `Backend '${backend.id}' is disabled.`, 409);
    return backend;
  };

  // ── Command / shell / process (Schema v3) ──────────────────────────
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

  const processStartOutput = okEnvelope.extend({
    process_id: z.string(),
    status: z.string(),
  }).shape;

  server.registerTool(
    'vacps.process.start_command',
    toolConfig('vacps.process.start_command', {
      description:
        'Start a long-running or interactive argv process (program + arguments). Returns a full Process Snapshot.',
      inputSchema: processStartCommandInputSchema,
      outputSchema: processStartOutput,
    }),
    wrap(async (args) => {
      const parsed = processStartCommandInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.processStartCommand(backend, {
        program: parsed.program,
        ...(parsed.arguments ? { arguments: parsed.arguments } : {}),
        ...(parsed.working_directory ? { working_directory: parsed.working_directory } : {}),
        ...(parsed.environment ? { environment: parsed.environment } : {}),
        ...(typeof parsed.tty === 'boolean' ? { tty: parsed.tty } : {}),
        ...(parsed.timeout_ms !== undefined ? { timeout_ms: parsed.timeout_ms } : {}),
        ...(parsed.stdout_hard_max_bytes !== undefined
          ? { stdout_hard_max_bytes: parsed.stdout_hard_max_bytes }
          : {}),
        ...(parsed.stderr_hard_max_bytes !== undefined
          ? { stderr_hard_max_bytes: parsed.stderr_hard_max_bytes }
          : {}),
        ...(parsed.idempotency_key ? { idempotency_key: parsed.idempotency_key } : {}),
      })) as Record<string, unknown>;
    }),
  );

  server.registerTool(
    'vacps.process.start_shell',
    toolConfig('vacps.process.start_shell', {
      description:
        'Start a long-running or interactive shell-string process. Returns a full Process Snapshot.',
      inputSchema: processStartShellInputSchema,
      outputSchema: processStartOutput,
    }),
    wrap(async (args) => {
      const parsed = processStartShellInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      return (await client.processStartShell(backend, {
        command: parsed.command,
        ...(parsed.shell ? { shell: parsed.shell } : {}),
        ...(typeof parsed.load_user_environment === 'boolean'
          ? { load_user_environment: parsed.load_user_environment }
          : {}),
        ...(parsed.working_directory ? { working_directory: parsed.working_directory } : {}),
        ...(parsed.environment ? { environment: parsed.environment } : {}),
        ...(typeof parsed.tty === 'boolean' ? { tty: parsed.tty } : {}),
        ...(parsed.timeout_ms !== undefined ? { timeout_ms: parsed.timeout_ms } : {}),
        ...(parsed.stdout_hard_max_bytes !== undefined
          ? { stdout_hard_max_bytes: parsed.stdout_hard_max_bytes }
          : {}),
        ...(parsed.stderr_hard_max_bytes !== undefined
          ? { stderr_hard_max_bytes: parsed.stderr_hard_max_bytes }
          : {}),
        ...(parsed.idempotency_key ? { idempotency_key: parsed.idempotency_key } : {}),
      })) as Record<string, unknown>;
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

  // ── Files (Schema v3) ──────────────────────────────────────────────
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
        entries: z.array(z.unknown()),
        returned_count: z.number().optional(),
        truncated: z.boolean().optional(),
        next_cursor: z.string().nullable().optional(),
      }).shape,
    }),
    wrap(async (args) => {
      const parsed = filesListInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      const raw = (await client.listDir(backend, {
        path: parsed.path,
        limit: parsed.limit ?? 200,
        includeHidden: parsed.include_hidden === true,
        ...(parsed.cursor ? { cursor: parsed.cursor } : {}),
      })) as Record<string, unknown>;
      // Schema v3: agent returns entries only; no matches alias.
      const entries = Array.isArray(raw.entries) ? raw.entries : [];
      return {
        entries,
        returned_count:
          typeof raw.returned_count === 'number' ? raw.returned_count : entries.length,
        truncated: Boolean(raw.truncated),
        next_cursor: (raw.next_cursor as string | null | undefined) ?? null,
      };
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
      description:
        'Apply a unified diff with git apply on a backend. Supports idempotency_key for safe retries.',
      inputSchema: gitApplyInputSchema,
      outputSchema: okEnvelope.extend({ process_id: z.string(), status: z.string() }).shape,
    }),
    wrap(async (args) => {
      const parsed = gitApplyInputSchema.parse(args);
      const backend = await requireBackend(parsed.backend_id);
      // Stable temp path when idempotent so retries hit the same request hash on write+apply.
      const path = parsed.idempotency_key
        ? `/tmp/vacps-git-apply-${parsed.backend_id}-${parsed.idempotency_key.replace(/[^A-Za-z0-9._:-]/g, '_')}.patch`
        : `/tmp/vacps-git-apply-${crypto.randomUUID()}.patch`;
      await client.writeFile(backend, {
        path,
        content: parsed.patch,
        mode: 'create_or_overwrite',
        create_parent_directories: true,
        ...(parsed.idempotency_key
          ? { idempotency_key: `git-apply-write:${parsed.idempotency_key}` }
          : {}),
      });
      try {
        return (await client.execCommand(backend, {
          program: 'git',
          arguments: parsed.check === true ? ['apply', '--check', path] : ['apply', path],
          working_directory: parsed.working_directory,
          timeout_ms: 60_000,
          yield_time_ms: 30_000,
          ...(parsed.idempotency_key
            ? { idempotency_key: `git-apply:${parsed.idempotency_key}` }
            : {}),
        })) as Record<string, unknown>;
      } finally {
        if (!parsed.idempotency_key) {
          await client.deleteFile(backend, { path }).catch(() => undefined);
        }
      }
    }),
  );

  // Schema v3 list patches: annotations + schema version meta (no oneOf injection).
  applySchemaV3ListPatches(server);

  return server;
}

/** Stable hash of advertised tool schemas for Host cache invalidation. */
export async function computeToolSchemaHash(): Promise<string> {
  const { publicToolJsonSchemas } = await import('./tool-schemas.js');
  const payload = JSON.stringify(publicToolJsonSchemas());
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(payload));
  return `sha256:${[...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('')}`;
}

/**
 * Schema v3 tools/list patches:
 * - ensure annotations present on every tool
 * - stamp tool_schema_version / revision meta
 * - strip nested $schema keywords from Zod-generated JSON Schema
 */
function applySchemaV3ListPatches(server: McpServer): void {
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
    tool._meta = {
      tool_schema_revision: TOOL_SCHEMA_REVISION,
      tool_schema_version: '3.0',
      ...(tool._meta ?? {}),
    };
  }

  const previous = host.server._requestHandlers.get('tools/list');
  if (!previous) return;

  host.server._requestHandlers.set('tools/list', async (request, extra) => {
    const result = (await previous(request, extra)) as {
      tools?: Array<{
        name?: string;
        inputSchema?: Record<string, unknown>;
        annotations?: unknown;
        _meta?: Record<string, unknown>;
      }>;
      _meta?: Record<string, unknown>;
    };
    for (const tool of result.tools ?? []) {
      if (tool.name) {
        tool.annotations = annotationsFor(tool.name);
        tool._meta = {
          ...(tool._meta ?? {}),
          tool_schema_version: '3.0',
          tool_schema_revision: TOOL_SCHEMA_REVISION,
        };
      }
      // Strip nested $schema from Zod-generated property schemas for dialect consistency.
      if (tool.inputSchema) stripNestedSchemaKeywords(tool.inputSchema);
    }
    result._meta = {
      ...(result._meta ?? {}),
      tool_schema_version: '3.0',
      tool_schema_revision: TOOL_SCHEMA_REVISION,
      mcp_server_version: MCP_PROTOCOL_VERSION,
    };
    return result;
  });
}

function stripNestedSchemaKeywords(schema: Record<string, unknown>, depth = 0): void {
  if (depth > 12 || !schema || typeof schema !== 'object') return;
  if (depth > 0 && '$schema' in schema) delete schema.$schema;
  for (const value of Object.values(schema)) {
    if (value && typeof value === 'object') {
      if (Array.isArray(value)) {
        for (const item of value) {
          if (item && typeof item === 'object')
            stripNestedSchemaKeywords(item as Record<string, unknown>, depth + 1);
        }
      } else {
        stripNestedSchemaKeywords(value as Record<string, unknown>, depth + 1);
      }
    }
  }
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

/** Normalize agent capabilities into Schema v3 nested shape (no dotted feature keys). */
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

function snakeTask(task: {
  id: string;
  backendId: string;
  kind?: string | undefined;
  source?: string | undefined;
  profile?: string | undefined;
  name?: string | undefined;
  summary?: string | undefined;
  status: string;
  scheduleId?: string | undefined;
  idempotencyKey?: string | undefined;
  requestHash?: string | undefined;
  retryOfTaskId?: string | undefined;
  createdAt: string;
  updatedAt: string;
  finishedAt?: string | undefined;
  reusedExistingTask?: boolean | undefined;
}) {
  return {
    id: task.id,
    backend_id: task.backendId,
    kind: task.kind ?? 'command',
    ...(task.source ? { source: task.source } : {}),
    ...(task.profile ? { profile: task.profile } : {}),
    ...(task.name ? { name: task.name } : {}),
    ...(task.summary ? { summary: task.summary } : {}),
    status: task.status,
    ...(task.scheduleId ? { schedule_id: task.scheduleId } : {}),
    ...(task.idempotencyKey ? { idempotency_key: task.idempotencyKey } : {}),
    ...(task.requestHash ? { request_hash: task.requestHash } : {}),
    ...(task.retryOfTaskId ? { retry_of_task_id: task.retryOfTaskId } : {}),
    created_at: task.createdAt,
    updated_at: task.updatedAt,
    ...(task.finishedAt ? { finished_at: task.finishedAt } : {}),
    cancellable: !['succeeded', 'failed', 'cancelled', 'timed_out', 'dispatch_failed'].includes(
      task.status,
    ),
  };
}

/** Schedule is already Schema v3 wire shape (snake_case); strip internal-only fields. */
function snakeSchedule(schedule: {
  id: string;
  backend_id: string;
  name: string;
  trigger: { type: 'cron'; expression: string; timezone: string };
  policy: { concurrency: string; misfire: string; max_catchup_runs: number };
  enabled: boolean;
  revision: number;
  task: unknown;
  idempotency_key?: string | undefined;
  requestHash?: string | undefined;
  last_run_at?: string | undefined;
  next_run_at?: string | undefined;
  created_at: string;
  updated_at: string;
}) {
  return {
    id: schedule.id,
    revision: schedule.revision,
    backend_id: schedule.backend_id,
    name: schedule.name,
    enabled: schedule.enabled,
    trigger: schedule.trigger,
    policy: schedule.policy,
    task: schedule.task,
    ...(schedule.idempotency_key ? { idempotency_key: schedule.idempotency_key } : {}),
    ...(schedule.requestHash ? { request_hash: schedule.requestHash } : {}),
    ...(schedule.last_run_at ? { last_run_at: schedule.last_run_at } : {}),
    ...(schedule.next_run_at ? { next_run_at: schedule.next_run_at } : {}),
    created_at: schedule.created_at,
    updated_at: schedule.updated_at,
  };
}

/** Recursively convert object keys to snake_case for Schema v3 status payloads. */
function snakeStatus(status: unknown): unknown {
  if (Array.isArray(status)) return status.map((item) => snakeStatus(item));
  if (!status || typeof status !== 'object') return status;
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(status as Record<string, unknown>)) {
    const snake = key
      .replace(/([a-z0-9])([A-Z])/g, '$1_$2')
      .replace(/([A-Z]+)([A-Z][a-z])/g, '$1_$2')
      .toLowerCase();
    out[snake] = snakeStatus(value);
  }
  return out;
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
