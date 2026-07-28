import { readFile } from 'node:fs/promises';

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
