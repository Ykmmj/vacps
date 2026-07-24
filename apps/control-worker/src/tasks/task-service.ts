import type { CreateTaskInput, TaskSource, TaskStatus } from '@vps-agent/contracts';

import { AppError } from '../lib/http.js';
import type { BackendClient } from '../registry/backend-client.js';
import type { BackendRepository } from '../registry/repository.js';

interface TaskRow {
  id: string;
  backend_id: string;
  type: 'shell' | 'agent';
  source: TaskSource;
  profile: string;
  summary: string | null;
  status: TaskStatus;
  schedule_id: string | null;
  created_at: string;
  updated_at: string;
  finished_at: string | null;
}

export interface TaskIndex {
  id: string;
  backendId: string;
  type: 'shell' | 'agent';
  source: TaskSource;
  profile: string;
  summary?: string;
  status: TaskStatus;
  scheduleId?: string;
  createdAt: string;
  updatedAt: string;
  finishedAt?: string;
}

export class TaskService {
  constructor(
    private readonly db: D1Database,
    private readonly backends: BackendRepository,
    private readonly client: BackendClient,
  ) {}

  async create(
    input: CreateTaskInput,
    source: TaskSource,
    scheduleId?: string,
  ): Promise<TaskIndex> {
    const backend = await this.backends.get(input.backendId);
    if (!backend.enabled)
      throw new AppError('backend_disabled', `Backend '${backend.id}' is disabled.`, 409);
    const taskId = crypto.randomUUID();
    const now = new Date().toISOString();
    const summary = summarize(input.type === 'shell' ? input.command : input.prompt);
    await this.db
      .prepare(
        `INSERT INTO tasks (id, backend_id, type, source, profile, summary, status, schedule_id, created_at, updated_at)
         VALUES (?, ?, ?, ?, ?, ?, 'created', ?, ?, ?)`,
      )
      .bind(
        taskId,
        input.backendId,
        input.type,
        source,
        input.profile,
        summary,
        scheduleId ?? null,
        now,
        now,
      )
      .run();
    await this.setStatus(taskId, 'dispatching');
    try {
      await this.client.createTask(backend, {
        ...input,
        taskId,
        source,
        ...(scheduleId ? { scheduleId } : {}),
      });
      await this.setStatus(taskId, 'queued');
    } catch (error) {
      await this.setStatus(taskId, 'dispatch_failed');
      throw error;
    }
    return this.get(taskId);
  }

  async list(limit = 50): Promise<TaskIndex[]> {
    const rows = await this.db
      .prepare('SELECT * FROM tasks ORDER BY created_at DESC LIMIT ?')
      .bind(Math.min(Math.max(limit, 1), 200))
      .all<TaskRow>();
    return rows.results.map(toTaskIndex);
  }

  async get(id: string): Promise<TaskIndex> {
    const row = await this.db.prepare('SELECT * FROM tasks WHERE id = ?').bind(id).first<TaskRow>();
    if (!row) throw new AppError('task_not_found', `Task '${id}' was not found.`, 404);
    return toTaskIndex(row);
  }

  async detail(id: string): Promise<unknown> {
    const task = await this.get(id);
    const backend = await this.backends.get(task.backendId);
    try {
      const remote = await this.client.getTask(backend, id);
      const remoteStatus = extractStatus(remote);
      if (remoteStatus && remoteStatus !== task.status) await this.setStatus(id, remoteStatus);
      return { task: await this.get(id), backend: remote };
    } catch (error) {
      return {
        task,
        backend: {
          unavailable: true,
          message: error instanceof Error ? error.message : String(error),
        },
      };
    }
  }

  async logs(id: string): Promise<unknown> {
    const task = await this.get(id);
    return this.client.getLogs(await this.backends.get(task.backendId), id);
  }

  async cancel(id: string): Promise<unknown> {
    const task = await this.get(id);
    const result = await this.client.cancelTask(await this.backends.get(task.backendId), id);
    return { task: await this.get(id), result };
  }

  async retry(id: string): Promise<unknown> {
    const task = await this.get(id);
    const result = await this.client.retryTask(await this.backends.get(task.backendId), id);
    await this.setStatus(id, 'queued');
    return { task: await this.get(id), result };
  }

  private async setStatus(id: string, status: TaskStatus): Promise<void> {
    const now = new Date().toISOString();
    const finished = ['succeeded', 'failed', 'cancelled', 'timed_out', 'dispatch_failed'].includes(
      status,
    )
      ? now
      : null;
    await this.db
      .prepare(
        'UPDATE tasks SET status = ?, updated_at = ?, finished_at = COALESCE(?, finished_at) WHERE id = ?',
      )
      .bind(status, now, finished, id)
      .run();
  }
}

function summarize(value: string): string {
  return value.replace(/\s+/g, ' ').slice(0, 240);
}

function toTaskIndex(row: TaskRow): TaskIndex {
  return {
    id: row.id,
    backendId: row.backend_id,
    type: row.type,
    source: row.source,
    profile: row.profile,
    ...(row.summary ? { summary: row.summary } : {}),
    status: row.status,
    ...(row.schedule_id ? { scheduleId: row.schedule_id } : {}),
    createdAt: row.created_at,
    updatedAt: row.updated_at,
    ...(row.finished_at ? { finishedAt: row.finished_at } : {}),
  };
}

function extractStatus(value: unknown): TaskStatus | undefined {
  if (!value || typeof value !== 'object') return undefined;
  const status = (value as { status?: unknown }).status;
  const valid: TaskStatus[] = [
    'created',
    'dispatching',
    'queued',
    'running',
    'waiting_for_approval',
    'succeeded',
    'failed',
    'cancelled',
    'timed_out',
    'dispatch_failed',
  ];
  return typeof status === 'string' && valid.includes(status as TaskStatus)
    ? (status as TaskStatus)
    : undefined;
}
