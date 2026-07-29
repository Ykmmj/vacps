import { OAuthProvider } from '@cloudflare/workers-oauth-provider';
import {
  createScheduleSchema,
  backendTelemetrySchema,
  createTaskSchema,
  registerBackendSchema,
  telemetrySettingsSchema,
  updateBackendSchema,
  updateScheduleSchema,
} from '@vacps/contracts';
import type { Backend, BackendRegistration, BackendStatus } from '@vacps/contracts';
import { z } from 'zod';

import {
  clearSessionCookie,
  createSessionCookie,
  passwordMatches,
  requireAuthenticated,
} from './auth/session.js';
import { CloudflareOAuthService } from './cloudflare/oauth-service.js';
import type { Env } from './env.js';
import { AppError, errorResponse, json, readJson } from './lib/http.js';
import { handleAuthorize } from './mcp/authorize-page.js';
import { publicToolJsonSchemas } from './mcp/tool-schemas.js';
import { BackendClient } from './registry/backend-client.js';
import { CloudflareOAuthRepository } from './registry/cloudflare-oauth-repository.js';
import { ManagedTunnelRepository } from './registry/managed-tunnel-repository.js';
import { AgentSignatureRepository } from './registry/agent-signature-repository.js';
import { RegistrationRepository } from './registry/registration-repository.js';
import { RegistrationTokenRepository } from './registry/registration-token-repository.js';
import { BackendRepository } from './registry/repository.js';
import { verifyAgentRequestSignature } from './security/request-signatures.js';
import { ScheduleService } from './schedules/schedule-service.js';
import { TaskService } from './tasks/task-service.js';
import { TelemetrySettingsRepository } from './telemetry/settings-repository.js';
import { ManagedTunnelService } from './tunnels/managed-tunnel-service.js';

// The Remote MCP endpoint. The OAuth provider validates the bearer token before delegating here, so
// unlike the cookie-authenticated WebUI there is no session or same-origin check — the token replaces
// both, and the granted identity is available on `ctx.props`.
//
// IMPORTANT: load MCP SDK + server via dynamic import. Static import evaluates MCP types.ts
// top-level z.custom() during Worker startup; esbuild + Zod v4 then throws
// "Class2 is not a constructor" (CF error 10021) on deploy validation.
const mcpApiHandler = {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      const [{ createMcpServer }, { WebStandardStreamableHTTPServerTransport }] = await Promise.all(
        [
          import('./mcp/server.js'),
          import('@modelcontextprotocol/sdk/server/webStandardStreamableHttp.js'),
        ],
      );
      // MCP SDK >=1.26 requires a fresh server for every stateless request.
      const server = createMcpServer(env);
      const transport = new WebStandardStreamableHTTPServerTransport();
      await server.connect(transport);
      return transport.handleRequest(request);
    } catch (error) {
      return errorResponse(error, crypto.randomUUID());
    }
  },
};

// Everything that is not a valid MCP API request: the password-authenticated WebUI and `/api/*`, the
// server-rendered OAuth `/authorize` consent page, and static assets.
const defaultHandler = {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === '/authorize') return handleAuthorize(request, env);
    if (url.pathname.startsWith('/api/')) return handleApi(request, env, crypto.randomUUID());
    return env.ASSETS.fetch(request);
  },
};

// The provider implements `/token`, `/register` (dynamic client registration), and the
// `.well-known/oauth-authorization-server` + `.well-known/oauth-protected-resource` metadata endpoints.
const oauthProvider = new OAuthProvider({
  apiRoute: '/mcp',
  apiHandler: mcpApiHandler,
  defaultHandler,
  authorizeEndpoint: '/authorize',
  tokenEndpoint: '/token',
  clientRegistrationEndpoint: '/register',
  scopesSupported: ['mcp'],
});

export default {
  fetch: (request, env, ctx) => oauthProvider.fetch(request, env, ctx),
  async scheduled(_controller, env, ctx): Promise<void> {
    const services = createServices(env);
    ctx.waitUntil(services.schedules.reconcile());
    ctx.waitUntil(
      Promise.all([
        services.registrationTokens.purgeExpired(),
        services.agentSignatures.purgeExpired(),
        // Soft-delete expired test tasks; hard-delete soft-deleted past grace.
        services.tasks.purgeExpired(),
      ]),
    );
    // Best-effort cleanup of expired OAuth grants/tokens from KV.
    ctx.waitUntil(oauthProvider.purgeExpiredData(env));
  },
} satisfies ExportedHandler<Env>;

