import type {
  Backend,
  BackendHealth,
  BackendMetrics,
  BackendStatusResponse,
} from '@vacps/contracts';

import { AppError } from '../lib/http.js';
import { createControlPlaneSignatureHeaders } from '../security/request-signatures.js';

/**
 * Target backend for control → agent calls. Audience id is signed explicitly
 * (x-vps-control-backend-id) so a shared control key cannot cross-replay.
 * Accepts Backend (`id`) or registration-shaped (`backendId`) targets.
 */
export type BackendRequestTarget =
  Pick<Backend, 'id' | 'baseUrl'> | { backendId: string; baseUrl: string };

/** Contract: Narrow — caller supplies a typed BackendRequestTarget discriminant. */
function backendAudienceId(backend: BackendRequestTarget): string {
  return 'id' in backend ? backend.id : backend.backendId;
}

export class BackendClient {
  constructor(private readonly controlPlaneSigningPrivateKey: string | undefined) {}

  async health(backend: BackendRequestTarget): Promise<BackendHealth> {
    return this.request(backend, '/health', { method: 'GET' }) as Promise<BackendHealth>;
  }

  async status(backend: BackendRequestTarget): Promise<BackendStatusResponse> {
    const [health, metrics] = await Promise.all([
      this.request(backend, '/health', { method: 'GET' }),
      this.request(backend, '/metrics', { method: 'GET' }),
    ]);
    return {
      health: health as BackendHealth,
      ...(metrics ? { metrics: metrics as BackendMetrics } : {}),
    };
  }

  async createTask(backend: BackendRequestTarget, task: unknown): Promise<unknown> {
    return this.request(backend, '/tasks', { method: 'POST', body: JSON.stringify(task) });
  }

  async getTask(backend: BackendRequestTarget, taskId: string): Promise<unknown> {
    return this.request(backend, `/tasks/${encodeURIComponent(taskId)}`, { method: 'GET' });
  }

  async getLogs(
    backend: BackendRequestTarget,
    taskId: string,
    query: {
      stream?: 'stdout' | 'stderr';
      offset?: number;
      maxBytes?: number;
      previewMaxBytes?: number;
      expectedStreamVersion?: string;
    } = {},
  ): Promise<unknown> {
    const params = new URLSearchParams();
    if (query.stream) params.set('stream', query.stream);
    if (query.offset !== undefined) params.set('offset', String(query.offset));
    if (query.maxBytes !== undefined) params.set('max_bytes', String(query.maxBytes));
    if (query.previewMaxBytes !== undefined)
      params.set('preview_max_bytes', String(query.previewMaxBytes));
    if (query.expectedStreamVersion)
      params.set('expected_stream_version', query.expectedStreamVersion);
    const suffix = params.size > 0 ? `?${params.toString()}` : '';
    return this.request(backend, `/tasks/${encodeURIComponent(taskId)}/logs${suffix}`, {
      method: 'GET',
    });
  }

  async readFile(
    backend: BackendRequestTarget,
    input: {
      path: string;
      startLine?: number | undefined;
      endLine?: number | undefined;
      maxBytes?: number | undefined;
      encoding?: 'utf-8' | 'base64' | undefined;
    },
  ): Promise<unknown> {
    const params = new URLSearchParams({ path: input.path });
    if (input.startLine !== undefined) params.set('start_line', String(input.startLine));
    if (input.endLine !== undefined) params.set('end_line', String(input.endLine));
    if (input.maxBytes !== undefined) params.set('max_bytes', String(input.maxBytes));
    if (input.encoding) params.set('encoding', input.encoding);
    return this.request(backend, `/fs/read?${params.toString()}`, { method: 'GET' });
  }

  async statFile(backend: BackendRequestTarget, path: string): Promise<unknown> {
    return this.request(backend, `/fs/stat?${new URLSearchParams({ path })}`, { method: 'GET' });
  }

  async listDir(
    backend: BackendRequestTarget,
    input: { path: string; limit?: number; includeHidden?: boolean; cursor?: string },
  ): Promise<unknown> {
    const params = new URLSearchParams({ path: input.path });
    if (input.limit !== undefined) params.set('limit', String(input.limit));
    if (input.includeHidden) params.set('include_hidden', 'true');
    if (input.cursor) params.set('cursor', input.cursor);
    return this.request(backend, `/fs/list?${params.toString()}`, { method: 'GET' });
  }

