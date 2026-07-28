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
import { BackendClient } from '../registry/backend-client.js';
import { BackendRepository } from '../registry/repository.js';
import { ScheduleService } from '../schedules/schedule-service.js';
import { TaskService } from '../tasks/task-service.js';

// Output schemas describe tool results so MCP clients (and the model) can interpret them. They are
// deliberately lenient: agent-reported health/metrics and remote task/schedule payloads are not
// re-validated by the control plane, so we use `looseObject` (tolerate extra keys), drop numeric
// range constraints, and fall back to `z.unknown()` for fields that pass through raw agent output.
// The SDK validates returned `structuredContent` against these at runtime, so strictness here would
// turn a working tool into an error whenever a backend's payload drifts.

const backendStatusOutput = z.looseObject({
  health: z.looseObject({
    ok: z.boolean(),
    backendId: z.string(),
    version: z.string(),
    uptimeSeconds: z.number(),
    worker: z.looseObject({ running: z.boolean(), concurrency: z.number() }),
    redis: z.looseObject({ connected: z.boolean() }),
    pi: z.looseObject({ available: z.boolean(), version: z.string().optional() }),
  }),
  metrics: z
    .looseObject({
      cpu: z.looseObject({ usagePercent: z.number(), load1: z.number(), cores: z.number() }),
      memory: z.looseObject({ totalBytes: z.number(), usedBytes: z.number() }),
      disk: z.looseObject({ totalBytes: z.number(), usedBytes: z.number() }),
      network: z
        .looseObject({
          receivedBytesPerSecond: z.number(),
          transmittedBytesPerSecond: z.number(),
        })
        .optional(),
      queue: z.looseObject({ waiting: z.number(), active: z.number(), failed: z.number() }),
    })
    .optional(),
  system: z
    .looseObject({
      platform: z.string(),
      distribution: z.string().optional(),
      version: z.string().optional(),
      kernel: z.string(),
      architecture: z.string(),
    })
    .optional(),
});

const backendOutput = z.looseObject({
  id: z.string(),
  name: z.string(),
  baseUrl: z.string(),
  tags: z.array(z.string()),
  enabled: z.boolean(),
  createdAt: z.string(),
  updatedAt: z.string(),
  lastStatus: backendStatusOutput.optional(),
  lastCheckedAt: z.string().optional(),
});

const taskOutput = z.looseObject({
  id: z.string(),
  backendId: z.string(),
  type: z.enum(['shell', 'agent']),
  source: z.enum(['mcp', 'web', 'schedule', 'api']),
  profile: z.string(),
  summary: z.string().optional(),
  status: z.enum(taskStatuses),
  scheduleId: z.string().optional(),
  createdAt: z.string(),
  updatedAt: z.string(),
  finishedAt: z.string().optional(),
});

const scheduleOutput = z.looseObject({
  id: z.string(),
  backendId: z.string(),
  name: z.string(),
  cron: z.string(),
  timezone: z.string(),
  enabled: z.boolean(),
  // The task template is a discriminated union with defaults/transforms; kept opaque here to avoid
  // re-validating stored templates.
  taskTemplate: z.unknown(),
  lastRunAt: z.string().optional(),
  nextRunAt: z.string().optional(),
  createdAt: z.string(),
  updatedAt: z.string(),
});

