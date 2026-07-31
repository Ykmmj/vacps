import { createTaskSchema, taskDispatchSchema, isTerminalTaskStatus } from '@vacps/contracts';
import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';

import type { AgentConfig } from '../config';
import { parseSchedulePolicy } from '../queue/schedule-logic';
import type { TaskQueue } from '../queue/task-queue';
import type { ControlPlaneState } from '../registration/control-plane-state';
import { probeShellEnvironment } from '../runtime/shell-environment';
import * as files from '../runtime/files';
import { hashRequest, IdempotencyStore } from '../runtime/idempotency';
import type { ProcessManager } from '../runtime/process-manager';
import { allowUnsignedWhenNoKey, isPublicHttpPath } from '../security/http-auth';
import { verifyControlPlaneRequest } from '../security/control-plane-verify';
import type { NativeTelemetryCollector } from '../telemetry/native-telemetry';
import { createApp, type App, type Reply } from './router';

export interface CreateServerInput {
  config: AgentConfig;
  queue: TaskQueue;
  processes: ProcessManager;
  telemetry: NativeTelemetryCollector;
  getControlPlaneState: () => ControlPlaneState;
  isReady: () => boolean;
}

function numberOr(value: string | undefined, fallback: number): number {
  if (value === undefined) return fallback;
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return fallback;
  return parsed;
}

function errorStatus(error: unknown, fallback = 400): number {
  if (error && typeof error === 'object' && 'statusCode' in error) {
    const n = Number((error as { statusCode: unknown }).statusCode);
    if (Number.isInteger(n) && n >= 400 && n < 600) return n;
  }
  return fallback;
}

function errorBody(error: unknown) {
  const message = error instanceof Error ? error.message : String(error);
  const code =
    error && typeof error === 'object' && 'code' in error
      ? String((error as { code: unknown }).code)
      : 'runtime_error';
  return { error: { code, message } };
}

async function runtimeError(reply: Reply, error: unknown, fallback = 400) {
  return reply.code(errorStatus(error, fallback)).send(errorBody(error));
}

/**
 * Product HTTP routes — same registration style as apps/vacps/src/server/app.ts
 * (`app.get` / `app.post` / hooks), over vacps:http Host instead of Fastify.
 */
