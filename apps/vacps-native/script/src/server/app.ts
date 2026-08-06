import {
  createTaskSchema,
  schedulePolicySchema,
  taskDispatchSchema,
  isTerminalTaskStatus,
  type BackendHealth,
  type CreateTaskInput,
  type SchedulePolicy,
  type TaskDispatch,
} from '@vacps/contracts';
import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';

import type { AgentConfig } from '../config';
import type { TaskQueue } from '../queue/task-queue';
import type { ControlPlaneState } from '../registration/control-plane-state';
import { probeShellEnvironment } from '../runtime/shell-environment';
import * as files from '../runtime/files';
import { hashRequest, IdempotencyStore } from '../runtime/idempotency';
import {
  NATIVE_STREAM_MAX_BYTES,
  PROCESS_READ_MAX_BYTES,
  type ProcessSessions,
} from '../runtime/process-sessions';
import { allowUnsignedWhenNoKey, isPublicHttpPath } from '../security/http-auth';
import { verifyControlPlaneRequest } from '../security/control-plane-verify';
import type { LiveHealthState } from '../telemetry/liveness-health';
import type { NativeTelemetryCollector } from '../telemetry/native-telemetry';
import { utf8ByteLengthOfString, utf8ByteSlice } from '../util/utf8';
import { createApp, type App, type Reply } from './router';