function createServices(env: Env) {
  const backends = new BackendRepository(env.DB);
  const registrations = new RegistrationRepository(env.DB);
  const registrationTokens = new RegistrationTokenRepository(env.DB);
  const agentSignatures = new AgentSignatureRepository(env.DB);
  const managedTunnels = new ManagedTunnelRepository(env.DB);
  const cloudflareOAuth = new CloudflareOAuthService(env, new CloudflareOAuthRepository(env.DB));
  const client = new BackendClient(env.CONTROL_PLANE_SIGNING_PRIVATE_KEY);
  const tunnels = new ManagedTunnelService(cloudflareOAuth, managedTunnels);
  const tasks = new TaskService(env.DB, backends, client);
  const schedules = new ScheduleService(env.DB, backends, client, tasks);
  const telemetrySettings = new TelemetrySettingsRepository(env.DB);
  return {
    backends,
    client,
    registrations,
    registrationTokens,
    agentSignatures,
    managedTunnels,
    cloudflareOAuth,
    tunnels,
    tasks,
    schedules,
    telemetrySettings,
  };
}

type ControlServices = ReturnType<typeof createServices>;

/** Tear down registration, backend row, nonces, and the Cloudflare tunnel for one node. */
async function removeNode(services: ControlServices, backendId: string): Promise<void> {
  await services.tunnels.remove(backendId);
  await services.agentSignatures.purgeBackend(backendId);
  await services.registrations.deleteByBackendId(backendId);
  await services.backends.deleteIfPresent(backendId);
}

