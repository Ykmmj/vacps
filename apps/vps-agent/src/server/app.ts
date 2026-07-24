import { readFile } from 'node:fs/promises';
import os from 'node:os';

import Fastify, { type FastifyInstance } from 'fastify';
import { createTaskSchema, taskDispatchSchema } from '@vps-agent/contracts';
import type { AgentConfig } from '../config.js';
import type { PiRuntime } from '../pi/pi-runtime.js';
import type { TaskQueue } from '../queue/task-queue.js';

export async function createServer(input: {
  config: AgentConfig;
  queue: TaskQueue;
  piRuntime: PiRuntime;
}): Promise<FastifyInstance> {
  const app = Fastify({ logger: true, bodyLimit: 256 * 1024 });
  const startedAt = Date.now();
  app.addHook('onRequest', async (request, reply) => {
    const token = request.headers.authorization?.replace(/^Bearer\s+/i, '');
    if (token !== input.config.BACKEND_SHARED_TOKEN) {
      return reply
        .code(401)
        .send({ error: { code: 'unauthorized', message: 'A valid bearer token is required.' } });
    }
  });

  app.get('/health', async () => ({
    ok: true,
    backendId: input.config.BACKEND_ID,
    version: '0.1.0',
    uptimeSeconds: Math.floor((Date.now() - startedAt) / 1000),
    worker: {
      running: input.queue.isWorkerRunning(),
      concurrency: input.config.WORKER_CONCURRENCY,
    },
    redis: { connected: input.queue.isRedisConnected() },
    pi: await input.piRuntime.availability(),
  }));

  app.get('/info', async () => ({
    backendId: input.config.BACKEND_ID,
    queue: input.queue.queueName,
    runMode: input.config.RUN_MODE,
  }));

  app.get('/metrics', async () => {
    const queue = await input.queue.metrics();
    return {
      cpu: { load1: os.loadavg()[0] ?? 0 },
      memory: { totalBytes: os.totalmem(), usedBytes: os.totalmem() - os.freemem() },
      disk: { totalBytes: 0, usedBytes: 0 },
      queue,
    };
  });

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
    const task = await input.queue.getTask(id);
    if (!task.task)
      return reply.code(404).send({ error: { code: 'not_found', message: 'Task not found.' } });
    const commands = await Promise.all(
      task.commands.map(async (command) => ({
        ...command,
        stdout: command.stdoutPath ? await safeRead(command.stdoutPath) : '',
        stderr: command.stderrPath ? await safeRead(command.stderrPath) : '',
      })),
    );
    return { taskId: id, commands };
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
      taskTemplate: {
        type: 'shell',
        backendId: input.config.BACKEND_ID,
        command: 'true',
        cwd: '/',
        timeoutSeconds: 1,
        profile: 'full',
      },
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

async function safeRead(path: string): Promise<string> {
  try {
    return await readFile(path, 'utf8');
  } catch {
    return '';
  }
}
