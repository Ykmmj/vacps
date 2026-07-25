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

  server.registerTool(
    'backends.list',
    { description: 'List enabled and disabled VPS agent backends.' },
    async () => response(await backends.list()),
  );
  server.registerTool(
    'backends.get_status',
    {
      description: 'Get health and queue status for one VPS agent.',
      inputSchema: { backendId: z.string() },
    },
    async ({ backendId }) => {
      const backend = await backends.get(backendId);
      const status = await client.status(backend);
      await backends.recordStatus(backendId, status);
      return response(status);
    },
  );
  server.registerTool(
    'tasks.create',
    {
      description:
        'Queue a Shell command or natural-language Pi agent task. This returns immediately.',
      inputSchema: {
        backendId: z.string(),
        type: z.enum(['shell', 'agent']),
        command: z.string().optional(),
        prompt: z.string().optional(),
        cwd: z.string(),
        profile: z.literal('full').default('full'),
        timeoutSeconds: z.number().int().min(1).max(86_400),
      },
    },
    async (input) => response(await tasks.create(parseTaskInput(input), 'mcp')),
  );
  server.registerTool(
    'tasks.get',
    {
      description: 'Get a task and its latest state from the target VPS.',
      inputSchema: { taskId: z.uuid() },
    },
    async ({ taskId }) => response(await tasks.detail(taskId)),
  );
  server.registerTool(
    'tasks.list',
    {
      description: 'List recent task summaries.',
      inputSchema: { limit: z.number().int().min(1).max(200).default(50) },
    },
    async ({ limit }) => response(await tasks.list(limit)),
  );
  server.registerTool(
    'tasks.cancel',
    { description: 'Cancel a queued or running task.', inputSchema: { taskId: z.uuid() } },
    async ({ taskId }) => response(await tasks.cancel(taskId)),
  );
  server.registerTool(
    'tasks.retry',
    {
      description: 'Retry a task after an explicit user request.',
      inputSchema: { taskId: z.uuid() },
    },
    async ({ taskId }) => response(await tasks.retry(taskId)),
  );
  server.registerTool(
    'schedules.create',
    {
      description: 'Create a cron schedule backed by BullMQ on the selected VPS.',
      inputSchema: scheduleToolSchema,
    },
    async (input) => response(await schedules.create(parseScheduleInput(input))),
  );
  server.registerTool(
    'schedules.get',
    { description: 'Get a schedule definition.', inputSchema: { scheduleId: z.uuid() } },
    async ({ scheduleId }) => response(await schedules.get(scheduleId)),
  );
  server.registerTool('schedules.list', { description: 'List cron schedules.' }, async () =>
    response(await schedules.list()),
  );
  server.registerTool(
    'schedules.update',
    {
      description: 'Update a cron schedule.',
      inputSchema: { scheduleId: z.uuid(), ...scheduleToolSchema },
    },
    async ({ scheduleId, ...input }) =>
      response(await schedules.update(scheduleId, parseScheduleInput(input))),
  );
  server.registerTool(
    'schedules.delete',
    { description: 'Delete a cron schedule.', inputSchema: { scheduleId: z.uuid() } },
    async ({ scheduleId }) => {
      await schedules.delete(scheduleId);
      return response({ deleted: true, scheduleId });
    },
  );
  server.registerTool(
    'schedules.run_now',
    {
      description: 'Immediately queue a schedule task once.',
      inputSchema: { scheduleId: z.uuid() },
    },
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