async function handleApi(request: Request, env: Env, requestId: string): Promise<Response> {
  try {
    const { pathname, searchParams } = new URL(request.url);
    const segments = pathname.split('/').filter(Boolean);
    const services = createServices(env);
    const resource = segments[1];
    const id = segments[2];
    const action = segments[3];

    if (resource === 'auth') return handleAuth(request, env, id);

    const isAgentRequest =
      (resource === 'registrations' && !id && request.method === 'POST') ||
      (resource === 'telemetry' && request.method === 'POST');
    if (!isAgentRequest) {
      await requireAuthenticated(request, env);
      if (isStateChanging(request)) requireSameOrigin(request);
    }

    // Operators / CI can verify tools/list contracts without an MCP client cache.
    if (resource === 'tool-schemas' && request.method === 'GET') {
      return json({ ok: true, ...publicToolJsonSchemas() });
    }

    if (resource === 'dashboard' && request.method === 'GET') {
      const [backends, tasks, schedulePage, registrations, telemetry] = await Promise.all([
        services.backends.list(),
        services.tasks.list(100),
        services.schedules.list({ limit: 200 }),
        services.registrations.list(),
        services.telemetrySettings.get(),
      ]);
      const schedules = schedulePage.schedules;
      const backendById = new Map(backends.map((backend) => [backend.id, backend]));
      const nodes = registrations.map((registration) =>
        inspectNode(
          registration,
          backendById.get(registration.backendId),
          telemetry.intervalSeconds,
        ),
      );
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
          pendingRegistrations: registrations.filter(
            (registration) => registration.status === 'pending',
          ).length,
        },
        backends,
        nodes,
        failed,
        telemetry,
      });
    }

    if (resource === 'telemetry') {
      if (request.method !== 'POST')
        throw new AppError('method_not_allowed', 'Telemetry requires POST.', 405);
      const { input: telemetry, body } = await readSignedJson(request, backendTelemetrySchema);
      const publicKey = await services.registrations.getPublicKey(telemetry.backendId);
      const identity = await verifyAgentRequestSignature(request, publicKey, body);
      if (identity.backendId !== telemetry.backendId)
        throw new AppError(
          'backend_identity_mismatch',
          'Telemetry backend ID does not match.',
          409,
        );
      await services.agentSignatures.claimNonce(identity.backendId, identity.nonce);
      const registration = await services.registrations.getByBackendId(telemetry.backendId);
      if (registration.status !== 'approved')
        throw new AppError(
          'telemetry_not_approved',
          'Telemetry is accepted only after node approval.',
          409,
        );
      const backend = await services.backends.get(telemetry.backendId);
      if (telemetry.health.backendId !== backend.id)
        throw new AppError(
          'backend_identity_mismatch',
          'Telemetry backend ID does not match.',
          409,
        );
      await services.backends.recordStatus(backend.id, {
        health: telemetry.health,
        metrics: telemetry.metrics,
        system: telemetry.system,
      });
      return json(await services.telemetrySettings.get());
    }

    if (resource === 'telemetry-settings') {
      if (request.method === 'GET') return json(await services.telemetrySettings.get());
      if (request.method === 'PATCH')
        return json(
          await services.telemetrySettings.update(
            telemetrySettingsSchema.parse(await readJson(request)),
          ),
        );
    }

    if (resource === 'registrations') {
      if (!id && request.method === 'POST') {
        const { input, body } = await readSignedJson(request, registerBackendSchema);
        const registrationToken = registrationTokenFromRequest(request);
        const publicKey = registrationToken
          ? input.publicKey
          : await services.registrations.getPublicKey(input.backendId);
        const identity = await verifyAgentRequestSignature(request, publicKey, body);
        if (identity.backendId !== input.backendId)
          throw new AppError(
            'backend_identity_mismatch',
            'Registration backend ID does not match.',
            409,
          );
        if (registrationToken) {
          try {
            await services.registrationTokens.consume(registrationToken);
          } catch (error) {
            // The installer intentionally retains an already-spent token in its root-owned
            // environment file. On a restart, an enrolled Agent is still allowed to refresh a
            // Quick Tunnel URL, but only if it proves the pre-existing private-key identity.
            const enrolledPublicKey = await services.registrations
              .getPublicKey(input.backendId)
              .catch(() => undefined);
            if (enrolledPublicKey !== input.publicKey) throw error;
          }
        }
        await services.agentSignatures.claimNonce(identity.backendId, identity.nonce);
        const registration = await services.registrations.request(
          input,
          registrationNetwork(request),
        );
        if (registration.status === 'approved') {
          try {
            await services.backends.update(registration.backendId, {
              name: registration.name,
              baseUrl: registration.baseUrl,
              tags: registration.tags,
            });
          } catch (error) {
            if (!(error instanceof AppError) || error.code !== 'backend_not_found') throw error;
          }
        }
        return json(registration, {
          status: 202,
        });
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
        const status = await services.client.status(registration);
        const { health } = status;
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
            tags: registration.tags,
            enabled: true,
          });
        }
        await services.backends.recordStatus(backend.id, status, { preserveSystem: true });
        return json({ registration: await services.registrations.approve(id), backend, health });
      }
      if (id && action === 'reject' && request.method === 'POST') {
        const input = z
          .object({ reason: z.string().trim().max(500).optional() })
          .parse(await readJson(request));
        return json(await services.registrations.reject(id, input.reason));
      }
      if (id && !action && request.method === 'DELETE') {
        const registration = await services.registrations.get(id);
        await removeNode(services, registration.backendId);
        return new Response(null, { status: 204 });
      }
    }

    if (resource === 'registration-tokens' && !id && request.method === 'POST') {
      if (!env.CONTROL_PLANE_SIGNING_PUBLIC_KEY)
        throw new AppError(
          'control_plane_identity_unconfigured',
          'Control-plane signing key is not configured.',
          503,
        );
      return json(
        {
          ...(await services.registrationTokens.issue()),
          controlPlanePublicKey: env.CONTROL_PLANE_SIGNING_PUBLIC_KEY,
        },
        { status: 201 },
      );
    }

    if (resource === 'tunnels') {
      if (!id && request.method === 'GET') return json(await services.tunnels.listAvailable());
      if (id === 'provision' && request.method === 'POST') {
        const input = z
          .object({ name: z.string().trim().min(1).max(120).optional() })
          .parse(await readJson(request));
        return json(await services.tunnels.provision(input), { status: 201 });
      }
      if (id === 'attach' && request.method === 'POST') {
        const input = z
          .object({
            tunnelId: z.string().uuid(),
            backendId: z
              .string()
              .regex(
                /^(?:vacps|vps)-[a-f0-9]{12}$/i,
                'backendId must match vacps-<12 hex characters>.',
              )
              .optional(),
          })
          .parse(await readJson(request));
        return json(await services.tunnels.attach(input), { status: 201 });
      }
      if (id && !action && request.method === 'DELETE') {
        await services.tunnels.remove(id);
        return new Response(null, { status: 204 });
      }
    }

    if (resource === 'cloudflare' && id === 'oauth') {
      if (action === 'callback' && request.method === 'GET')
        return services.cloudflareOAuth.callback(request);
      if (action === 'status' && request.method === 'GET')
        return json(await services.cloudflareOAuth.status());
      if (action === 'connect' && request.method === 'POST')
        return json(await services.cloudflareOAuth.begin());
      if (action === 'zones' && request.method === 'GET')
        return json(await services.cloudflareOAuth.zones());
      if (action === 'zone' && request.method === 'POST') {
        const input = z
          .object({
            zoneId: z
              .string()
              .regex(/^[0-9a-f]{32}$/i, 'Zone ID must be 32 hexadecimal characters.'),
          })
          .parse(await readJson(request));
        return json(await services.cloudflareOAuth.selectZone(input.zoneId));
      }
      if (action === 'connection' && request.method === 'DELETE') {
        await services.cloudflareOAuth.disconnect();
        return new Response(null, { status: 204 });
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
        await removeNode(services, id);
        return new Response(null, { status: 204 });
      }
      if (id && action === 'test' && request.method === 'POST') {
        const backend = await services.backends.get(id);
        const status = await services.client.status(backend);
        await services.backends.recordStatus(id, status, { preserveSystem: true });
        return json(status);
      }
      if (id && action === 'status' && request.method === 'GET') {
        const backend = await services.backends.get(id);
        const status = await services.client.status(backend);
        await services.backends.recordStatus(id, status, { preserveSystem: true });
        return json(status);
      }
    }

    if (resource === 'tasks') {
      if (!id && request.method === 'GET') {
        return json(
          await services.tasks.list({
            limit: Number(searchParams.get('limit') ?? 50),
            ...(searchParams.get('backend_id')
              ? { backendId: searchParams.get('backend_id')! }
              : {}),
            ...(searchParams.get('status') ? { status: searchParams.get('status')! } : {}),
            ...(searchParams.get('environment')
              ? { environment: searchParams.get('environment')! }
              : {}),
            ...(searchParams.get('source') ? { source: searchParams.get('source')! } : {}),
            ...(searchParams.get('hide_test') === '1' || searchParams.get('hide_test') === 'true'
              ? { hideTest: true }
              : {}),
            ...(searchParams.get('include_deleted') === '1' ||
            searchParams.get('include_deleted') === 'true'
              ? { includeDeleted: true }
              : {}),
            ...(searchParams.get('created_after')
              ? { createdAfter: searchParams.get('created_after')! }
              : {}),
          }),
        );
      }
      if (!id && request.method === 'POST')
        return json(
          await services.tasks.create(createTaskSchema.parse(await readJson(request)), 'web'),
          { status: 202 },
        );
      if (id && !action && request.method === 'GET') return json(await services.tasks.detail(id));
      if (id && !action && request.method === 'DELETE') {
        const body = (await readJson(request).catch(() => ({}))) as {
          mode?: 'soft' | 'hard';
          reason?: string;
        };
        return json(
          await services.tasks.delete(id, {
            ...(body.mode ? { mode: body.mode } : {}),
            ...(body.reason ? { reason: body.reason } : {}),
            deletedBy: 'web',
          }),
        );
      }
      if (id && action === 'logs' && request.method === 'GET')
        return json(await services.tasks.logs(id));
      if (id && action === 'cancel' && request.method === 'POST')
        return json(await services.tasks.cancel(id));
      if (id && action === 'retry' && request.method === 'POST')
        return json(await services.tasks.retry(id), { status: 202 });
    }

    if (resource === 'schedules') {
      if (!id && request.method === 'GET') {
        const page = await services.schedules.list({
          limit: Number(searchParams.get('limit') ?? 200),
          ...(searchParams.get('backend_id') ? { backendId: searchParams.get('backend_id')! } : {}),
        });
        return json(page.schedules);
      }
      if (!id && request.method === 'POST')
        return json(
          await services.schedules.create(createScheduleSchema.parse(await readJson(request))),
          { status: 201 },
        );
      if (id && !action && request.method === 'GET') return json(await services.schedules.get(id));
      if (id && !action && request.method === 'PATCH') {
        const body = (await readJson(request)) as Record<string, unknown>;
        // Schema v3 patch: { expected_revision?, changes: { name, enabled, trigger, policy, task } }
        if (body && typeof body === 'object' && body.changes && typeof body.changes === 'object') {
          const { patchScheduleSchema } = await import('@vacps/contracts');
          return json(await services.schedules.patch(id, patchScheduleSchema.parse(body)));
        }
        return json(await services.schedules.update(id, updateScheduleSchema.parse(body)));
      }
      if (id && !action && request.method === 'DELETE') {
        const result = await services.schedules.delete(id);
        return json(result);
      }
      if (id && action === 'run' && request.method === 'POST') {
        const body = (await readJson(request).catch(() => ({}))) as {
          idempotency_key?: string;
        };
        return json(
          await services.schedules.runNow(id, {
            ...(body.idempotency_key ? { idempotencyKey: body.idempotency_key } : {}),
          }),
          { status: 202 },
        );
      }
      if (!id && action === 'reconcile' && request.method === 'POST')
        return json(await services.schedules.reconcile());
    }

    return json(
      { error: { code: 'not_found', message: 'API route not found.', requestId } },
      { status: 404 },
    );
  } catch (error) {
    const response = errorResponse(error, requestId);
    if (new URL(request.url).pathname.startsWith('/api/auth/')) {
      response.headers.set('cache-control', 'no-store');
    }
    return response;
  }
}