  async glob(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    return this.request(backend, '/fs/glob', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: 60_000,
    });
  }

  async grep(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    return this.request(backend, '/fs/grep', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: 60_000,
    });
  }

  async editFile(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    return this.request(backend, '/fs/edit', { method: 'POST', body: JSON.stringify(body) });
  }

  async writeFile(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    return this.request(backend, '/fs/write', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: 60_000,
    });
  }

  async applyPatch(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    return this.request(backend, '/fs/apply_patch', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: 60_000,
    });
  }

  async moveFile(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    return this.request(backend, '/fs/move', { method: 'POST', body: JSON.stringify(body) });
  }

  async deleteFile(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    return this.request(backend, '/fs/delete', { method: 'POST', body: JSON.stringify(body) });
  }

  async mkdir(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    return this.request(backend, '/fs/mkdir', { method: 'POST', body: JSON.stringify(body) });
  }

  async getCapabilities(backend: BackendRequestTarget): Promise<unknown> {
    return this.request(backend, '/capabilities', { method: 'GET' });
  }

  async execCommand(
    backend: BackendRequestTarget,
    body: Record<string, unknown>,
  ): Promise<unknown> {
    const yieldMs = typeof body.yield_time_ms === 'number' ? body.yield_time_ms : 10_000;
    return this.request(backend, '/exec/command', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: Math.min(yieldMs + 5_000, 125_000),
    });
  }

  async execShell(backend: BackendRequestTarget, body: Record<string, unknown>): Promise<unknown> {
    const yieldMs = typeof body.yield_time_ms === 'number' ? body.yield_time_ms : 10_000;
    return this.request(backend, '/exec/shell', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: Math.min(yieldMs + 5_000, 125_000),
    });
  }

  async processStartCommand(
    backend: BackendRequestTarget,
    body: Record<string, unknown>,
  ): Promise<unknown> {
    return this.request(backend, '/process/start_command', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: 30_000,
    });
  }

  async processStartShell(
    backend: BackendRequestTarget,
    body: Record<string, unknown>,
  ): Promise<unknown> {
    return this.request(backend, '/process/start_shell', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: 30_000,
    });
  }

  async processRead(
    backend: BackendRequestTarget,
    body: Record<string, unknown>,
  ): Promise<unknown> {
    const waitMs = typeof body.wait_ms === 'number' ? body.wait_ms : 0;
    return this.request(backend, '/process/read', {
      method: 'POST',
      body: JSON.stringify(body),
      timeoutMs: Math.min(waitMs + 5_000, 70_000),
    });
  }

  async processWrite(
    backend: BackendRequestTarget,
    body: Record<string, unknown>,
  ): Promise<unknown> {
    return this.request(backend, '/process/write', { method: 'POST', body: JSON.stringify(body) });
  }

  async processTerminate(
    backend: BackendRequestTarget,
    body: Record<string, unknown>,
  ): Promise<unknown> {
    return this.request(backend, '/process/terminate', {
      method: 'POST',
      body: JSON.stringify(body),
    });
  }

  async cancelTask(backend: BackendRequestTarget, taskId: string): Promise<unknown> {
    // Always send JSON body so Content-Type: application/json is valid for the agent parser.
    return this.request(backend, `/tasks/${encodeURIComponent(taskId)}/cancel`, {
      method: 'POST',
      body: JSON.stringify({}),
    });
  }

  async retryTask(backend: BackendRequestTarget, taskId: string): Promise<unknown> {
    return this.request(backend, `/tasks/${encodeURIComponent(taskId)}/retry`, {
      method: 'POST',
      body: JSON.stringify({}),
    });
  }

  async upsertScheduler(
    backend: BackendRequestTarget,
    id: string,
    schedule: unknown,
  ): Promise<void> {
    await this.request(backend, `/schedulers/${encodeURIComponent(id)}`, {
      method: 'PUT',
      body: JSON.stringify(schedule),
    });
  }

  async deleteScheduler(backend: BackendRequestTarget, id: string): Promise<void> {
    await this.request(backend, `/schedulers/${encodeURIComponent(id)}`, { method: 'DELETE' });
  }

  async runSchedule(backend: BackendRequestTarget, id: string, task: unknown): Promise<unknown> {
    return this.request(backend, `/schedulers/${encodeURIComponent(id)}/run`, {
      method: 'POST',
      body: JSON.stringify({ task }),
    });
  }

  private async request(
    backend: BackendRequestTarget,
    path: string,
    init: RequestInit & { timeoutMs?: number },
  ): Promise<unknown> {
    const controller = new AbortController();
    const timeoutMs = init.timeoutMs ?? 12_000;
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    const targetUrl = joinBackendUrl(backend.baseUrl, path);
    const backendId = backendAudienceId(backend);
    try {
      const body = typeof init.body === 'string' ? init.body : '';
      const request = new Request(targetUrl, { method: init.method ?? 'GET' });
      const signatureHeaders = await createControlPlaneSignatureHeaders(
        this.controlPlaneSigningPrivateKey,
        request,
        body,
        backendId,
      );
      const { timeoutMs: _ignored, ...fetchInit } = init;
      const response = await fetch(request, {
        ...fetchInit,
        signal: controller.signal,
        headers: {
          'content-type': 'application/json',
          ...signatureHeaders,
          'x-request-id': crypto.randomUUID(),
        },
      });
      const responseBody = (await response.json().catch(() => undefined)) as
        | {
            error?: {
              message?: string;
              code?: string;
              current_stream_version?: string;
              details?: Record<string, unknown>;
            };
          }
        | undefined;
      if (!response.ok) {
        const err = responseBody?.error;
        const detail = err?.message;
        const code =
          err?.code ??
          (response.status === 404
            ? 'backend_not_found'
            : response.status >= 400 && response.status < 500
              ? 'backend_client_error'
              : 'backend_request_failed');
        const details: Record<string, unknown> = {
          backend_status: response.status,
          backend_url: targetUrl,
          ...(err?.code ? { backend_code: err.code } : {}),
          ...(err?.message ? { backend_message: err.message } : {}),
          ...(err?.details ?? {}),
          ...(err?.current_stream_version
            ? { current_stream_version: err.current_stream_version }
            : {}),
        };
        throw new AppError(
          code,
          detail ?? describeBackendHttpFailure(response.status, targetUrl),
          response.status >= 400 && response.status < 500 ? response.status : 502,
          details,
        );
      }
      return responseBody;
    } catch (error: unknown) {
      if (error instanceof AppError) throw error;
      throw new AppError(
        'backend_unreachable',
        `Could not reach backend at ${targetUrl}: ${error instanceof Error ? error.message : String(error)}`,
        502,
      );
    } finally {
      clearTimeout(timer);
    }
  }
}

