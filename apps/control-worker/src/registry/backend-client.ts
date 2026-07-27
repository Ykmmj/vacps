import type {
  Backend,
  BackendHealth,
  BackendMetrics,
  BackendStatusResponse,
} from '@vps-agent/contracts';

import { AppError } from '../lib/http.js';
import { createControlPlaneSignatureHeaders } from '../security/request-signatures.js';

export class BackendClient {
  constructor(private readonly controlPlaneSigningPrivateKey: string | undefined) {}

  async health(backend: Pick<Backend, 'baseUrl'>): Promise<BackendHealth> {
    return this.request(backend, '/health', { method: 'GET' }) as Promise<BackendHealth>;
  }

  async status(backend: Pick<Backend, 'baseUrl'>): Promise<BackendStatusResponse> {
    const [health, metrics] = await Promise.all([
      this.request(backend, '/health', { method: 'GET' }),
      this.request(backend, '/metrics', { method: 'GET' }),
    ]);
    return {
      health: health as BackendHealth,
      ...(metrics ? { metrics: metrics as BackendMetrics } : {}),
    };
  }

  async createTask(backend: Pick<Backend, 'baseUrl'>, task: unknown): Promise<unknown> {
    return this.request(backend, '/tasks', { method: 'POST', body: JSON.stringify(task) });
  }

  async getTask(backend: Pick<Backend, 'baseUrl'>, taskId: string): Promise<unknown> {
    return this.request(backend, `/tasks/${encodeURIComponent(taskId)}`, { method: 'GET' });
  }

  async getLogs(backend: Pick<Backend, 'baseUrl'>, taskId: string): Promise<unknown> {
    return this.request(backend, `/tasks/${encodeURIComponent(taskId)}/logs`, { method: 'GET' });
  }

  async cancelTask(backend: Pick<Backend, 'baseUrl'>, taskId: string): Promise<unknown> {
    return this.request(backend, `/tasks/${encodeURIComponent(taskId)}/cancel`, { method: 'POST' });
  }

  async retryTask(backend: Pick<Backend, 'baseUrl'>, taskId: string): Promise<unknown> {
    return this.request(backend, `/tasks/${encodeURIComponent(taskId)}/retry`, { method: 'POST' });
  }

  async upsertScheduler(
    backend: Pick<Backend, 'baseUrl'>,
    id: string,
    schedule: unknown,
  ): Promise<void> {
    await this.request(backend, `/schedulers/${encodeURIComponent(id)}`, {
      method: 'PUT',
      body: JSON.stringify(schedule),
    });
  }

  async deleteScheduler(backend: Pick<Backend, 'baseUrl'>, id: string): Promise<void> {
    await this.request(backend, `/schedulers/${encodeURIComponent(id)}`, { method: 'DELETE' });
  }

  async runSchedule(
    backend: Pick<Backend, 'baseUrl'>,
    id: string,
    taskTemplate: unknown,
  ): Promise<unknown> {
    return this.request(backend, `/schedulers/${encodeURIComponent(id)}/run`, {
      method: 'POST',
      body: JSON.stringify({ taskTemplate }),
    });
  }

  private async request(
    backend: Pick<Backend, 'baseUrl'>,
    path: string,
    init: RequestInit,
  ): Promise<unknown> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10_000);
    try {
      const body = typeof init.body === 'string' ? init.body : '';
      const request = new Request(`${backend.baseUrl}${path}`, { method: init.method ?? 'GET' });
      const signatureHeaders = await createControlPlaneSignatureHeaders(
        this.controlPlaneSigningPrivateKey,
        request,
        body,
      );
      const response = await fetch(request, {
        ...init,
        signal: controller.signal,
        headers: {
          'content-type': 'application/json',
          ...signatureHeaders,
          'x-request-id': crypto.randomUUID(),
        },
      });
      const responseBody = await response.json().catch(() => undefined);
      if (!response.ok) {
        throw new AppError(
          'backend_request_failed',
          `Backend returned HTTP ${response.status}.`,
          502,
        );
      }
      return responseBody;
    } catch (error: unknown) {
      if (error instanceof AppError) throw error;
      throw new AppError(
        'backend_unreachable',
        `Could not reach backend: ${error instanceof Error ? error.message : String(error)}`,
        502,
      );
    } finally {
      clearTimeout(timer);
    }
  }
}
