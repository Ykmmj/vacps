import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import {
  createScheduleSchema,
  createTaskSchema,
  type CreateScheduleInput,
  type CreateTaskInput,
} from '@vps-agent/contracts';
import { z } from 'zod';

import type { Env } from '../env.js';
import { BackendClient } from '../registry/backend-client.js';
import { BackendRepository } from '../registry/repository.js';
import { ScheduleService } from '../schedules/schedule-service.js';
import { TaskService } from '../tasks/task-service.js';

export function createMcpServer(env: Env): McpServer {
  const backends = new BackendRepository(env.DB);
  const client = new BackendClient(env.BACKEND_SHARED_TOKEN);
  const tasks = new TaskService(env.DB, backends, client);
  const schedules = new ScheduleService(env.DB, backends, client, tasks);
  const server = new McpServer({ name: 'vps-agent-platform', version: '0.1.0' });
  const response = (value: unknown) => ({
    content: [{ type: 'text' as const, text: JSON.stringify(value, null, 2) }],
  });

  server.tool('backends.list', 'List enabled and disabled VPS agent backends.', {}, async () =>
    response(await backends.list()),
  );
  server.tool(
    'backends.get_status',
    'Get health and queue status for one VPS agent.',
    { backendId: z.string() },
    async ({ backendId }) => {
      const backend = await backends.get(backendId);
      const status = await client.status(backend);
      await backends.recordStatus(backendId, status);
      return response(status);
    },
  );
  server.tool(
    'tasks.create',
    'Queue a Shell command or natural-language Pi agent task. This returns immediately.',
    {
      backendId: z.string(),
      type: z.enum(['shell', 'agent']),
      command: z.string().optional(),
      prompt: z.string().optional(),
      cwd: z.string(),
      profile: z.literal('full').default('full'),
      timeoutSeconds: z.number().int().min(1).max(86_400),
    },
    async (input) => response(await tasks.create(parseTaskInput(input), 'mcp')),
  );
  server.tool(
    'tasks.get',
    'Get a task and its latest state from the target VPS.',
    { taskId: z.string().uuid() },
    async ({ taskId }) => response(await tasks.detail(taskId)),
  );
  server.tool(
    'tasks.list',
    'List recent task summaries.',
    { limit: z.number().int().min(1).max(200).default(50) },
    async ({ limit }) => response(await tasks.list(limit)),
  );
  server.tool(
    'tasks.cancel',
    'Cancel a queued or running task.',
    { taskId: z.string().uuid() },
    async ({ taskId }) => response(await tasks.cancel(taskId)),
  );
  server.tool(
    'tasks.retry',
    'Retry a task after an explicit user request.',
    { taskId: z.string().uuid() },
    async ({ taskId }) => response(await tasks.retry(taskId)),
  );
  server.tool(
    'schedules.create',
    'Create a cron schedule backed by BullMQ on the selected VPS.',
    scheduleToolSchema,
    async (input) => response(await schedules.create(parseScheduleInput(input))),
  );
  server.tool(
    'schedules.get',
    'Get a schedule definition.',
    { scheduleId: z.string().uuid() },
    async ({ scheduleId }) => response(await schedules.get(scheduleId)),
  );
  server.tool('schedules.list', 'List cron schedules.', {}, async () =>
    response(await schedules.list()),
  );
  server.tool(
    'schedules.update',
    'Update a cron schedule.',
    { scheduleId: z.string().uuid(), ...scheduleToolSchema },
    async ({ scheduleId, ...input }) =>
      response(await schedules.update(scheduleId, parseScheduleInput(input))),
  );
  server.tool(
    'schedules.delete',
    'Delete a cron schedule.',
    { scheduleId: z.string().uuid() },
    async ({ scheduleId }) => {
      await schedules.delete(scheduleId);
      return response({ deleted: true, scheduleId });
    },
  );
  server.tool(
    'schedules.run_now',
    'Immediately queue a schedule task once.',
    { scheduleId: z.string().uuid() },
    async ({ scheduleId }) => response(await schedules.runNow(scheduleId)),
  );
  return server;
}

const scheduleToolSchema = {
  backendId: z.string(),
  name: z.string(),
  cron: z.string(),
  timezone: z.string().default('UTC'),
  enabled: z.boolean().default(true),
  taskTemplate: z.object({
    type: z.enum(['shell', 'agent']),
    command: z.string().optional(),
    prompt: z.string().optional(),
    cwd: z.string(),
    profile: z.literal('full').default('full'),
    timeoutSeconds: z.number().int().min(1).max(86_400),
  }),
};

function parseTaskInput(input: unknown): CreateTaskInput {
  return createTaskSchema.parse(input);
}

function parseScheduleInput(input: unknown): CreateScheduleInput {
  return createScheduleSchema.parse(input);
}