export function createMcpServer(env: Env): McpServer {
  const backends = new BackendRepository(env.DB);
  const client = new BackendClient(env.CONTROL_PLANE_SIGNING_PRIVATE_KEY);
  const tasks = new TaskService(env.DB, backends, client);
  const schedules = new ScheduleService(env.DB, backends, client, tasks);
  const server = new McpServer({ name: 'vacps', version: '0.1.0' });
  const respond = (value: unknown) => ({
    structuredContent: value as Record<string, unknown>,
    content: [{ type: 'text' as const, text: JSON.stringify(value, null, 2) }],
  });

  server.registerTool(
    'backends.list',
    {
      description: 'List enabled and disabled VPS agent backends.',
      outputSchema: { backends: z.array(backendOutput) },
    },
    async () => respond({ backends: await backends.list() }),
  );
  server.registerTool(
    'backends.get_status',
    {
      description: 'Get health and queue status for one VPS agent.',
      inputSchema: { backendId: z.string() },
      outputSchema: backendStatusOutput.shape,
    },
    async ({ backendId }) => {
      const backend = await backends.get(backendId);
      const status = await client.status(backend);
      await backends.recordStatus(backendId, status, { preserveSystem: true });
      return respond(status);
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
      outputSchema: taskOutput.shape,
    },
    async (input) => respond(await tasks.create(parseTaskInput(input), 'mcp')),
  );
  server.registerTool(
    'tasks.get',
    {
      description: 'Get a task and its latest state from the target VPS.',
      inputSchema: { taskId: z.uuid() },
      outputSchema: { task: taskOutput, backend: z.unknown() },
    },
    async ({ taskId }) => respond(await tasks.detail(taskId)),
  );
  server.registerTool(
    'tasks.list',
    {
      description: 'List recent task summaries.',
      inputSchema: { limit: z.number().int().min(1).max(200).default(50) },
      outputSchema: { tasks: z.array(taskOutput) },
    },
    async ({ limit }) => respond({ tasks: await tasks.list(limit) }),
  );
  server.registerTool(
    'tasks.cancel',
    {
      description: 'Cancel a queued or running task.',
      inputSchema: { taskId: z.uuid() },
      outputSchema: { task: taskOutput, result: z.unknown() },
    },
    async ({ taskId }) => respond(await tasks.cancel(taskId)),
  );
  server.registerTool(
    'tasks.retry',
    {
      description: 'Retry a task after an explicit user request.',
      inputSchema: { taskId: z.uuid() },
      outputSchema: { task: taskOutput, result: z.unknown() },
    },
    async ({ taskId }) => respond(await tasks.retry(taskId)),
  );
  server.registerTool(
    'schedules.create',
    {
      description: 'Create a cron schedule backed by BullMQ on the selected VPS.',
      inputSchema: scheduleToolSchema,
      outputSchema: scheduleOutput.shape,
    },
    async (input) => respond(await schedules.create(parseScheduleInput(input))),
  );
  server.registerTool(
    'schedules.get',
    {
      description: 'Get a schedule definition.',
      inputSchema: { scheduleId: z.uuid() },
      outputSchema: scheduleOutput.shape,
    },
    async ({ scheduleId }) => respond(await schedules.get(scheduleId)),
  );
  server.registerTool(
    'schedules.list',
    { description: 'List cron schedules.', outputSchema: { schedules: z.array(scheduleOutput) } },
    async () => respond({ schedules: await schedules.list() }),
  );
  server.registerTool(
    'schedules.update',
    {
      description: 'Update a cron schedule.',
      inputSchema: { scheduleId: z.uuid(), ...scheduleToolSchema },
      outputSchema: scheduleOutput.shape,
    },
    async ({ scheduleId, ...input }) =>
      respond(await schedules.update(scheduleId, parseScheduleInput(input))),
  );
  server.registerTool(
    'schedules.delete',
    {
      description: 'Delete a cron schedule.',
      inputSchema: { scheduleId: z.uuid() },
      outputSchema: { deleted: z.boolean(), scheduleId: z.string() },
    },
    async ({ scheduleId }) => {
      await schedules.delete(scheduleId);
      return respond({ deleted: true, scheduleId });
    },
  );
  server.registerTool(
    'schedules.run_now',
    {
      description: 'Immediately queue a schedule task once.',
      inputSchema: { scheduleId: z.uuid() },
      outputSchema: taskOutput.shape,
    },
    async ({ scheduleId }) => respond(await schedules.runNow(scheduleId)),
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