async function handleAuth(
  request: Request,
  env: Env,
  action: string | undefined,
): Promise<Response> {
  if (action === 'session' && request.method === 'GET') {
    await requireAuthenticated(request, env);
    return authJson({ authenticated: true });
  }
  if (action === 'login' && request.method === 'POST') {
    requireSameOrigin(request);
    let password: string | undefined;
    try {
      const input = await request.json();
      password =
        input &&
        typeof input === 'object' &&
        typeof (input as { password?: unknown }).password === 'string'
          ? (input as { password: string }).password
          : undefined;
    } catch {
      // Invalid credentials intentionally use the same generic response.
    }
    if (!password || !(await passwordMatches(password, env))) {
      throw new AppError('invalid_credentials', 'Invalid credentials.', 401);
    }
    return authJson(
      { authenticated: true },
      { headers: { 'set-cookie': await createSessionCookie(env) } },
    );
  }
  if (action === 'logout' && request.method === 'POST') {
    await requireAuthenticated(request, env);
    requireSameOrigin(request);
    return new Response(null, {
      status: 204,
      headers: { 'cache-control': 'no-store', 'set-cookie': clearSessionCookie() },
    });
  }
  return json(
    { error: { code: 'not_found', message: 'API route not found.' } },
    { status: 404, headers: { 'cache-control': 'no-store' } },
  );
}