export interface CreateServerInput {
  config: AgentConfig;
  queue: TaskQueue;
  telemetry: NativeTelemetryCollector;
  processes: ProcessSessions;
  getControlPlaneState: () => ControlPlaneState;
  isReady: () => boolean;
  /** Cheap public /health body (no telemetry/shell probes). */
  getLivenessHealth: () => BackendHealth;
  /** Live ok/worker flags for authenticated status/telemetry. */
  getLiveHealthState: () => LiveHealthState;
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
 * (`app.get` / `app.post` / hooks), over the script router (not Fastify).
 * Invoked from Application's Server onRequest (native event → JS callback).
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
        expectedBackendId: input.config.BACKEND_ID,
        method: request.method,
        // Sign/verify the raw pre-router path; request.path drops a trailing slash.
        path: request.raw.path,
        query: request.raw.query,
        headers: request.headers,
        body: request.raw.body ?? '',
      });
      if (!(await input.queue.claimNonce(nonce))) {
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

  // Public liveness only: ready/loop state + cheap host fields. No df/uname/bash/telemetry.
  app.get('/health', async () => input.getLivenessHealth());

  app.get('/metrics', async () => {
    const status = await input.telemetry.collect(input.getLiveHealthState());
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
    ...(await input.telemetry.collect(input.getLiveHealthState())),
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

    const capability = nativeTaskCapabilityRejection(parsed.data);
    if (capability) {
      return reply.code(capability.status).send(capability.body);
    }

    if (parsed.data.idempotency_key) {
      const prior = await input.queue.findByIdempotencyKey(parsed.data.idempotency_key);
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

    const { created } = await input.queue.enqueue(parsed.data);
    const stored = await input.queue.getTask(parsed.data.task_id);
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
    const task = await input.queue.getTask(id);
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
      host.nowMs() - terminalAt > retentionSec * 1000;

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
    const task = await input.queue.getTask(id);
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
      host.nowMs() - terminalAt > retentionSec * 1000;

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

    // Stream-style read: absolute **UTF-8 byte** offsets over concatenated retained text.
    // EOF = end of retained content; total/truncated report native drain facts when known.
    if (stream === 'stdout' || stream === 'stderr') {
      const rows = await input.queue.listLogs(id, { stream, offset: 0, limit: 50_000 });
      const full = rows.map((r) => r.data).join('');
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
      const slice = utf8ByteSlice(full, offset, offset + maxBytes);
      const terminal = isTerminalTaskStatus(task.status);
      const meta = processStreamMeta(task.result, stream, slice.totalBytes);
      return {
        task_id: id,
        stream,
        offset: slice.start,
        next_offset: slice.end,
        eof: terminal && slice.end >= slice.totalBytes,
        total_bytes: meta.totalBytes,
        truncated: meta.nativeTruncated,
        expired: false,
        encoding: 'utf-8',
        content: slice.content,
        stream_version: streamVersion,
        // Do not re-embed full log rows (would bypass max_bytes).
      };
    }

    const logs = await input.queue.listLogs(id, { offset: 0, limit: 500 });

    // Control-plane tasks.get preview expects `commands[]` (Node shape), not raw log rows.
    const allStdout = (await input.queue.listLogs(id, { stream: 'stdout', offset: 0, limit: 2000 }))
      .map((r) => r.data)
      .join('');
    const allStderr = (await input.queue.listLogs(id, { stream: 'stderr', offset: 0, limit: 2000 }))
      .map((r) => r.data)
      .join('');
    const stdoutPreview = utf8ByteSlice(allStdout, 0, previewMax);
    const stderrPreview = utf8ByteSlice(allStderr, 0, previewMax);
    const stdoutMeta = processStreamMeta(task.result, 'stdout', stdoutPreview.totalBytes);
    const stderrMeta = processStreamMeta(task.result, 'stderr', stderrPreview.totalBytes);
    const stdoutClipped = previewMax > 0 && stdoutPreview.end < stdoutPreview.totalBytes;
    const stderrClipped = previewMax > 0 && stderrPreview.end < stderrPreview.totalBytes;
    // previewMax <= 0 → empty preview; treat any retained/native bytes as clipped.
    const stdoutPreviewEmptyClip = previewMax <= 0 && stdoutMeta.totalBytes > 0;
    const stderrPreviewEmptyClip = previewMax <= 0 && stderrMeta.totalBytes > 0;
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
        stdout: stdoutPreview.content,
        stderr: stderrPreview.content,
        stdoutPreview: stdoutPreview.content,
        stderrPreview: stderrPreview.content,
        stdout_bytes: stdoutMeta.totalBytes,
        stderr_bytes: stderrMeta.totalBytes,
        stdoutBytes: stdoutMeta.totalBytes,
        stderrBytes: stderrMeta.totalBytes,
        stdout_truncated: stdoutMeta.nativeTruncated || stdoutClipped || stdoutPreviewEmptyClip,
        stderr_truncated: stderrMeta.nativeTruncated || stderrClipped || stderrPreviewEmptyClip,
        stdout_complete: isTerminalTaskStatus(task.status),
        stderr_complete: isTerminalTaskStatus(task.status),
      },
    ];

    return { task_id: id, taskId: id, commands, logs };
  });

  app.post('/tasks/:id/cancel', async (request, reply) => {
    const id = request.params.id ?? '';
    const result = await input.queue.cancel(id);
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
      const result = await input.queue.retry(id);
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
      return await withIdempotency(
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
      return await withIdempotency(
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
      return await withIdempotency(
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
      return await withIdempotency(
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
      return await withIdempotency(
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

  // ── Command / shell / interactive process sessions ───────────────
  app.post('/exec/command', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.program !== 'string' || !body.program) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'program is required.' } });
    }
    const unsupported = rejectUnsupportedExecFields(body);
    if (unsupported) {
      return reply.code(unsupported.status).send(unsupported.body);
    }
    if (body.working_directory !== undefined && typeof body.working_directory !== 'string') {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'working_directory must be a string when present.',
        },
      });
    }
    let args: string[] | undefined;
    if (body.arguments !== undefined) {
      if (!isStringArray(body.arguments)) {
        return reply.code(400).send({
          error: {
            code: 'validation_error',
            message: 'arguments must be an array of strings when present.',
          },
        });
      }
      args = body.arguments;
    }
    let numeric: ReturnType<typeof parseExecNumericOptions>;
    try {
      numeric = parseExecNumericOptions(body);
    } catch (error) {
      return runtimeError(reply, error);
    }
    try {
      return await withProcessIdempotency(
        idempotency,
        input.processes,
        input.config.BACKEND_ID,
        'command.exec',
        body,
        canonicalProcessArguments(body, ['yield_time_ms', 'stdout_max_bytes', 'stderr_max_bytes']),
        {
          stdoutMaxBytes: numeric.stdoutMaxBytes,
          stderrMaxBytes: numeric.stderrMaxBytes,
        },
        async () => ({
          ok: true,
          ...(await input.processes.exec(
            {
              kind: 'command',
              program: body.program as string,
              ...(args ? { arguments: args } : {}),
              ...(typeof body.working_directory === 'string'
                ? { workingDirectory: body.working_directory }
                : {}),
              timeoutMs: numeric.timeoutMs,
              stdoutHardMaxBytes: 16 * 1024 * 1024,
              stderrHardMaxBytes: 16 * 1024 * 1024,
              stdin: 'ignore',
            },
            {
              stdoutMaxBytes: numeric.stdoutMaxBytes,
              stderrMaxBytes: numeric.stderrMaxBytes,
            },
            numeric.yieldMs,
          )),
        }),
      );
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
    const unsupported = rejectUnsupportedExecFields(body);
    if (unsupported) {
      return reply.code(unsupported.status).send(unsupported.body);
    }
    if (body.working_directory !== undefined && typeof body.working_directory !== 'string') {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'working_directory must be a string when present.',
        },
      });
    }
    let shell: '/bin/bash' | '/bin/sh' = '/bin/bash';
    if (body.shell !== undefined) {
      if (body.shell !== '/bin/bash' && body.shell !== '/bin/sh') {
        return reply.code(400).send({
          error: {
            code: 'validation_error',
            message: 'shell must be exactly /bin/bash or /bin/sh when present.',
          },
        });
      }
      shell = body.shell;
    }
    if (
      body.load_user_environment !== undefined &&
      typeof body.load_user_environment !== 'boolean'
    ) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'load_user_environment must be a boolean when present.',
        },
      });
    }
    // At the direct /exec boundary: /bin/sh + omitted → false; /bin/sh + true → 400.
    if (shell === '/bin/sh' && body.load_user_environment === true) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message:
            'load_user_environment=true is not supported with shell=/bin/sh; use /bin/bash or omit/false.',
        },
      });
    }
    const loadUserEnvironment = shell === '/bin/sh' ? false : body.load_user_environment !== false;
    let numeric: ReturnType<typeof parseExecNumericOptions>;
    try {
      numeric = parseExecNumericOptions(body);
    } catch (error) {
      return runtimeError(reply, error);
    }
    try {
      return await withProcessIdempotency(
        idempotency,
        input.processes,
        input.config.BACKEND_ID,
        'shell.exec',
        body,
        canonicalProcessArguments(body, ['yield_time_ms', 'stdout_max_bytes', 'stderr_max_bytes']),
        {
          stdoutMaxBytes: numeric.stdoutMaxBytes,
          stderrMaxBytes: numeric.stderrMaxBytes,
        },
        async () => ({
          ok: true,
          ...(await input.processes.exec(
            {
              kind: 'shell',
              command: body.command as string,
              shell,
              loadUserEnvironment,
              ...(typeof body.working_directory === 'string'
                ? { workingDirectory: body.working_directory }
                : {}),
              timeoutMs: numeric.timeoutMs,
              stdoutHardMaxBytes: 16 * 1024 * 1024,
              stderrHardMaxBytes: 16 * 1024 * 1024,
              stdin: 'ignore',
            },
            {
              stdoutMaxBytes: numeric.stdoutMaxBytes,
              stderrMaxBytes: numeric.stderrMaxBytes,
            },
            numeric.yieldMs,
          )),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/start_command', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.program !== 'string' || body.program.length === 0) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'program is required.' } });
    }
    const unsupported = rejectUnsupportedProcessStartFields(body);
    if (unsupported) return reply.code(unsupported.status).send(unsupported.body);
    if (body.working_directory !== undefined && typeof body.working_directory !== 'string') {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'working_directory must be a string when present.',
        },
      });
    }
    let args: string[] | undefined;
    if (body.arguments !== undefined) {
      if (!isStringArray(body.arguments)) {
        return reply.code(400).send({
          error: {
            code: 'validation_error',
            message: 'arguments must be an array of strings when present.',
          },
        });
      }
      args = body.arguments;
    }
    try {
      const options = parseProcessStartOptions(body);
      return await withProcessIdempotency(
        idempotency,
        input.processes,
        input.config.BACKEND_ID,
        'process.start_command',
        body,
        canonicalProcessArguments(body),
        { stdoutMaxBytes: 16_384, stderrMaxBytes: 16_384 },
        async () => ({
          ok: true,
          ...(await input.processes.start(
            {
              kind: 'command',
              program: body.program as string,
              ...(args ? { arguments: args } : {}),
              ...(typeof body.working_directory === 'string'
                ? { workingDirectory: body.working_directory }
                : {}),
              timeoutMs: options.timeoutMs,
              stdoutHardMaxBytes: options.stdoutHardMaxBytes,
              stderrHardMaxBytes: options.stderrHardMaxBytes,
              stdin: 'pipe',
            },
            { stdoutMaxBytes: 16_384, stderrMaxBytes: 16_384 },
          )),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/start_shell', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.command !== 'string' || body.command.trim().length === 0) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'command is required.' } });
    }
    const unsupported = rejectUnsupportedProcessStartFields(body);
    if (unsupported) return reply.code(unsupported.status).send(unsupported.body);
    if (body.working_directory !== undefined && typeof body.working_directory !== 'string') {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'working_directory must be a string when present.',
        },
      });
    }
    let shell: '/bin/bash' | '/bin/sh' = '/bin/bash';
    if (body.shell !== undefined) {
      if (body.shell !== '/bin/bash' && body.shell !== '/bin/sh') {
        return reply.code(400).send({
          error: {
            code: 'validation_error',
            message: 'shell must be exactly /bin/bash or /bin/sh when present.',
          },
        });
      }
      shell = body.shell;
    }
    if (
      body.load_user_environment !== undefined &&
      typeof body.load_user_environment !== 'boolean'
    ) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'load_user_environment must be a boolean when present.',
        },
      });
    }
    if (shell === '/bin/sh' && body.load_user_environment === true) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message:
            'load_user_environment=true is not supported with shell=/bin/sh; use /bin/bash or omit/false.',
        },
      });
    }
    const loadUserEnvironment = shell === '/bin/sh' ? false : body.load_user_environment !== false;
    try {
      const options = parseProcessStartOptions(body);
      return await withProcessIdempotency(
        idempotency,
        input.processes,
        input.config.BACKEND_ID,
        'process.start_shell',
        body,
        canonicalProcessArguments(body),
        { stdoutMaxBytes: 16_384, stderrMaxBytes: 16_384 },
        async () => ({
          ok: true,
          ...(await input.processes.start(
            {
              kind: 'shell',
              command: body.command as string,
              shell,
              loadUserEnvironment,
              ...(typeof body.working_directory === 'string'
                ? { workingDirectory: body.working_directory }
                : {}),
              timeoutMs: options.timeoutMs,
              stdoutHardMaxBytes: options.stdoutHardMaxBytes,
              stderrHardMaxBytes: options.stderrHardMaxBytes,
              stdin: 'pipe',
            },
            { stdoutMaxBytes: 16_384, stderrMaxBytes: 16_384 },
          )),
        }),
      );
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/read', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.process_id !== 'string' || body.process_id.length === 0) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'process_id is required.' } });
    }
    if (body.cursor !== undefined && typeof body.cursor !== 'string') {
      return reply.code(400).send({
        error: { code: 'validation_error', message: 'cursor must be a string when present.' },
      });
    }
    try {
      const maxBytes = readOptionalIntField(body, 'max_bytes', 65_536, 1, PROCESS_READ_MAX_BYTES);
      const waitMs = readOptionalIntField(body, 'wait_ms', 0, 0, 60_000);
      return {
        ok: true,
        ...(await input.processes.read(
          body.process_id,
          typeof body.cursor === 'string' ? body.cursor : undefined,
          maxBytes,
          waitMs,
        )),
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/write', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.process_id !== 'string' || body.process_id.length === 0) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'process_id is required.' } });
    }
    if (typeof body.data !== 'string') {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'data must be a string.' } });
    }
    if (utf8ByteLengthOfString(body.data) > PROCESS_READ_MAX_BYTES) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: `data must be at most ${PROCESS_READ_MAX_BYTES} UTF-8 bytes.`,
        },
      });
    }
    if (body.close_stdin !== undefined && typeof body.close_stdin !== 'boolean') {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'close_stdin must be a boolean when present.',
        },
      });
    }
    try {
      const written = await input.processes.write(
        body.process_id,
        body.data,
        body.close_stdin === true,
      );
      return { ok: true, process_id: body.process_id, written_bytes: written };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  app.post('/process/terminate', async (request, reply) => {
    const body = asRecord(request.body);
    if (typeof body.process_id !== 'string' || body.process_id.length === 0) {
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'process_id is required.' } });
    }
    if (
      body.signal !== undefined &&
      body.signal !== 'sigterm' &&
      body.signal !== 'sigint' &&
      body.signal !== 'sigkill'
    ) {
      return reply.code(400).send({
        error: {
          code: 'validation_error',
          message: 'signal must be sigterm, sigint, or sigkill.',
        },
      });
    }
    try {
      const gracePeriodMs = readOptionalIntField(body, 'grace_period_ms', 3_000, 0, 60_000);
      return {
        ok: true,
        ...(await input.processes.terminate(
          body.process_id,
          body.signal === 'sigint' || body.signal === 'sigkill' ? body.signal : 'sigterm',
          gracePeriodMs,
          { stdoutMaxBytes: 16_384, stderrMaxBytes: 16_384 },
        )),
      };
    } catch (error) {
      return runtimeError(reply, error);
    }
  });

  // ── Schedulers (SQLite; no Redis/BullMQ) ──────────────────────────
  app.get('/schedulers', async () => await input.queue.listSchedulers());

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

    const capability = nativeTaskCapabilityRejection(template.data);
    if (capability) {
      return reply.code(capability.status).send(capability.body);
    }

    const revision =
      typeof body.revision === 'number' && Number.isInteger(body.revision) && body.revision >= 1
        ? body.revision
        : undefined;

    // Wide boundary: validate policy once when provided; omit keeps current PUT merge behavior.
    let policy: SchedulePolicy | undefined;
    if (body.policy !== undefined) {
      const parsedPolicy = schedulePolicySchema.safeParse(body.policy);
      if (!parsedPolicy.success) {
        return reply.code(400).send({
          error: {
            code: 'invalid_scheduler',
            message: 'Invalid scheduler policy.',
            details: { field: 'policy', issues: parsedPolicy.error.issues },
          },
        });
      }
      const concurrency = parsedPolicy.data.concurrency;
      if (concurrency === 'allow' || concurrency === 'replace') {
        return reply.code(409).send({
          error: {
            code: 'capability_unavailable',
            message: `Schedule concurrency "${concurrency}" is not supported on this backend.`,
            details: { capability: 'schedule.concurrency', concurrency },
          },
        });
      }
      if (concurrency === 'forbid' && parsedPolicy.data.misfire === 'catch_up') {
        return reply.code(409).send({
          error: {
            code: 'capability_unavailable',
            message:
              'Schedule concurrency "forbid" with misfire "catch_up" is not supported on this backend.',
            details: {
              capability: 'schedule.concurrency',
              concurrency,
              misfire: parsedPolicy.data.misfire,
            },
          },
        });
      }
      policy = parsedPolicy.data;
    }

    await input.queue.upsertScheduler({
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
      await input.queue.deleteScheduler(id);
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

    const capability = nativeTaskCapabilityRejection(template.data);
    if (capability) {
      return reply.code(capability.status).send(capability.body);
    }

    const taskId = await input.queue.runScheduleNow({ id, task: template.data });
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

/**
 * Exact native drain totals + capture truncation from stored canonical result metadata.
 * In-progress tasks have no result yet — fall back to retained UTF-8 byte length only.
 * This is not backward compatibility for alternate field shapes.
 */
function processStreamMeta(
  result: unknown,
  stream: 'stdout' | 'stderr',
  retainedUtf8Bytes: number,
): { totalBytes: number; nativeTruncated: boolean } {
  if (!result || typeof result !== 'object') {
    return { totalBytes: retainedUtf8Bytes, nativeTruncated: false };
  }
  const r = result as Record<string, unknown>;
  const bytesKey = stream === 'stdout' ? 'stdoutBytes' : 'stderrBytes';
  const truncKey = stream === 'stdout' ? 'stdoutTruncated' : 'stderrTruncated';
  const rawBytes = r[bytesKey];
  const totalBytes =
    typeof rawBytes === 'number' && Number.isFinite(rawBytes) && rawBytes >= 0
      ? rawBytes
      : retainedUtf8Bytes;
  const nativeTruncated = r[truncKey] === true;
  return { totalBytes, nativeTruncated };
}

/** Shared in-memory idempotency for mutating file tools and process creation. */
async function withIdempotency(
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
  const { result, replayed } = await store.execute(toolName, key, requestHash, run);
  return store.withIdempotencyMeta(key, requestHash, replayed, result);
}

/**
 * Process idempotency stores only the initial protocol snapshot. Replays are
 * refreshed from the still-owned JS Process session so status/output never go
 * stale. Observation-only yield/preview fields are excluded by the caller from
 * the work identity.
 */
async function withProcessIdempotency(
  store: IdempotencyStore,
  processes: ProcessSessions,
  backendId: string,
  toolName: string,
  body: Record<string, unknown>,
  workArguments: Record<string, unknown>,
  preview: { stdoutMaxBytes: number; stderrMaxBytes: number },
  run: () => Promise<Record<string, unknown>>,
): Promise<Record<string, unknown>> {
  const key = typeof body.idempotency_key === 'string' ? body.idempotency_key : undefined;
  const requestHash = hashRequest({
    tool_name: toolName,
    backend_id: backendId,
    arguments: workArguments,
  });
  const { result: initial, replayed } = await store.execute(toolName, key, requestHash, run);
  const processId = initial.process_id;
  if (typeof processId !== 'string') {
    throw new Error(`${toolName} did not return a process_id.`);
  }
  if (key !== undefined && !replayed) {
    processes.retainForIdempotencyReplay(processId);
  }
  const result = replayed ? { ok: true, ...processes.snapshotById(processId, preview) } : initial;
  return store.withIdempotencyMeta(key, requestHash, replayed, result);
}

function canonicalProcessArguments(
  body: Record<string, unknown>,
  observationFields: readonly string[] = [],
): Record<string, unknown> {
  const excluded = new Set(['backend_id', 'idempotency_key', ...observationFields]);
  return Object.fromEntries(Object.entries(body).filter(([key]) => !excluded.has(key)));
}

function isStringArray(value: unknown): value is string[] {
  return Array.isArray(value) && value.every((item) => typeof item === 'string');
}

/** Reject native-unsupported /exec fields before side effects. */
function rejectUnsupportedExecFields(
  body: Record<string, unknown>,
): { status: number; body: Record<string, unknown> } | null {
  if (body.environment !== undefined) {
    return {
      status: 409,
      body: {
        error: {
          code: 'capability_unavailable',
          message: 'Process environment injection is not supported on this backend.',
          details: { capability: 'environment' },
        },
      },
    };
  }
  // Shared command/shell protocol does not define hard-max capture fields.
  for (const field of ['stdout_hard_max_bytes', 'stderr_hard_max_bytes'] as const) {
    if (body[field] !== undefined) {
      return {
        status: 400,
        body: {
          error: {
            code: 'validation_error',
            message: `${field} is not supported on /exec; native exec sessions use a fixed internal cap.`,
            details: { field },
          },
        },
      };
    }
  }
  return null;
}

function rejectUnsupportedProcessStartFields(
  body: Record<string, unknown>,
): { status: number; body: Record<string, unknown> } | null {
  if (body.environment !== undefined) {
    return {
      status: 409,
      body: {
        error: {
          code: 'capability_unavailable',
          message: 'Process environment injection is not supported on this backend.',
          details: { capability: 'environment' },
        },
      },
    };
  }
  if (body.tty !== undefined && typeof body.tty !== 'boolean') {
    return {
      status: 400,
      body: {
        error: {
          code: 'validation_error',
          message: 'tty must be a boolean when present.',
        },
      },
    };
  }
  if (body.tty === true) {
    return {
      status: 409,
      body: {
        error: {
          code: 'capability_unavailable',
          message: 'PTY/TTY processes are not supported on this backend.',
          details: { capability: 'tty' },
        },
      },
    };
  }
  return null;
}

function parseProcessStartOptions(body: Record<string, unknown>): {
  timeoutMs: number;
  stdoutHardMaxBytes: number;
  stderrHardMaxBytes: number;
} {
  return {
    timeoutMs: readOptionalIntField(body, 'timeout_ms', 3_600_000, 1, 3_600_000),
    stdoutHardMaxBytes: readOptionalIntField(
      body,
      'stdout_hard_max_bytes',
      NATIVE_STREAM_MAX_BYTES,
      0,
      NATIVE_STREAM_MAX_BYTES,
    ),
    stderrHardMaxBytes: readOptionalIntField(
      body,
      'stderr_hard_max_bytes',
      NATIVE_STREAM_MAX_BYTES,
      0,
      NATIVE_STREAM_MAX_BYTES,
    ),
  };
}

/**
 * Wide-boundary optional integer fields for /exec.
 * Defaults applied only when absent; invalid values throw validation_error (no clamp).
 */
function parseExecNumericOptions(body: Record<string, unknown>): {
  timeoutMs: number;
  yieldMs: number | undefined;
  stdoutMaxBytes: number;
  stderrMaxBytes: number;
} {
  return {
    timeoutMs: readOptionalIntField(body, 'timeout_ms', 120_000, 1, 3_600_000),
    yieldMs: readOptionalIntFieldWhenPresent(body, 'yield_time_ms', 1, 120_000),
    stdoutMaxBytes: readOptionalIntField(body, 'stdout_max_bytes', 16_384, 0, 1_048_576),
    stderrMaxBytes: readOptionalIntField(body, 'stderr_max_bytes', 16_384, 0, 1_048_576),
  };
}

function readOptionalIntFieldWhenPresent(
  body: Record<string, unknown>,
  field: string,
  min: number,
  max: number,
): number | undefined {
  if (body[field] === undefined) return undefined;
  return readOptionalIntField(body, field, min, min, max);
}

function readOptionalIntField(
  body: Record<string, unknown>,
  field: string,
  defaultValue: number,
  min: number,
  max: number,
): number {
  const value = body[field];
  if (value === undefined) return defaultValue;
  if (typeof value !== 'number' || !Number.isInteger(value) || value < min || value > max) {
    const error = new Error(`${field} must be an integer in range ${min}..${max}.`) as Error & {
      code: string;
      statusCode: number;
    };
    error.code = 'validation_error';
    error.statusCode = 400;
    throw error;
  }
  return value;
}

/**
 * Native capability gate after shared Zod parse, before enqueue/persist/side effects.
 * Valid shared-contract features this backend does not implement → 409 capability_unavailable.
 * Invalid /bin/sh+load_user_environment and hard caps above native limit → 400 validation_error.
 */
function nativeTaskCapabilityRejection(
  task: CreateTaskInput | TaskDispatch,
): { status: number; body: Record<string, unknown> } | null {
  if (task.kind === 'agent') {
    return {
      status: 409,
      body: {
        error: {
          code: 'capability_unavailable',
          message: 'Pi runtime is not available on this backend.',
          details: { capability: 'pi' },
        },
      },
    };
  }

  if (task.profile !== 'full') {
    return {
      status: 409,
      body: {
        error: {
          code: 'capability_unavailable',
          message: 'Only profile "full" is supported on this backend.',
          details: { capability: 'profile', profile: task.profile },
        },
      },
    };
  }

  if (task.environment !== undefined) {
    return {
      status: 409,
      body: {
        error: {
          code: 'capability_unavailable',
          message: 'Task environment injection is not supported on this backend.',
          details: { capability: 'environment' },
        },
      },
    };
  }

  if (task.retry !== undefined) {
    return {
      status: 409,
      body: {
        error: {
          code: 'capability_unavailable',
          message:
            'Automatic retry policy is not supported on this backend; use the manual retry endpoint.',
          details: { capability: 'retry' },
        },
      },
    };
  }

  if (task.verify !== undefined && task.verify.mode === 'command') {
    return {
      status: 409,
      body: {
        error: {
          code: 'capability_unavailable',
          message: 'Verify mode "command" is not supported on this backend.',
          details: { capability: 'verify.command' },
        },
      },
    };
  }

  const hardMax = task.output?.hard_max_bytes ?? 10_485_760;
  if (hardMax > NATIVE_STREAM_MAX_BYTES) {
    return {
      status: 400,
      body: {
        error: {
          code: 'validation_error',
          message:
            'output.hard_max_bytes exceeds the native vacps:process 64 MiB per-stream maximum.',
          details: {
            field: 'output.hard_max_bytes',
            max: NATIVE_STREAM_MAX_BYTES,
          },
        },
      },
    };
  }

  if (task.kind === 'shell' && task.shell === '/bin/sh' && task.load_user_environment === true) {
    return {
      status: 400,
      body: {
        error: {
          code: 'validation_error',
          message:
            'load_user_environment=true is not supported with shell=/bin/sh; use /bin/bash or set load_user_environment=false.',
        },
      },
    };
  }

  return null;
}
