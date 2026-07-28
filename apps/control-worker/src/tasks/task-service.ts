import {
  taskSummary,
  type CreateTaskInput,
  type TaskSource,
  type TaskStatus,
} from '@vacps/contracts';

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
  idempotency_key: string | null;
  retry_of_task_id: string | null;
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
  idempotencyKey?: string;
  retryOfTaskId?: string;
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
  ): Promise<TaskIndex & { reusedExistingTask?: boolean }> {
    const backend = await this.backends.get(input.backendId);
    if (!backend.enabled)
      throw new AppError('backend_disabled', `Backend '${backend.id}' is disabled.`, 409);

    if (input.idempotencyKey) {
      const existing = await this.findByIdempotency(input.backendId, input.idempotencyKey);
      if (existing) return { ...existing, reusedExistingTask: true };
    }

    const taskId = crypto.randomUUID();
    const now = new Date().toISOString();
    const summary = taskSummary(input);
    try {
      await this.db
        .prepare(
          `INSERT INTO tasks (
             id, backend_id, type, source, profile, summary, status, schedule_id,
             idempotency_key, created_at, updated_at
           ) VALUES (?, ?, ?, ?, ?, ?, 'created', ?, ?, ?, ?)`,
        )
        .bind(
          taskId,
          input.backendId,
          input.type,
          source,
          input.profile,
          summary,
          scheduleId ?? null,
          input.idempotencyKey ?? null,
          now,
          now,
        )
        .run();
    } catch {
      if (input.idempotencyKey) {
        const raced = await this.findByIdempotency(input.backendId, input.idempotencyKey);
        if (raced) return { ...raced, reusedExistingTask: true };
      }
      throw new AppError('internal_error', 'Could not create task index.', 500);
    }

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

  async detail(
    id: string,
    options: {
      includeCommands?: boolean;
      includeOutputPreview?: boolean;
      previewMaxBytes?: number;
    } = {},
  ): Promise<{
    task: TaskIndex;
    remote?: unknown;
    result?: unknown;
    output?: unknown;
    commands?: unknown;
  }> {
    const task = await this.get(id);
    const backend = await this.backends.get(task.backendId);
    const previewMaxBytes = options.previewMaxBytes ?? 8192;
    try {
      const remote = (await this.client.getTask(backend, id)) as {
        status?: unknown;
        result?: unknown;
        task?: { status?: unknown };
      };
      const remoteStatus = extractStatus(remote) ?? extractStatus(remote.task);
      if (remoteStatus && remoteStatus !== task.status) await this.setStatus(id, remoteStatus);
      const refreshed = await this.get(id);
      let commands: unknown;
      let output: unknown;
      if (options.includeCommands || options.includeOutputPreview) {
        const logs = (await this.client.getLogs(backend, id, {
          previewMaxBytes,
        })) as {
          commands?: Array<Record<string, unknown>>;
        };
        commands = options.includeCommands ? logs.commands : undefined;
        if (options.includeOutputPreview) {
          const last = Array.isArray(logs.commands) ? logs.commands.at(-1) : undefined;
          output = {
            stdout: streamPreview(last, 'stdout', previewMaxBytes),
            stderr: streamPreview(last, 'stderr', previewMaxBytes),
          };
        }
      }
      return {
        task: refreshed,
        remote,
        ...(remote?.result !== undefined ? { result: remote.result } : {}),
        ...(output ? { output } : {}),
        ...(commands ? { commands } : {}),
      };
    } catch (error) {
      return {
        task,
        remote: {
          unavailable: true,
          message: error instanceof Error ? error.message : String(error),
        },
      };
    }
  }

  async readOutput(
    id: string,
    input: { stream: 'stdout' | 'stderr'; offset?: number; maxBytes?: number },
  ): Promise<unknown> {
    const task = await this.get(id);
    const backend = await this.backends.get(task.backendId);
    return this.client.getLogs(backend, id, {
      stream: input.stream,
      offset: input.offset ?? 0,
      maxBytes: input.maxBytes ?? 65_536,
    });
  }

  async logs(id: string): Promise<unknown> {
    const task = await this.get(id);
    return this.client.getLogs(await this.backends.get(task.backendId), id);
  }

  async cancel(id: string): Promise<unknown> {
    const task = await this.get(id);
    if (
      ['succeeded', 'failed', 'cancelled', 'timed_out', 'dispatch_failed'].includes(task.status)
    ) {
      throw new AppError(
        'task_state_conflict',
        `Task '${id}' is ${task.status} and cannot be cancelled.`,
        409,
      );
    }
    const result = await this.client.cancelTask(await this.backends.get(task.backendId), id);
    await this.setStatus(id, 'cancelled');
    return { task: await this.get(id), result };
  }

  async retry(id: string): Promise<unknown> {
    const original = await this.get(id);
    const backend = await this.backends.get(original.backendId);
    const result = await this.client.retryTask(backend, id);
    await this.setStatus(id, 'queued');
    return { task: original, result, retry_of_task_id: id };
  }

  private async findByIdempotency(
    backendId: string,
    idempotencyKey: string,
  ): Promise<TaskIndex | undefined> {
    const row = await this.db
      .prepare('SELECT * FROM tasks WHERE backend_id = ? AND idempotency_key = ? LIMIT 1')
      .bind(backendId, idempotencyKey)
      .first<TaskRow>();
    return row ? toTaskIndex(row) : undefined;
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
    ...(row.idempotency_key ? { idempotencyKey: row.idempotency_key } : {}),
    ...(row.retry_of_task_id ? { retryOfTaskId: row.retry_of_task_id } : {}),
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

function streamPreview(
  command: Record<string, unknown> | undefined,
  stream: 'stdout' | 'stderr',
  previewMaxBytes: number,
) {
  const preview = typeof command?.[stream] === 'string' ? (command[stream] as string) : '';
  const bytesKey = `${stream}_bytes`;
  const truncatedKey = `${stream}_truncated`;
  const completeKey = `${stream}_complete`;
  const totalBytes =
    typeof command?.[bytesKey] === 'number'
      ? (command[bytesKey] as number)
      : new TextEncoder().encode(preview).byteLength;
  const truncated =
    typeof command?.[truncatedKey] === 'boolean'
      ? (command[truncatedKey] as boolean)
      : totalBytes > previewMaxBytes;
  const complete =
    typeof command?.[completeKey] === 'boolean' ? (command[completeKey] as boolean) : true;
  return {
    available: Boolean(command),
    bytes: totalBytes,
    complete,
    truncated,
    preview: preview.slice(0, previewMaxBytes),
    next_offset: truncated ? previewMaxBytes : null,
  };
}
