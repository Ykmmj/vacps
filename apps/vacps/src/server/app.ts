import { open, stat } from 'node:fs/promises';
import { userInfo } from 'node:os';

import Fastify, { type FastifyInstance } from 'fastify';
import { createTaskSchema, taskDispatchSchema } from '@vacps/contracts';
import type { AgentConfig } from '../config.js';
import type { PiRuntime } from '../pi/pi-runtime.js';
import type { TaskQueue } from '../queue/task-queue.js';
import type { TaskStore } from '../storage/task-store.js';
import type { NodeTelemetryCollector } from '../telemetry/node-telemetry.js';
import { verifyControlPlaneRequest } from '../security/request-signatures.js';
import { ProcessManager } from '../runtime/process-manager.js';
import { hashRequest, IdempotencyStore } from '../runtime/idempotency.js';
import * as files from '../runtime/files.js';

export async function createServer(input: {
  config: AgentConfig;
  queue: TaskQueue;
  piRuntime: PiRuntime;
  telemetry: NodeTelemetryCollector;
  store: TaskStore;
}): Promise<FastifyInstance> {
  const app = Fastify({ logger: true, bodyLimit: 2 * 1024 * 1024 });
  const processes = new ProcessManager(input.config.BACKEND_ID);
  const idempotency = new IdempotencyStore();
  app.addHook('preValidation', async (request, reply) => {
    try {
      const body = request.body === undefined ? '' : JSON.stringify(request.body);
      const { nonce } = await verifyControlPlaneRequest(input.config, {
        method: request.method,
        url: request.url,
        headers: request.headers,
        body,
      });
      if (
        !input.store.claimControlPlaneNonce(
          nonce,
          new Date(Date.now() + 5 * 60 * 1000).toISOString(),
        )
      ) {
        return reply.code(401).send({
          error: {
            code: 'replayed_request',
            message: 'A control-plane request may only be used once.',
          },
        });
      }
    } catch (error) {
      return reply.code(401).send({
        error: {
          code: 'unauthorized',
          message:
            error instanceof Error ? error.message : 'A valid control-plane signature is required.',
        },
      });
    }
  });

  app.get('/health', async () => {
    const health = await input.telemetry.collect();
    const agentEnv = await probeAgentEnvironment();
    return {
      ...health.health,
      agent_environment: agentEnv,
      // shell_exec is only healthy when the agent can use its real home + bashrc.
      shell_environment_ok: agentEnv.home_accessible && agentEnv.shell_smoke_ok,
    };
  });

  app.get('/info', async () => ({
    backendId: input.config.BACKEND_ID,
    queue: input.queue.queueName,
    runMode: input.config.RUN_MODE,
    agent_environment: await probeAgentEnvironment(),
  }));

  app.get('/metrics', async () => (await input.telemetry.collect()).metrics);

  app.post('/tasks', async (request, reply) => {
    const parsed = taskDispatchSchema.safeParse(request.body);
    if (!parsed.success)
      return reply
        .code(400)
        .send({ error: { code: 'invalid_task', message: parsed.error.message } });
    if (parsed.data.backendId !== input.config.BACKEND_ID) {
      return reply
        .code(409)
        .send({ error: { code: 'backend_mismatch', message: 'Task targets another backend.' } });
    }
    await input.queue.enqueue(parsed.data);
    return reply
      .code(202)
      .send({ taskId: parsed.data.taskId, status: 'queued', backendId: parsed.data.backendId });
  });

  app.get('/tasks/:id', async (request, reply) => {
    const { id } = request.params as { id: string };
    const task = await input.queue.getTask(id);
    if (!task.task)
      return reply.code(404).send({ error: { code: 'not_found', message: 'Task not found.' } });
    return task;
  });

  app.get('/tasks/:id/logs', async (request, reply) => {
    const { id } = request.params as { id: string };
    const query = request.query as {
      stream?: string;
      offset?: string;
      max_bytes?: string;
      preview_max_bytes?: string;
    };
    const task = await input.queue.getTask(id);
    if (!task.task)
      return reply.code(404).send({ error: { code: 'not_found', message: 'Task not found.' } });

    // Offset-based stream read (vacps.tasks.output.read).
    if (query.stream === 'stdout' || query.stream === 'stderr') {
      const path = pickStreamPath(task.commands, query.stream);
      if (!path)
        return reply
          .code(404)
          .send({ error: { code: 'output_not_available', message: 'Stream is not available.' } });
      const offset = Math.max(0, Number(query.offset ?? 0) || 0);
      const maxBytes = Math.min(
        Math.max(Number(query.max_bytes ?? 65_536) || 65_536, 1),
        1_048_576,
      );
      const slice = await readFileSlice(path, offset, maxBytes);
      return {
        task_id: id,
        stream: query.stream,
        offset,
        next_offset: slice.nextOffset,
        eof: slice.eof,
        total_bytes: slice.totalBytes,
        truncated: false,
        expired: false,
        encoding: 'utf-8',
        data: slice.data,
      };
    }

    const previewMax = Math.min(
      Math.max(Number(query.preview_max_bytes ?? 8192) || 8192, 0),
      65_536,
    );
    const commands = await Promise.all(
      task.commands.map(async (command) => {
        const stdoutPath = command.stdoutPath;
        const stderrPath = command.stderrPath;
        const [stdoutMeta, stderrMeta] = await Promise.all([
          stdoutPath ? previewFile(stdoutPath, previewMax) : emptyPreview(),
          stderrPath ? previewFile(stderrPath, previewMax) : emptyPreview(),
        ]);
        return {
          ...command,
          stdout: stdoutMeta.preview,
          stderr: stderrMeta.preview,
          stdout_bytes: stdoutMeta.totalBytes,
          stderr_bytes: stderrMeta.totalBytes,
          stdout_truncated: stdoutMeta.truncated,
          stderr_truncated: stderrMeta.truncated,
          stdout_complete: Boolean(command.finishedAt),
          stderr_complete: Boolean(command.finishedAt),
        };
      }),
    );
    return { taskId: id, commands };
  });

  // ── Files ──────────────────────────────────────────────────────────
  app.get('/fs/read', async (request, reply) => {
    const query = request.query as Record<string, string | undefined>;
    const filePath = query.path?.trim() || query.file_path?.trim();
    if (!filePath)
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    try {
      const result = await files.filesRead({
        path: filePath,
        startLine: numberOr(query.start_line ?? query.offset, 1),
        endLine: query.end_line
          ? numberOr(query.end_line, undefined)
          : query.limit
            ? numberOr(query.offset ?? '1', 1) + numberOr(query.limit, 2000) - 1
            : undefined,
        maxBytes: numberOr(query.max_bytes, 32_768),
        encoding: query.encoding === 'base64' ? 'base64' : 'utf-8',
      });
      return { ok: true, backend_id: input.config.BACKEND_ID, ...result };
    } catch (error) {
      return runtimeError(reply, error, 404);
    }
  });

  app.get('/fs/stat', async (request, reply) => {
    const path = (request.query as { path?: string }).path?.trim();
    if (!path)
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    try {
      return { ok: true, backend_id: input.config.BACKEND_ID, ...(await files.filesStat(path)) };
    } catch (error) {
      return runtimeError(reply, error, 404);
    }
  });

  app.get('/fs/list', async (request, reply) => {
    const query = request.query as Record<string, string | undefined>;
    const path = query.path?.trim();
    if (!path)
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    try {
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        ...(await files.filesList({
          path,
          limit: numberOr(query.limit, 200),
          includeHidden: query.include_hidden === 'true',
        })),
      };
    } catch (error) {
      return runtimeError(reply, error, 404);
    }
  });

  app.post('/fs/glob', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.pattern !== 'string' || !body.pattern)
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'pattern is required.' } });
    try {
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        ...(await files.filesGlob({
          pattern: body.pattern,
          path: typeof body.path === 'string' ? body.path : undefined,
          includeHidden: Boolean(body.include_hidden),
          respectGitignore: body.respect_gitignore !== false,
          limit: typeof body.limit === 'number' ? body.limit : 200,
        })),
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/grep', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.pattern !== 'string' || !body.pattern)
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'pattern is required.' } });
    try {
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        ...(await files.filesGrep({
          pattern: body.pattern,
          path: typeof body.path === 'string' ? body.path : undefined,
          filePattern: typeof body.file_pattern === 'string' ? body.file_pattern : undefined,
          caseSensitive: body.case_sensitive === true,
          fixedString: body.fixed_string === true,
          contextBefore: typeof body.context_before === 'number' ? body.context_before : 0,
          contextAfter: typeof body.context_after === 'number' ? body.context_after : 0,
          maxMatches: typeof body.max_matches === 'number' ? body.max_matches : 100,
          maxBytes: typeof body.max_bytes === 'number' ? body.max_bytes : 64_000,
        })),
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/edit', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (
      typeof body.path !== 'string' ||
      typeof body.old_text !== 'string' ||
      typeof body.new_text !== 'string'
    )
      return reply.code(400).send({
        error: { code: 'validation_error', message: 'path, old_text, and new_text are required.' },
      });
    try {
      return await withFileIdempotency(
        idempotency,
        input.config.BACKEND_ID,
        'files.edit',
        body,
        async () => ({
          ok: true,
          backend_id: input.config.BACKEND_ID,
          ...(await files.filesEdit({
            path: body.path as string,
            oldText: body.old_text as string,
            newText: body.new_text as string,
            replaceAll: body.replace_all === true,
            expectedSha256:
              typeof body.expected_sha256 === 'string' ? body.expected_sha256 : undefined,
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/write', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.path !== 'string' || typeof body.content !== 'string')
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path and content are required.' } });
    if (
      body.mode !== 'create' &&
      body.mode !== 'overwrite' &&
      body.mode !== 'create_or_overwrite'
    ) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'mode is required: create | overwrite | create_or_overwrite.',
        },
      });
    }
    try {
      return await withFileIdempotency(
        idempotency,
        input.config.BACKEND_ID,
        'files.write',
        body,
        async () => ({
          ok: true,
          backend_id: input.config.BACKEND_ID,
          ...(await files.filesWrite({
            path: body.path as string,
            content: body.content as string,
            mode: body.mode as 'create' | 'overwrite' | 'create_or_overwrite',
            expectedSha256:
              typeof body.expected_sha256 === 'string' ? body.expected_sha256 : undefined,
            createParentDirectories: body.create_parent_directories !== false,
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/apply_patch', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.patch !== 'string' || !body.patch)
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'patch is required.' } });
    try {
      return await withFileIdempotency(
        idempotency,
        input.config.BACKEND_ID,
        'files.apply_patch',
        body,
        async () => ({
          ok: true,
          backend_id: input.config.BACKEND_ID,
          ...(await files.applyPatch({
            patch: body.patch as string,
            workspacePath:
              typeof body.workspace_path === 'string' ? body.workspace_path : undefined,
            dryRun: body.dry_run === true,
            atomic: body.atomic !== false,
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/move', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.from !== 'string' || typeof body.to !== 'string')
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'from and to are required.' } });
    try {
      return await withFileIdempotency(
        idempotency,
        input.config.BACKEND_ID,
        'files.move',
        body,
        async () => ({
          ok: true,
          backend_id: input.config.BACKEND_ID,
          ...(await files.filesMove({
            from: body.from as string,
            to: body.to as string,
            overwrite: body.overwrite === true,
            expectedSha256:
              typeof body.expected_sha256 === 'string' ? body.expected_sha256 : undefined,
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/delete', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.path !== 'string')
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    try {
      return await withFileIdempotency(
        idempotency,
        input.config.BACKEND_ID,
        'files.delete',
        body,
        async () => ({
          ok: true,
          backend_id: input.config.BACKEND_ID,
          ...(await files.filesDelete({
            path: body.path as string,
            recursive: body.recursive === true,
            expectedSha256:
              typeof body.expected_sha256 === 'string' ? body.expected_sha256 : undefined,
            expectedType:
              body.expected_type === 'file' || body.expected_type === 'directory'
                ? body.expected_type
                : undefined,
            dryRun: body.dry_run === true,
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/mkdir', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.path !== 'string')
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    try {
      return await withFileIdempotency(
        idempotency,
        input.config.BACKEND_ID,
        'files.mkdir',
        body,
        async () => ({
          ok: true,
          backend_id: input.config.BACKEND_ID,
          ...(await files.filesMkdir({
            path: body.path as string,
            recursive: body.recursive !== false,
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.get('/capabilities', async () => ({
    ok: true,
    backend_id: input.config.BACKEND_ID,
    ...(await files.detectCapabilities()),
  }));

  // ── Command / shell / process ──────────────────────────────────────
  app.post('/exec/command', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.program !== 'string' || !body.program)
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'program is required.' } });
    try {
      const result = await processes.exec({
        toolName: 'command.exec',
        program: body.program,
        arguments: Array.isArray(body.arguments) ? body.arguments.map(String) : undefined,
        workingDirectory:
          typeof body.working_directory === 'string' ? body.working_directory : undefined,
        environment:
          body.environment && typeof body.environment === 'object'
            ? (body.environment as Record<string, string>)
            : undefined,
        timeoutMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 120_000,
        yieldTimeMs: typeof body.yield_time_ms === 'number' ? body.yield_time_ms : 10_000,
        stdoutMaxBytes: typeof body.stdout_max_bytes === 'number' ? body.stdout_max_bytes : 16_384,
        stderrMaxBytes: typeof body.stderr_max_bytes === 'number' ? body.stderr_max_bytes : 16_384,
        idempotencyKey: typeof body.idempotency_key === 'string' ? body.idempotency_key : undefined,
        closeStdin: true,
      });
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/exec/shell', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.command !== 'string' || !body.command.trim())
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'command is required.' } });
    try {
      const result = await processes.exec({
        toolName: 'shell.exec',
        command: body.command,
        shell: body.shell === '/bin/sh' ? '/bin/sh' : '/bin/bash',
        workingDirectory:
          typeof body.working_directory === 'string' ? body.working_directory : undefined,
        environment:
          body.environment && typeof body.environment === 'object'
            ? (body.environment as Record<string, string>)
            : undefined,
        timeoutMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 120_000,
        yieldTimeMs: typeof body.yield_time_ms === 'number' ? body.yield_time_ms : 10_000,
        stdoutMaxBytes: typeof body.stdout_max_bytes === 'number' ? body.stdout_max_bytes : 16_384,
        stderrMaxBytes: typeof body.stderr_max_bytes === 'number' ? body.stderr_max_bytes : 16_384,
        idempotencyKey: typeof body.idempotency_key === 'string' ? body.idempotency_key : undefined,
        // Default true: shell.exec loads the real agent login environment.
        loadUserEnvironment: body.load_user_environment !== false,
        closeStdin: true,
      });
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  // Compatibility alias for older control planes.
  app.post('/exec/bash', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.command !== 'string' || !body.command.trim())
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'command is required.' } });
    try {
      const result = await processes.exec({
        toolName: 'shell.exec',
        command: body.command,
        workingDirectory: typeof body.cwd === 'string' ? body.cwd : undefined,
        timeoutMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 120_000,
        yieldTimeMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 120_000,
        stdoutMaxBytes: 30_000,
        stderrMaxBytes: 30_000,
        closeStdin: true,
      });
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        status: result.status,
        exit_code: result.exit_code,
        signal: result.signal,
        timed_out: result.timed_out,
        stdout_preview: result.stdout.preview,
        stderr_preview: result.stderr.preview,
        stdout_truncated: result.stdout.truncated,
        stderr_truncated: result.stderr.truncated,
        stdout_bytes: result.stdout.bytes,
        stderr_bytes: result.stderr.bytes,
        process_id: result.process_id,
        shell_id: null,
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/start', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    const hasProgram = typeof body.program === 'string' && body.program.length > 0;
    const hasCommand = typeof body.command === 'string' && body.command.length > 0;
    if (hasProgram === hasCommand) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'Provide exactly one of program or command.',
        },
      });
    }
    if (
      body.stdout_hard_max_bytes !== undefined &&
      (typeof body.stdout_hard_max_bytes !== 'number' ||
        body.stdout_hard_max_bytes < 0 ||
        body.stdout_hard_max_bytes > 1_073_741_824)
    ) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'stdout_hard_max_bytes must be 0..1073741824.',
        },
      });
    }
    if (
      body.stderr_hard_max_bytes !== undefined &&
      (typeof body.stderr_hard_max_bytes !== 'number' ||
        body.stderr_hard_max_bytes < 0 ||
        body.stderr_hard_max_bytes > 1_073_741_824)
    ) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'stderr_hard_max_bytes must be 0..1073741824.',
        },
      });
    }
    try {
      const result = await processes.exec({
        toolName: 'process.start',
        program: hasProgram ? String(body.program) : undefined,
        arguments: Array.isArray(body.arguments) ? body.arguments.map(String) : undefined,
        command: hasCommand ? String(body.command) : undefined,
        workingDirectory:
          typeof body.working_directory === 'string' ? body.working_directory : undefined,
        environment:
          body.environment && typeof body.environment === 'object'
            ? (body.environment as Record<string, string>)
            : undefined,
        timeoutMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 3_600_000,
        yieldTimeMs: 50,
        hardMaxStdout:
          typeof body.stdout_hard_max_bytes === 'number'
            ? body.stdout_hard_max_bytes
            : 100 * 1024 * 1024,
        hardMaxStderr:
          typeof body.stderr_hard_max_bytes === 'number'
            ? body.stderr_hard_max_bytes
            : 100 * 1024 * 1024,
        tty: body.tty === true,
        idempotencyKey: typeof body.idempotency_key === 'string' ? body.idempotency_key : undefined,
        closeStdin: body.tty === true ? false : true,
      });
      return {
        ok: true,
        process_id: result.process_id,
        status: result.status,
        stdin_available: result.stdin_available,
        tty: result.tty,
        output_cursor: '1:0',
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/read', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.process_id !== 'string')
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'process_id is required.' } });
    try {
      const result = await processes.readWait(body.process_id, {
        cursor: typeof body.cursor === 'string' ? body.cursor : undefined,
        maxBytes: typeof body.max_bytes === 'number' ? body.max_bytes : 65_536,
        waitMs: typeof body.wait_ms === 'number' ? body.wait_ms : 0,
      });
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/write', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.process_id !== 'string' || typeof body.data !== 'string')
      return reply.code(400).send({
        error: { code: 'validation_error', message: 'process_id and data are required.' },
      });
    try {
      return {
        ok: true,
        process_id: body.process_id,
        ...processes.write(body.process_id, body.data, body.close_stdin === true),
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/terminate', async (request, reply) => {
    const body = request.body as Record<string, unknown>;
    if (typeof body.process_id !== 'string')
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'process_id is required.' } });
    try {
      const signal =
        body.signal === 'sigint' || body.signal === 'sigkill' || body.signal === 'sigterm'
          ? body.signal
          : 'sigterm';
      const result = processes.terminate(
        body.process_id,
        signal,
        typeof body.grace_period_ms === 'number' ? body.grace_period_ms : 3_000,
      );
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/tasks/:id/cancel', async (request) =>
    input.queue.cancel((request.params as { id: string }).id),
  );
  app.post('/tasks/:id/retry', async (request, reply) => {
    try {
      await input.queue.retry((request.params as { id: string }).id);
      return reply.code(202).send({ status: 'queued' });
    } catch (error: unknown) {
      return reply.code(404).send({
        error: {
          code: 'not_found',
          message: error instanceof Error ? error.message : String(error),
        },
      });
    }
  });

  app.get('/schedulers', async () => input.queue.listSchedulers());
  app.put('/schedulers/:id', async (request, reply) => {
    const body = request.body as {
      cron?: unknown;
      timezone?: unknown;
      enabled?: unknown;
      taskTemplate?: unknown;
    };
    const template = createTaskSchema.safeParse(body.taskTemplate);
    if (
      !template.success ||
      typeof body.cron !== 'string' ||
      typeof body.timezone !== 'string' ||
      typeof body.enabled !== 'boolean'
    ) {
      return reply
        .code(400)
        .send({ error: { code: 'invalid_scheduler', message: 'Invalid scheduler payload.' } });
    }
    await input.queue.upsertScheduler({
      id: (request.params as { id: string }).id,
      cron: body.cron,
      timezone: body.timezone,
      enabled: body.enabled,
      taskTemplate: template.data,
    });
    return reply.code(204).send();
  });
  app.delete('/schedulers/:id', async (request, reply) => {
    await input.queue.upsertScheduler({
      id: (request.params as { id: string }).id,
      cron: '* * * * *',
      timezone: 'UTC',
      enabled: false,
      taskTemplate: createTaskSchema.parse({
        type: 'shell',
        backendId: input.config.BACKEND_ID,
        cwd: '/',
        timeoutSeconds: 1,
        profile: 'full',
        shell: { mode: 'exec', program: 'true', arguments: [] },
      }),
    });
    return reply.code(204).send();
  });
  app.post('/schedulers/:id/run', async (request) => {
    const body = request.body as { taskTemplate: unknown };
    const template = createTaskSchema.parse(body.taskTemplate);
    return {
      taskId: await input.queue.runScheduleNow({
        id: (request.params as { id: string }).id,
        taskTemplate: template,
      }),
    };
  });

  return app;
}

async function emptyPreview() {
  return { preview: '', totalBytes: 0, truncated: false };
}

async function previewFile(path: string, maxBytes: number) {
  try {
    const totalBytes = (await stat(path)).size;
    if (maxBytes <= 0) return { preview: '', totalBytes, truncated: totalBytes > 0 };
    const slice = await readFileSlice(path, 0, maxBytes);
    return {
      preview: slice.data,
      totalBytes,
      truncated: !slice.eof,
    };
  } catch {
    return emptyPreview();
  }
}

function pickStreamPath(
  commands: Array<{ stdoutPath?: string; stderrPath?: string }>,
  stream: 'stdout' | 'stderr',
): string | undefined {
  for (let index = commands.length - 1; index >= 0; index -= 1) {
    const command = commands[index];
    const path = stream === 'stdout' ? command?.stdoutPath : command?.stderrPath;
    if (path) return path;
  }
  return undefined;
}

async function readFileSlice(
  path: string,
  offset: number,
  maxBytes: number,
): Promise<{ data: string; nextOffset: number; eof: boolean; totalBytes: number }> {
  const handle = await open(path, 'r');
  try {
    const totalBytes = (await handle.stat()).size;
    if (offset >= totalBytes) return { data: '', nextOffset: totalBytes, eof: true, totalBytes };
    const length = Math.min(maxBytes, totalBytes - offset);
    const buffer = Buffer.alloc(length);
    const { bytesRead } = await handle.read(buffer, 0, length, offset);
    const data = buffer.subarray(0, bytesRead).toString('utf8');
    const nextOffset = offset + bytesRead;
    return { data, nextOffset, eof: nextOffset >= totalBytes, totalBytes };
  } finally {
    await handle.close();
  }
}

function numberOr(value: string | undefined, fallback: number): number;
function numberOr(value: string | undefined, fallback: undefined): number | undefined;
function numberOr(value: string | undefined, fallback: number | undefined): number | undefined {
  if (value === undefined) return fallback;
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return fallback;
  return parsed;
}

async function withFileIdempotency(
  store: IdempotencyStore,
  backendId: string,
  toolName: string,
  body: Record<string, unknown>,
  run: () => Promise<Record<string, unknown>>,
): Promise<Record<string, unknown>> {
  const key = typeof body.idempotency_key === 'string' ? body.idempotency_key : undefined;
  // Hash the mutating arguments (exclude nothing critical — full body minus nothing).
  const requestHash = hashRequest({
    tool_name: toolName,
    backend_id: backendId,
    arguments: body,
  });
  const cached = store.lookup(toolName, key, requestHash);
  if (cached && typeof cached === 'object') {
    return store.withIdempotencyMeta(key, requestHash, true, cached as Record<string, unknown>);
  }
  const result = await run();
  store.store(toolName, key, requestHash, result);
  return store.withIdempotencyMeta(key, requestHash, false, result);
}

async function probeAgentEnvironment(): Promise<{
  uid: number;
  gid: number;
  user: string;
  home: string;
  shell: string;
  home_accessible: boolean;
  home_writable: boolean;
  bashrc_readable: boolean;
  shell_smoke_ok: boolean;
  bashrc_path: string;
  notes: string[];
}> {
  const { access, constants } = await import('node:fs/promises');
  const { spawn } = await import('node:child_process');
  let user = process.env.USER || 'agent';
  let home = process.env.HOME || `/home/${user}`;
  try {
    const info = userInfo();
    if (typeof info.username === 'string') user = info.username;
    if (typeof info.homedir === 'string') home = info.homedir;
  } catch {
    /* keep defaults */
  }
  const bashrc = `${home}/.bashrc`;
  const notes: string[] = [];
  let home_accessible = false;
  let home_writable = false;
  let bashrc_readable = false;
  try {
    await access(home, constants.X_OK);
    home_accessible = true;
  } catch {
    notes.push(
      `HOME ${home} is not accessible (check /home mode and systemd ProtectHome/BindPaths).`,
    );
  }
  try {
    await access(home, constants.W_OK);
    home_writable = true;
  } catch {
    notes.push(`HOME ${home} is not writable by the agent user.`);
  }
  try {
    await access(bashrc, constants.R_OK);
    bashrc_readable = true;
  } catch {
    notes.push(`${bashrc} is not readable; shell login env may emit Permission denied.`);
  }

  const shell_smoke_ok = await new Promise<boolean>((resolve) => {
    const child = spawn('/bin/bash', ['-lc', 'test -n "$HOME" && test -x "$HOME" && id -un'], {
      env: {
        ...process.env,
        HOME: home,
        USER: user,
        LOGNAME: user,
        SHELL: '/bin/bash',
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      resolve(false);
    }, 3_000);
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => (stdout += chunk));
    child.stderr.on('data', (chunk) => (stderr += chunk));
    child.on('close', (code) => {
      clearTimeout(timer);
      if (stderr.includes('Permission denied')) {
        notes.push(`bash -lc reported: ${stderr.trim()}`);
        resolve(false);
        return;
      }
      resolve(code === 0 && stdout.trim().length > 0);
    });
    child.on('error', () => {
      clearTimeout(timer);
      resolve(false);
    });
  });

  return {
    uid: process.getuid?.() ?? -1,
    gid: process.getgid?.() ?? -1,
    user,
    home,
    shell: process.env.SHELL || '/bin/bash',
    home_accessible,
    home_writable,
    bashrc_readable,
    shell_smoke_ok,
    bashrc_path: bashrc,
    notes,
  };
}

function runtimeError(
  reply: { code: (status: number) => { send: (body: unknown) => unknown } },
  error: unknown,
  fallbackStatus = 400,
) {
  const err = error as {
    code?: string;
    message?: string;
    statusCode?: number;
    current_sha256?: string;
    match_count?: number;
  };
  const status = err.statusCode ?? fallbackStatus;
  return reply.code(status).send({
    error: {
      code: err.code ?? 'runtime_error',
      message: err.message ?? (error instanceof Error ? error.message : String(error)),
      ...(err.current_sha256 ? { current_sha256: err.current_sha256 } : {}),
      ...(err.match_count !== undefined ? { match_count: err.match_count } : {}),
    },
  });
}