function joinBackendUrl(baseUrl: string, path: string): string {
  return new URL(path.startsWith('/') ? path : `/${path}`, ensureTrailingSlash(baseUrl)).toString();
}

function ensureTrailingSlash(value: string): string {
  return value.endsWith('/') ? value : `${value}/`;
}

function describeBackendHttpFailure(status: number, targetUrl: string): string {
  const host = safeHost(targetUrl);
  if (status === 530 || status === 1033) {
    return (
      `Cloudflare could not reach the Agent origin via Tunnel (HTTP ${status}` +
      `${host ? ` at ${host}` : ''}). ` +
      'On the VPS run: systemctl status vacps vacps-tunnel; confirm PUBLIC_BASE_URL matches the Tunnel hostname; ' +
      'wait until the Tunnel is healthy, then approve again.'
    );
  }
  if (status === 502 || status === 503 || status === 504) {
    return (
      `Agent origin is not healthy (HTTP ${status}${host ? ` at ${host}` : ''}). ` +
      'Confirm vacps is listening on 127.0.0.1:3100 and vacps-tunnel is active.'
    );
  }
  if (status === 401 || status === 403) {
    return (
      `Agent rejected the control-plane request (HTTP ${status}${host ? ` at ${host}` : ''}). ` +
      'Re-run install/upgrade so CONTROL_PLANE_PUBLIC_KEY matches the Worker signing identity.'
    );
  }
  return `Backend returned HTTP ${status}${host ? ` for ${host}` : ''}.`;
}

function safeHost(url: string): string | undefined {
  try {
    return new URL(url).host;
  } catch {
    return undefined;
  }
}
