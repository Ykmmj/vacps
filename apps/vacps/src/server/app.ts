import { open, readFile, stat } from 'node:fs/promises';
import { spawn } from 'node:child_process';

import Fastify, { type FastifyInstance } from 'fastify';
import { createTaskSchema, taskDispatchSchema } from '@vacps/contracts';
import type { AgentConfig } from '../config.js';
import type { PiRuntime } from '../pi/pi-runtime.js';
import type { TaskQueue } from '../queue/task-queue.js';
import type { TaskStore } from '../storage/task-store.js';
import type { NodeTelemetryCollector } from '../telemetry/node-telemetry.js';
import { verifyControlPlaneRequest } from '../security/request-signatures.js';

export async function createServer(input: {
  config: AgentConfig;
  queue: TaskQueue;
  piRuntime: PiRuntime;
  telemetry: NodeTelemetryCollector;
  store: TaskStore;
}): Promise<FastifyInstance> {
  const app = Fastify({ logger: true, bodyLimit: 256 * 1024 });
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

  app.get('/health', async () => (await input.telemetry.collect()).health);

  app.get('/info', async () => ({
    backendId: input.config.BACKEND_ID,
    queue: input.queue.queueName,
    runMode: input.config.RUN_MODE,
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

  // Layer A: vacps.read
  app.get('/fs/read', async (request, reply) => {
    const query = request.query as { file_path?: string; offset?: string; limit?: string };
    const filePath = query.file_path?.trim();
    if (!filePath?.startsWith('/'))
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'file_path must be absolute.' } });
    try {
      const raw = await readFile(filePath, 'utf8');
      const lines = raw.split('\n');
      const offset = Math.max(1, Number(query.offset ?? 1) || 1);
      const limit = Math.min(Math.max(Number(query.limit ?? 2000) || 2000, 1), 5000);
      const slice = lines.slice(offset - 1, offset - 1 + limit);
      const numbered = slice
        .map((line, index) => {
          const text = line.length > 2000 ? `${line.slice(0, 2000)}…` : line;
          return `${String(offset + index).padStart(6, ' ')}	${text}`;
        })
        .join('\n');
      const nextOffset = offset + slice.length;
      const truncated = nextOffset <= lines.length;
      return {
        ok: true,
        backend_id: input.config.BACKEND_ID,
        file_path: filePath,
        offset,
        limit,
        total_lines: lines.length,
        truncated,
        next_offset: truncated ? nextOffset : null,
        content: numbered,
      };
    } catch (error) {
      return reply.code(404).send({
        error: {
          code: 'not_found',
          message: error instanceof Error ? error.message : 'File not found.',
        },
      });
    }
  });

  // Layer A: vacps.bash (foreground only for v1)
  app.post('/exec/bash', async (request, reply) => {
    const body = request.body as {
      command?: unknown;
      timeout_ms?: unknown;
      cwd?: unknown;
      description?: unknown;
    };
    if (typeof body.command !== 'string' || !body.command.trim())
      return reply
        .code(400)
        .send({ error: { code: 'validation_error', message: 'command is required.' } });
    const timeoutMs = Math.min(
      Math.max(typeof body.timeout_ms === 'number' ? body.timeout_ms : 120_000, 1),
      600_000,
    );
    const cwd = typeof body.cwd === 'string' && body.cwd.startsWith('/') ? body.cwd : process.cwd();
    const result = await runBashCommand(body.command, cwd, timeoutMs);
    return {
      ok: true,
      backend_id: input.config.BACKEND_ID,
      status: result.timedOut ? 'timed_out' : 'exited',
      exit_code: result.exitCode,
      signal: null,
      timed_out: result.timedOut,
      stdout_preview: result.stdout.slice(0, 30_000),
      stderr_preview: result.stderr.slice(0, 30_000),
      stdout_truncated: result.stdout.length > 30_000,
      stderr_truncated: result.stderr.length > 30_000,
      stdout_bytes: Buffer.byteLength(result.stdout),
      stderr_bytes: Buffer.byteLength(result.stderr),
      shell_id: null,
    };
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

async function runBashCommand(
  command: string,
  cwd: string,
  timeoutMs: number,
): Promise<{ exitCode: number | null; stdout: string; stderr: string; timedOut: boolean }> {
  return new Promise((resolve) => {
    const child = spawn('/bin/bash', ['-lc', command], {
      cwd,
      env: process.env,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    let timedOut = false;
    const append = (current: string, chunk: Buffer) => {
      const remaining = 1_048_576 - Buffer.byteLength(current);
      return remaining > 0 ? current + chunk.subarray(0, remaining).toString('utf8') : current;
    };
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill('SIGTERM');
    }, timeoutMs);
    child.stdout.on('data', (chunk: Buffer) => {
      stdout = append(stdout, chunk);
    });
    child.stderr.on('data', (chunk: Buffer) => {
      stderr = append(stderr, chunk);
    });
    child.on('close', (code) => {
      clearTimeout(timer);
      resolve({ exitCode: code, stdout, stderr, timedOut });
    });
    child.on('error', () => {
      clearTimeout(timer);
      resolve({ exitCode: 1, stdout, stderr, timedOut });
    });
  });
}