function authJson(data: unknown, init: ResponseInit = {}): Response {
  const headers = new Headers(init.headers);
  headers.set('cache-control', 'no-store');
  return json(data, { ...init, headers });
}

function isStateChanging(request: Request): boolean {
  return ['POST', 'PATCH', 'DELETE'].includes(request.method);
}

function requireSameOrigin(request: Request): void {
  const origin = request.headers.get('origin');
  if (origin !== new URL(request.url).origin) {
    throw new AppError('invalid_origin', 'Request origin is not allowed.', 403);
  }
}

function registrationTokenFromRequest(request: Request): string | undefined {
  const token = request.headers
    .get('authorization')
    ?.match(/^Bearer\s+(.+)$/i)?.[1]
    ?.trim();
  return token || undefined;
}

async function readSignedJson<T>(
  request: Request,
  schema: { parse(input: unknown): T },
): Promise<{ input: T; body: string }> {
  const body = await request.clone().text();
  try {
    return { input: schema.parse(JSON.parse(body)), body };
  } catch (error) {
    if (error instanceof z.ZodError)
      throw new AppError('invalid_request', error.issues[0]?.message ?? 'Invalid request.', 400);
    throw new AppError('invalid_json', 'Request body must be valid JSON.', 400);
  }
}

function inspectNode(
  registration: BackendRegistration,
  backend: Backend | undefined,
  intervalSeconds: number,
): {
  registration: BackendRegistration;
  backend?: Backend;
  status?: BackendStatus;
  online: boolean;
  checkedAt: string;
} {
  const checkedAt = backend?.lastCheckedAt ?? registration.updatedAt;
  const lastChecked = Date.parse(checkedAt);
  const fresh =
    Number.isFinite(lastChecked) && Date.now() - lastChecked <= intervalSeconds * 3 * 1000;
  const status = backend?.lastStatus;
  return {
    registration,
    ...(backend ? { backend } : {}),
    ...(status ? { status } : {}),
    online: registration.status === 'approved' && Boolean(status?.health.ok) && fresh,
    checkedAt,
  };
}

function registrationNetwork(request: Request): { ip?: string; ips?: string[]; location?: string } {
  const ip = request.headers.get('cf-connecting-ip') ?? undefined;
  const cf = request.cf as unknown as Record<string, unknown> | undefined;
  const city = typeof cf?.city === 'string' ? cf.city : undefined;
  const country = typeof cf?.country === 'string' ? cf.country : undefined;
  const location = [city, country].filter(Boolean).join(', ') || undefined;
  return {
    ...(ip ? { ip, ips: [ip] } : {}),
    ...(location ? { location } : {}),
  };
}
