import { WebStandardStreamableHTTPServerTransport } from '@modelcontextprotocol/sdk/server/webStandardStreamableHttp.js';
import {
  createScheduleSchema,
  createTaskSchema,
  registerBackendSchema,
  updateBackendSchema,
  updateScheduleSchema,
} from '@vps-agent/contracts';
import { z } from 'zod';

import type { Env } from './env.js';
import { AppError, errorResponse, json, readJson } from './lib/http.js';
import { createMcpServer } from './mcp/server.js';
import { BackendClient } from './registry/backend-client.js';
import { RegistrationRepository } from './registry/registration-repository.js';
import { BackendRepository } from './registry/repository.js';
import { ScheduleService } from './schedules/schedule-service.js';
import { TaskService } from './tasks/task-service.js';

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === '/mcp') {
      // MCP SDK >=1.26 requires a fresh server for every stateless request.
      const server = createMcpServer(env);
      const transport = new WebStandardStreamableHTTPServerTransport();
      await server.connect(transport);
      return transport.handleRequest(request);
    }
    if (url.pathname.startsWith('/api/')) return handleApi(request, env, crypto.randomUUID());
    return env.ASSETS.fetch(request);
  },
  async scheduled(_controller, env, ctx): Promise<void> {
    const services = createServices(env);
    ctx.waitUntil(services.schedules.reconcile());
  },
} satisfies ExportedHandler<Env>;

function createServices(env: Env) {
  const backends = new BackendRepository(env.DB);
  const registrations = new RegistrationRepository(env.DB);
  const client = new BackendClient(env.BACKEND_SHARED_TOKEN);
  const tasks = new TaskService(env.DB, backends, client);
  const schedules = new ScheduleService(env.DB, backends, client, tasks);
  return { backends, client, registrations, tasks, schedules };
}

async function handleApi(request: Request, env: Env, requestId: string): Promise<Response> {
  try {
    const { pathname, searchParams } = new URL(request.url);
    const segments = pathname.split('/').filter(Boolean);
    const services = createServices(env);
    const resource = segments[1];
    const id = segments[2];
    const action = segments[3];

    if (resource === 'dashboard' && request.method === 'GET') {
      const [backends, tasks, schedules, pendingRegistrations] = await Promise.all([
        services.backends.list(),
        services.tasks.list(100),
        services.schedules.list(),
        services.registrations.list('pending'),
      ]);
      const active = tasks.filter((task) => task.status === 'running').length;
      const queued = tasks.filter((task) => task.status === 'queued').length;
      const failed = tasks
        .filter((task) => ['failed', 'dispatch_failed', 'timed_out'].includes(task.status))
        .slice(0, 10);
      return json({
        totals: {
          backends: backends.length,
          enabledBackends: backends.filter((backend) => backend.enabled).length,
          queued,
          active,
          schedules: schedules.length,
          pendingRegistrations: pendingRegistrations.length,
        },
        backends,
        pendingRegistrations,
        failed,
      });
    }

    if (resource === 'registrations') {
      if (!id && request.method === 'POST') {
        requireBackendToken(request, env);
        return json(
          await services.registrations.request(
            registerBackendSchema.parse(await readJson(request)),
          ),
          {
            status: 202,
          },
        );
      }
      if (!id && request.method === 'GET') return json(await services.registrations.list());
      if (id && action === 'approve' && request.method === 'POST') {
        const registration = await services.registrations.get(id);
        if (registration.status === 'rejected')
          throw new AppError(
            'registration_rejected',
            'A rejected registration must request approval again.',
            409,
          );
        const health = await services.client.health(registration);
        if (health.backendId !== registration.backendId)
          throw new AppError(
            'backend_identity_mismatch',
            'The agent health response does not match the requested Backend ID.',
            409,
          );
        let backend;
        try {
          backend = await services.backends.get(registration.backendId);
        } catch {
          backend = await services.backends.create({
            id: registration.backendId,
            name: registration.name,
            baseUrl: registration.baseUrl,
            ...(registration.region ? { region: registration.region } : {}),
            tags: registration.tags,
            enabled: true,
          });
        }
        await services.backends.recordStatus(backend.id, health);
        return json({ registration: await services.registrations.approve(id), backend, health });
      }
      if (id && action === 'reject' && request.method === 'POST') {
        const input = z
          .object({ reason: z.string().trim().max(500).optional() })
          .parse(await readJson(request));
        return json(await services.registrations.reject(id, input.reason));
      }
    }

    if (resource === 'backends') {
      if (!id && request.method === 'GET') return json(await services.backends.list());
      if (id && !action && request.method === 'GET') return json(await services.backends.get(id));
      if (id && !action && request.method === 'PATCH')
        return json(
          await services.backends.update(id, updateBackendSchema.parse(await readJson(request))),
        );
      if (id && !action && request.method === 'DELETE') {
        await services.backends.delete(id);
        return new Response(null, { status: 204 });
      }
      if (id && action === 'test' && request.method === 'POST') {
        const backend = await services.backends.get(id);
        const health = await services.client.health(backend);
        await services.backends.recordStatus(id, health);
        return json(health);
      }
      if (id && action === 'status' && request.method === 'GET') {
        const backend = await services.backends.get(id);
        const status = await services.client.status(backend);
        await services.backends.recordStatus(id, status);
        return json(status);
      }
    }

    if (resource === 'tasks') {
      if (!id && request.method === 'GET')
        return json(await services.tasks.list(Number(searchParams.get('limit') ?? 50)));
      if (!id && request.method === 'POST')
        return json(
          await services.tasks.create(createTaskSchema.parse(await readJson(request)), 'web'),
          { status: 202 },
        );
      if (id && !action && request.method === 'GET') return json(await services.tasks.detail(id));
      if (id && action === 'logs' && request.method === 'GET')
        return json(await services.tasks.logs(id));
      if (id && action === 'cancel' && request.method === 'POST')
        return json(await services.tasks.cancel(id));
      if (id && action === 'retry' && request.method === 'POST')
        return json(await services.tasks.retry(id), { status: 202 });
    }

    if (resource === 'schedules') {
      if (!id && request.method === 'GET') return json(await services.schedules.list());
      if (!id && request.method === 'POST')
        return json(
          await services.schedules.create(createScheduleSchema.parse(await readJson(request))),
          { status: 201 },
        );
      if (id && !action && request.method === 'GET') return json(await services.schedules.get(id));
      if (id && !action && request.method === 'PATCH')
        return json(
          await services.schedules.update(id, updateScheduleSchema.parse(await readJson(request))),
        );
      if (id && !action && request.method === 'DELETE') {
        await services.schedules.delete(id);
        return new Response(null, { status: 204 });
      }
      if (id && action === 'run' && request.method === 'POST')
        return json(await services.schedules.runNow(id), { status: 202 });
      if (!id && action === 'reconcile' && request.method === 'POST')
        return json(await services.schedules.reconcile());
    }

    return json(
      { error: { code: 'not_found', message: 'API route not found.', requestId } },
      { status: 404 },
    );
  } catch (error) {
    return errorResponse(error, requestId);
  }
}

function requireBackendToken(request: Request, env: Env): void {
  const token = request.headers.get('authorization')?.replace(/^Bearer\s+/i, '');
  if (token !== env.BACKEND_SHARED_TOKEN) {
    throw new AppError(
      'unauthorized_registration',
      'A valid Backend Shared Token is required.',
      401,
    );
  }
}