export async function createServer(input: CreateServerInput): Promise<App> {
  const app = createApp();
  const idempotency = new IdempotencyStore();

  // Only /health is unauthenticated. GET /tasks and /fs/* previously bypassed
  // auth and must not — loopback/Tunnel is not application-layer security.
  app.addHook('preValidation', async (request, reply) => {
    if (isPublicHttpPath(request.path)) return undefined;

    const pub = input.config.CONTROL_PLANE_PUBLIC_KEY;
    if (!pub) {
      if (allowUnsignedWhenNoKey(input.config)) return undefined;
      return reply.code(401).send({
        error: {
          code: 'unauthorized',
          message: 'CONTROL_PLANE_PUBLIC_KEY is required; unsigned requests are not accepted.',
        },
      });
    }

    try {
      const { nonce } = verifyControlPlaneRequest({
        publicKeyB64: pub,
        method: request.method,
        path: request.path,
        headers: request.headers,
        body: request.raw.body ?? '',
      });
      if (!input.queue.claimNonce(nonce)) {
        return reply.code(401).send({
          error: {
            code: 'replayed_request',
            message: 'A control-plane request may only be used once.',
          },
        });
      }
      return undefined;
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
    const status = await input.telemetry.collect();
    const shellEnv = await probeShellEnvironment();
    return {
      ...status.health,
      shell_environment: shellEnv,
      shell_environment_ok: shellEnv.home_accessible && shellEnv.shell_smoke_ok,
    };
  });

  app.get('/metrics', async () => {
    const status = await input.telemetry.collect();
    return status.metrics ?? {};
  });

  app.get('/ready', async (_request, reply) => {
    const ready = input.isReady();
    const state = input.getControlPlaneState();
    return reply.code(ready ? 200 : 503).send({
      ready,
      database: ready ? 'ok' : 'error',
      runtime: ready ? 'ok' : 'error',
      listener: ready ? 'ok' : 'error',
      registration: state.registrationStatus,
    });
  });

  app.get('/script/ping', async (request) => ({
    ok: true,
    service: 'vacps-script',
    hostVersion: host.version(),
    requestId: request.requestId,
  }));

  app.get('/status', async () => ({
    registration: input.getControlPlaneState(),
    controlPlaneConfigured: Boolean(input.config.CONTROL_PLANE_URL),
    ...(await input.telemetry.collect()),
  }));

  app.get('/info', async () => ({
    backendId: input.config.BACKEND_ID,
    runMode: 'api+worker',
    redis: false,
    pi: false,
    shell_environment: await probeShellEnvironment(),
  }));

  // ── Tasks ─────────────────────────────────────────────────────────
  app.post('/tasks', async (request, reply) => {
    if (!input.isReady()) {
      return reply.code(503).send({
        error: { code: 'service_unavailable', message: 'application not initialized' },
      });
    }

    const parsed = taskDispatchSchema.safeParse(request.body);
    if (!parsed.success) {
      return reply
        .code(400)
        .send({ error: { code: 'invalid_task', message: parsed.error.message } });
    }
    if (parsed.data.backend_id !== input.config.BACKEND_ID) {
      return reply
        .code(409)
        .send({ error: { code: 'backend_mismatch', message: 'Task targets another backend.' } });
    }
    if (parsed.data.kind === 'agent') {
      // Protocol kind from @vacps/contracts; native never runs Pi.
      return reply.code(409).send({
        error: {
          code: 'capability_unavailable',
          message: 'Pi runtime is not available on this backend.',
          details: { capability: 'pi' },
        },
      });
    }

    if (parsed.data.idempotency_key) {
      const prior = input.queue.findByIdempotencyKey(parsed.data.idempotency_key);
      if (prior) {
        return reply.code(202).send({
          task_id: prior.task.task_id,
          status: prior.status,
          backend_id: prior.task.backend_id,
          kind: prior.task.kind,
          deduped: true,
          idempotency_key: parsed.data.idempotency_key,
        });
      }
    }

    const { created } = input.queue.enqueue(parsed.data);
    const stored = input.queue.getTask(parsed.data.task_id);
    return reply.code(202).send({
      task_id: parsed.data.task_id,
      status: stored?.status ?? 'queued',
      backend_id: parsed.data.backend_id,
      kind: parsed.data.kind,
      ...(created ? {} : { deduped: true }),
    });
  });

  app.get('/tasks/:id', async (request, reply) => {
    const id = request.params.id ?? '';
    const task = input.queue.getTask(id);
    if (!task) {
      return reply.code(404).send({ error: { code: 'not_found', message: 'Task not found.' } });
    }
    const retentionSec = Math.max(
      60,
      Number(task.task.output?.retention_seconds ?? 86_400) || 86_400,
    );
    const terminalAt = task.finishedAt ? Date.parse(task.finishedAt) : NaN;
    const outputExpired =
      isTerminalTaskStatus(task.status) &&
      Number.isFinite(terminalAt) &&
      Date.now() - terminalAt > retentionSec * 1000;

    let result: unknown = task.result;
    if (outputExpired && result && typeof result === 'object') {
      const r = result as Record<string, unknown>;
      result = {
        kind: 'process',
        exit_code: r.exitCode ?? r.exit_code ?? null,
        signal: r.signal ?? null,
        timed_out: r.timedOut ?? r.timed_out ?? false,
        output_state: 'expired',
      };
    }

    return {
      task: task.task,
      status: task.status,
      result,
      error: task.error,
      createdAt: task.createdAt,
      startedAt: task.startedAt,
      finishedAt: task.finishedAt,
      ...(outputExpired ? { output_expired: true } : {}),
    };
  });

  app.get('/tasks/:id/logs', async (request, reply) => {
    const id = request.params.id ?? '';
    const task = input.queue.getTask(id);
    if (!task) {
      return reply.code(404).send({ error: { code: 'not_found', message: 'Task not found.' } });
    }

    // Output TTL: task.output.retention_seconds from terminal time (agent-local).
    const retentionSec = Math.max(
      60,
      Number(task.task.output?.retention_seconds ?? 86_400) || 86_400,
    );
    const terminalAt = task.finishedAt ? Date.parse(task.finishedAt) : NaN;
    const outputExpired =
      isTerminalTaskStatus(task.status) &&
      Number.isFinite(terminalAt) &&
      Date.now() - terminalAt > retentionSec * 1000;

    const offset = Math.max(0, Number(request.query.offset ?? '0') || 0);
    const stream = request.query.stream;
    const maxBytes = Math.min(
      Math.max(Number(request.query.max_bytes ?? '65536') || 65_536, 1),
      1_048_576,
    );
    const previewMax = Math.min(
      Math.max(Number(request.query.preview_max_bytes ?? '8192') || 8192, 0),
      65_536,
    );

    if (outputExpired) {
      if (stream === 'stdout' || stream === 'stderr') {
        return reply.code(410).send({
          error: {
            code: 'output_expired',
            message: 'Task output has expired per retention_seconds.',
          },
          task_id: id,
          stream,
          expired: true,
        });
      }
      return {
        task_id: id,
        taskId: id,
        expired: true,
        commands: [],
        logs: [],
      };
    }

    // Stream-style read: absolute **byte** offsets over concatenated stream text.
    if (stream === 'stdout' || stream === 'stderr') {
      const rows = input.queue.listLogs(id, { stream, offset: 0, limit: 50_000 });
      const full = rows.map((r) => r.data).join('');
      const totalBytes = full.length;
      const streamVersion = `sha256:${crypto.sha256Hex(full)}`;
      const expected =
        typeof request.query.expected_stream_version === 'string'
          ? request.query.expected_stream_version
          : undefined;
      if (expected && expected !== streamVersion) {
        return reply.code(409).send({
          error: {
            code: 'stream_version_conflict',
            message:
              'Stream version changed (log rotated, rebuilt, or replaced). Restart from offset 0 with the new stream_version.',
            current_stream_version: streamVersion,
            details: {
              expected_stream_version: expected,
              current_stream_version: streamVersion,
              restart_offset: 0,
            },
          },
        });
      }
      const start = Math.min(offset, totalBytes);
      const end = Math.min(start + maxBytes, totalBytes);
      const content = full.slice(start, end);
      const terminal = isTerminalTaskStatus(task.status);
      return {
        task_id: id,
        stream,
        offset: start,
        next_offset: end,
        eof: terminal && end >= totalBytes,
        total_bytes: totalBytes,
        truncated: false,
        expired: false,
        encoding: 'utf-8',
        content,
        stream_version: streamVersion,
        // Do not re-embed full log rows (would bypass max_bytes).
      };
    }

    const logs = input.queue.listLogs(id, { offset: 0, limit: 500 });

    // Control-plane tasks.get preview expects `commands[]` (Node shape), not raw log rows.
    const allStdout = input.queue
      .listLogs(id, { stream: 'stdout', offset: 0, limit: 2000 })
      .map((r) => r.data)
      .join('');
    const allStderr = input.queue
      .listLogs(id, { stream: 'stderr', offset: 0, limit: 2000 })
      .map((r) => r.data)
      .join('');
    const clip = (s: string) => (s.length > previewMax ? s.slice(0, previewMax) : s);
    const cmdStatus =
      task.status === 'succeeded'
        ? 'succeeded'
        : task.status === 'failed' || task.status === 'timed_out' || task.status === 'cancelled'
          ? 'failed'
          : task.status;
    const result = task.result as Record<string, unknown> | undefined;
    const commands = [
      {
        id: '1',
        sequence: 1,
        command:
          task.task.kind === 'command'
            ? [task.task.program, ...(task.task.arguments ?? [])].join(' ')
            : task.task.kind === 'shell'
              ? task.task.command
              : null,
        cwd: task.task.working_directory ?? null,
        status: cmdStatus,
        exitCode: result?.exitCode ?? result?.exit_code ?? null,
        exit_code: result?.exitCode ?? result?.exit_code ?? null,
        startedAt: task.startedAt ?? null,
        finishedAt: task.finishedAt ?? null,
        stdout: clip(allStdout),
        stderr: clip(allStderr),
        stdoutPreview: clip(allStdout),
        stderrPreview: clip(allStderr),
        stdout_bytes: allStdout.length,
        stderr_bytes: allStderr.length,
        stdoutBytes: allStdout.length,
        stderrBytes: allStderr.length,
        stdout_truncated: allStdout.length > previewMax,
        stderr_truncated: allStderr.length > previewMax,
        stdout_complete: isTerminalTaskStatus(task.status),
        stderr_complete: isTerminalTaskStatus(task.status),
      },
    ];

    return { task_id: id, taskId: id, commands, logs };
  });

  app.post('/tasks/:id/cancel', async (request, reply) => {
    const id = request.params.id ?? '';
    const result = input.queue.cancel(id);
    if (result.status === 'not_found') {
      return reply.code(404).send({ error: { code: 'not_found', message: 'Task not found.' } });
    }
    if (!result.cancelled) {
      return reply.code(409).send({
        error: { code: 'not_cancellable', message: 'Task already terminal.' },
        task_id: id,
        status: result.status,
        already_terminal: true,
      });
    }
    return { ok: true, task_id: id, ...result };
  });

  app.post('/tasks/:id/retry', async (request, reply) => {
    const id = request.params.id ?? '';
    try {
      const result = input.queue.retry(id);
      return reply.code(202).send({ ok: true, ...result });
    } catch (error: unknown) {
      const message = error instanceof Error ? error.message : String(error);
      if (message.includes('not found') || message.includes('Task not found')) {
        return reply.code(404).send({ error: { code: 'not_found', message } });
      }
      return reply.code(400).send({ error: { code: 'retry_failed', message } });
    }
  });

  // ── Files (vacps:fs) ──────────────────────────────────────────────
  app.get('/fs/read', async (request, reply) => {
    const filePath = request.query.path?.trim() || request.query.file_path?.trim();
    if (!filePath) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    }
    try {
      const startRaw = request.query.start_line ?? request.query.offset;
      const endRaw = request.query.end_line;
      const result = await files.filesRead({
        path: filePath,
        ...(startRaw !== undefined ? { startLine: numberOr(startRaw, 1) } : {}),
        ...(endRaw !== undefined ? { endLine: numberOr(endRaw, 1) } : {}),
        maxBytes: numberOr(request.query.max_bytes, 32_768),
        encoding: request.query.encoding === 'base64' ? 'base64' : 'utf-8',
      });
      return { ok: true, backend_id: input.config.BACKEND_ID, ...result };
    } catch (error) {
      return runtimeError(reply, error, 404);
    }
  });

  app.get('/fs/stat', async (request, reply) => {
    const path = request.query.path?.trim();
    if (!path) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    }
    try {
      return { ok: true, backend_id: input.config.BACKEND_ID, ...(await files.filesStat(path)) };
    } catch (error) {
      return runtimeError(reply, error, 404);
    }
  });

  app.get('/fs/list', async (request, reply) => {
    const path = request.query.path?.trim();
    if (!path) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    }
    try {
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        ...(await files.filesList({
          path,
          limit: numberOr(request.query.limit, 200),
          includeHidden: request.query.include_hidden === 'true',
          ...(typeof request.query.cursor === 'string' ? { cursor: request.query.cursor } : {}),
        })),
      };
    } catch (error) {
      return runtimeError(reply, error, 404);
    }
  });

  app.post('/fs/write', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.path !== 'string' || typeof body.content !== 'string') {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path and content are required.' } });
    }
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
            createParentDirectories: body.create_parent_directories !== false,
            ...(typeof body.expected_sha256 === 'string'
              ? { expectedSha256: body.expected_sha256 }
              : {}),
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/glob', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.pattern !== 'string' || !body.pattern) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'pattern is required.' } });
    }
    try {
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        ...(await files.filesGlob({
          pattern: body.pattern,
          ...(typeof body.path === 'string' ? { path: body.path } : {}),
          includeHidden: Boolean(body.include_hidden),
          limit: typeof body.limit === 'number' ? body.limit : 200,
          respectGitignore: body.respect_gitignore !== false,
          ...(typeof body.cursor === 'string' ? { cursor: body.cursor } : {}),
        })),
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/grep', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.pattern !== 'string' || !body.pattern) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'pattern is required.' } });
    }
    try {
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        ...(await files.filesGrep({
          pattern: body.pattern,
          ...(typeof body.path === 'string' ? { path: body.path } : {}),
          ...(typeof body.file_pattern === 'string' ? { filePattern: body.file_pattern } : {}),
          caseSensitive: body.case_sensitive === true,
          fixedString: body.fixed_string === true,
          contextBefore: typeof body.context_before === 'number' ? body.context_before : 0,
          contextAfter: typeof body.context_after === 'number' ? body.context_after : 0,
          maxMatches: typeof body.max_matches === 'number' ? body.max_matches : 100,
          maxBytes: typeof body.max_bytes === 'number' ? body.max_bytes : 64_000,
          ...(typeof body.cursor === 'string' ? { cursor: body.cursor } : {}),
        })),
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/edit', async (request, reply) => {
    const body = asRecord(request.body);
    if (
      typeof body.path !== 'string' ||
      typeof body.old_text !== 'string' ||
      typeof body.new_text !== 'string'
    ) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'path, old_text, and new_text are required.',
        },
      });
    }
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
            ...(typeof body.expected_sha256 === 'string'
              ? { expectedSha256: body.expected_sha256 }
              : {}),
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/apply_patch', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.patch !== 'string' || !body.patch) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'patch is required.' } });
    }
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
            ...(typeof body.workspace_path === 'string'
              ? { workspacePath: body.workspace_path }
              : {}),
            dryRun: body.dry_run === true,
            atomic: body.atomic !== false,
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

  app.post('/fs/mkdir', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.path !== 'string') {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    }
    try {
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        ...(await files.filesMkdir({
          path: body.path,
          recursive: body.recursive !== false,
        })),
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/delete', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.path !== 'string') {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'path is required.' } });
    }
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
            // Default recursive for directories when flag omitted is false — require explicit true.
            recursive: body.recursive === true,
            dryRun: body.dry_run === true,
            ...(typeof body.expected_sha256 === 'string'
              ? { expectedSha256: body.expected_sha256 }
              : {}),
            ...(body.expected_type === 'file' || body.expected_type === 'directory'
              ? { expectedType: body.expected_type }
              : {}),
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/fs/move', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.from !== 'string' || typeof body.to !== 'string') {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'from and to are required.' } });
    }
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
            ...(typeof body.expected_sha256 === 'string'
              ? { expectedSha256: body.expected_sha256 }
              : {}),
          })),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  // ── Command / shell ───────────────────────────────────────────────
  app.post('/exec/command', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.program !== 'string' || !body.program) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'program is required.' } });
    }
    try {
      const result = await input.processes.exec({
        toolName: 'command.exec',
        program: body.program,
        ...(Array.isArray(body.arguments) ? { arguments: body.arguments.map(String) } : {}),
        ...(typeof body.working_directory === 'string'
          ? { workingDirectory: body.working_directory }
          : {}),
        timeoutMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 120_000,
        stdoutMaxBytes: typeof body.stdout_max_bytes === 'number' ? body.stdout_max_bytes : 16_384,
        stderrMaxBytes: typeof body.stderr_max_bytes === 'number' ? body.stderr_max_bytes : 16_384,
        ...(typeof body.idempotency_key === 'string'
          ? { idempotencyKey: body.idempotency_key }
          : {}),
      });
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/exec/shell', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.command !== 'string' || !body.command.trim()) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'command is required.' } });
    }
    const shell = body.shell === '/bin/sh' ? '/bin/sh' : '/bin/bash';
    const loadUserEnvironment = shell === '/bin/sh' ? false : body.load_user_environment !== false;
    if (shell === '/bin/sh' && body.load_user_environment === true) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message:
            'load_user_environment=true is not supported with shell=/bin/sh; use /bin/bash or omit/false.',
        },
      });
    }
    try {
      const result = await input.processes.exec({
        toolName: 'shell.exec',
        command: body.command,
        shell,
        ...(typeof body.working_directory === 'string'
          ? { workingDirectory: body.working_directory }
          : {}),
        timeoutMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 120_000,
        stdoutMaxBytes: typeof body.stdout_max_bytes === 'number' ? body.stdout_max_bytes : 16_384,
        stderrMaxBytes: typeof body.stderr_max_bytes === 'number' ? body.stderr_max_bytes : 16_384,
        ...(typeof body.idempotency_key === 'string'
          ? { idempotencyKey: body.idempotency_key }
          : {}),
        loadUserEnvironment,
      });
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  // ── Long-lived process (start / read / write / terminate) ─────────
  app.post('/process/start_command', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.program !== 'string' || !body.program) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'program is required.' } });
    }
    try {
      const result = await input.processes.start({
        toolName: 'process.start_command',
        program: body.program,
        ...(Array.isArray(body.arguments) ? { arguments: body.arguments.map(String) } : {}),
        ...(typeof body.working_directory === 'string'
          ? { workingDirectory: body.working_directory }
          : {}),
        timeoutMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 3_600_000,
        closeStdin: body.tty === true ? false : body.close_stdin !== false,
        tty: body.tty === true,
        ...(typeof body.stdout_hard_max_bytes === 'number'
          ? { hardMaxStdout: body.stdout_hard_max_bytes }
          : {}),
        ...(typeof body.stderr_hard_max_bytes === 'number'
          ? { hardMaxStderr: body.stderr_hard_max_bytes }
          : {}),
      });
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/start_shell', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.command !== 'string' || !body.command.trim()) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'command is required.' } });
    }
    const shell = body.shell === '/bin/sh' ? '/bin/sh' : '/bin/bash';
    const loadUserEnvironment = shell === '/bin/sh' ? false : body.load_user_environment !== false;
    try {
      const result = await input.processes.start({
        toolName: 'process.start_shell',
        command: body.command,
        shell,
        ...(typeof body.working_directory === 'string'
          ? { workingDirectory: body.working_directory }
          : {}),
        timeoutMs: typeof body.timeout_ms === 'number' ? body.timeout_ms : 3_600_000,
        closeStdin: body.tty === true ? false : body.close_stdin !== false,
        tty: body.tty === true,
        loadUserEnvironment,
        ...(typeof body.stdout_hard_max_bytes === 'number'
          ? { hardMaxStdout: body.stdout_hard_max_bytes }
          : {}),
        ...(typeof body.stderr_hard_max_bytes === 'number'
          ? { hardMaxStderr: body.stderr_hard_max_bytes }
          : {}),
      });
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/read', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.process_id !== 'string') {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'process_id is required.' } });
    }
    try {
      const result = await input.processes.readWait(body.process_id, {
        ...(typeof body.cursor === 'string' ? { cursor: body.cursor } : {}),
        maxBytes: typeof body.max_bytes === 'number' ? body.max_bytes : 65_536,
        waitMs: typeof body.wait_ms === 'number' ? body.wait_ms : 0,
      });
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/write', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.process_id !== 'string' || typeof body.data !== 'string') {
      return reply.code(400).send({
        error: { code: 'validation_error', message: 'process_id and data are required.' },
      });
    }
    try {
      const result = await input.processes.write(
        body.process_id,
        body.data,
        body.close_stdin === true,
      );
      return { ok: true, process_id: body.process_id, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/terminate', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.process_id !== 'string') {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'process_id is required.' } });
    }
    try {
      const signal =
        body.signal === 'sigint' || body.signal === 'sigkill' || body.signal === 'sigterm'
          ? body.signal
          : 'sigterm';
      const result = await input.processes.terminate(
        body.process_id,
        signal,
        typeof body.grace_period_ms === 'number' ? body.grace_period_ms : 3_000,
      );
      return { ok: true, ...result };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  // ── Schedulers (SQLite; no Redis/BullMQ) ──────────────────────────
  app.get('/schedulers', async () => input.queue.listSchedulers());

  app.put('/schedulers/:id', async (request, reply) => {
    const id = request.params.id ?? '';
    const body = asRecord(request.body);
    if (body.taskTemplate !== undefined) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'taskTemplate is not accepted; use task (Schema v3 kind payload).',
        },
      });
    }
    const template = createTaskSchema.safeParse(body.task);
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
    const revision =
      typeof body.revision === 'number' && Number.isInteger(body.revision) && body.revision >= 1
        ? body.revision
        : undefined;
    const policy =
      body.policy && typeof body.policy === 'object' ? parseSchedulePolicy(body.policy) : undefined;
    input.queue.upsertScheduler({
      id,
      cron: body.cron,
      timezone: body.timezone,
      enabled: body.enabled,
      task: template.data,
      ...(revision !== undefined ? { revision } : {}),
      ...(policy ? { policy } : {}),
      ...(typeof body.next_run_at === 'string'
        ? { nextRunAt: body.next_run_at }
        : body.next_run_at === null
          ? { nextRunAt: null }
          : {}),
    });
    return reply.code(204).send();
  });

  app.delete('/schedulers/:id', async (request, reply) => {
    const id = request.params.id ?? '';
    try {
      input.queue.deleteScheduler(id);
    } catch {
      /* best-effort */
    }
    return reply.code(204).send();
  });

  app.post('/schedulers/:id/run', async (request, reply) => {
    const id = request.params.id ?? '';
    const body = asRecord(request.body);
    if (body.taskTemplate !== undefined) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'taskTemplate is not accepted; use task (Schema v3 kind payload).',
        },
      });
    }
    const template = createTaskSchema.safeParse(body.task);
    if (!template.success) {
      return reply
        .code(400)
        .send({ error: { code: 'invalid_task', message: template.error.message } });
    }
    const taskId = input.queue.runScheduleNow({ id, task: template.data });
    return { task_id: taskId };
  });

  return app;
}

function asRecord(body: unknown): Record<string, unknown> {
  if (body && typeof body === 'object' && !Array.isArray(body)) {
    return body as Record<string, unknown>;
  }
  return {};
}

async function withFileIdempotency(
  store: IdempotencyStore,
  backendId: string,
  toolName: string,
  body: Record<string, unknown>,
  run: () => Promise<Record<string, unknown>>,
): Promise<Record<string, unknown>> {
  const key = typeof body.idempotency_key === 'string' ? body.idempotency_key : undefined;
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
