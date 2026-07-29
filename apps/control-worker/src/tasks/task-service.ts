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
  type: string;
  kind: string | null;
  source: TaskSource;
  profile: string;
  name: string | null;
  summary: string | null;
  status: TaskStatus;
  schedule_id: string | null;
  idempotency_key: string | null;
  request_hash: string | null;
  retry_of_task_id: string | null;
  created_at: string;
  updated_at: string;
  finished_at: string | null;
}

export interface TaskIndex {
  id: string;
  backendId: string;
  /** Schema v3 kind: command | shell | agent. */
  kind: string;
  source: TaskSource;
  profile: string;
  name?: string;
  summary?: string;
  status: TaskStatus;
  scheduleId?: string;
  idempotencyKey?: string;
  requestHash?: string;
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
    _publicKind?: 'command' | 'shell' | 'agent',
  ): Promise<
    TaskIndex & {
      reusedExistingTask?: boolean;
      requestHash?: string;
    }
  > {
    const backend = await this.backends.get(input.backend_id);
    if (!backend.enabled)
      throw new AppError('backend_disabled', `Backend '${backend.id}' is disabled.`, 409);

    const kind = input.kind;
    // Preflight agent capability before writing an index row (avoids orphan dispatch_failed).
    if (kind === 'agent') {
      try {
        const caps = (await this.client.getCapabilities(backend)) as {
          features?: { pi?: { available?: boolean }; agent?: boolean };
          pi?: { available?: boolean };
        };
        const pi =
          caps.pi ??
          (caps.features as { pi?: { available?: boolean } } | undefined)?.pi ??
          null;
        const available =
          typeof pi === 'object' && pi && 'available' in pi
            ? Boolean(pi.available)
            : caps.features?.agent === true;
        // If capability payload is present and pi is explicitly unavailable, fail early.
        if (pi && typeof pi === 'object' && pi.available === false) {
          throw new AppError(
            'capability_unavailable',
            'Agent runtime (Pi) is not available on this backend.',
            409,
            { capability: 'agent', pi: { available: false } },
          );
        }
        void available;
      } catch (error) {
        if (error instanceof AppError && error.code === 'capability_unavailable') throw error;
        // If capabilities endpoint fails, fall through to dispatch (agent will reject).
      }
    }
    const requestHash = await hashTaskRequest(input);
    if (input.idempotency_key) {
      const existing = await this.findByIdempotency(input.backend_id, input.idempotency_key);
      if (existing) {
        if (existing.requestHash && existing.requestHash !== requestHash) {
          throw new AppError(
            'idempotency_conflict',
            'The idempotency key was previously used with different arguments.',
            409,
          );
        }
        return {
          ...existing,
          reusedExistingTask: true,
          requestHash: existing.requestHash ?? requestHash,
        };
      }
    }

    const taskId = crypto.randomUUID();
    const now = new Date().toISOString();
    const summary = taskSummary(input);
    const name = input.name?.trim() || null;
    try {
      await this.db
        .prepare(
          `INSERT INTO tasks (
             id, backend_id, type, kind, source, profile, name, summary, status, schedule_id,
             idempotency_key, request_hash, created_at, updated_at
           ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'created', ?, ?, ?, ?, ?)`,
        )
        .bind(
          taskId,
          input.backend_id,
          // D1 CHECK still allows only shell|agent on `type`; public kind is `command|shell|agent`.
          kind === 'agent' ? 'agent' : 'shell',
          kind,
          source,
          input.profile,
          name,
          summary,
          scheduleId ?? null,
          input.idempotency_key ?? null,
          requestHash,
          now,
          now,
        )
        .run();
    } catch (error) {
      if (input.idempotency_key) {
        const raced = await this.findByIdempotency(input.backend_id, input.idempotency_key);
        if (raced) {
          if (raced.requestHash && raced.requestHash !== requestHash) {
            throw new AppError(
              'idempotency_conflict',
              'The idempotency key was previously used with different arguments.',
              409,
            );
          }
          return {
            ...raced,
            reusedExistingTask: true,
            requestHash: raced.requestHash ?? requestHash,
          };
        }
      }
      throw new AppError(
        'internal_error',
        `Could not create task index.${error instanceof Error ? ` ${error.message}` : ''}`,
        500,
      );
    }

    await this.setStatus(taskId, 'dispatching');
    try {
      // Dispatch Schema v3 wire body as-is (snake_case + kind).
      await this.client.createTask(backend, {
        ...input,
        task_id: taskId,
        source,
        ...(scheduleId ? { schedule_id: scheduleId } : {}),
      });
      await this.setStatus(taskId, 'queued');
    } catch (error) {
      // Capability preflight failures should not leave orphan index rows.
      const isCapability =
        error instanceof AppError &&
        (error.code === 'capability_unavailable' ||
          error.details?.backend_code === 'capability_unavailable');
      if (isCapability) {
        await this.db.prepare('DELETE FROM tasks WHERE id = ?').bind(taskId).run();
        if (error instanceof AppError) {
          throw new AppError(error.code, error.message, error.status, {
            ...error.details,
            // No task_id — row was rolled back.
          });
        }
        throw error;
      }
      await this.setStatus(taskId, 'dispatch_failed');
      if (error instanceof AppError) {
        throw new AppError(error.code, error.message, error.status, {
          ...error.details,
          task_id: taskId,
        });
      }
      throw error;
    }
    const created = await this.get(taskId);
    return { ...created, requestHash };
  }

  async list(
    query:
      | number
      | {
          limit?: number;
          offset?: number;
          backendId?: string;
          type?: string;
          kind?: string;
          status?: string;
          createdAfter?: string;
        } = 50,
  ): Promise<TaskIndex[]> {
    const opts = typeof query === 'number' ? { limit: query } : query;
    const limit = Math.min(Math.max(opts.limit ?? 50, 1), 200);
    const offset = Math.max(opts.offset ?? 0, 0);
    const clauses: string[] = [];
    const binds: unknown[] = [];
    if (opts.backendId) {
      clauses.push('backend_id = ?');
      binds.push(opts.backendId);
    }
    const kindFilter = opts.kind ?? opts.type;
    if (kindFilter) {
      clauses.push('(kind = ? OR type = ?)');
      binds.push(kindFilter, kindFilter);
    }
    if (opts.status) {
      clauses.push('status = ?');
      binds.push(opts.status);
    }
    if (opts.createdAfter) {
      clauses.push('created_at >= ?');
      binds.push(opts.createdAfter);
    }
    const where = clauses.length ? `WHERE ${clauses.join(' AND ')}` : '';
    const rows = await this.db
      .prepare(`SELECT * FROM tasks ${where} ORDER BY created_at DESC LIMIT ? OFFSET ?`)
      .bind(...binds, limit, offset)
      .all<TaskRow>();
    return rows.results.map(toTaskIndex);
  }

  async listPage(
    query: {
      limit?: number;
      offset?: number;
      backendId?: string;
      type?: string;
      kind?: string;
      status?: string;
      createdAfter?: string;
    } = {},
  ): Promise<{ tasks: TaskIndex[]; returned_count: number; next_offset: number | null }> {
    const limit = Math.min(Math.max(query.limit ?? 50, 1), 200);
    const offset = Math.max(query.offset ?? 0, 0);
    const tasks = await this.list({ ...query, limit: limit + 1, offset });
    const page = tasks.slice(0, limit);
    return {
      tasks: page,
      returned_count: page.length,
      next_offset: tasks.length > limit ? offset + page.length : null,
    };
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
      let result: unknown = remote?.result;
      if (options.includeCommands || options.includeOutputPreview) {
        const logs = (await this.client.getLogs(backend, id, {
          previewMaxBytes,
        })) as {
          commands?: Array<Record<string, unknown>>;
        };
        if (options.includeCommands) {
          commands = Array.isArray(logs.commands)
            ? logs.commands.map((cmd) => normalizeCommandEntry(cmd, id))
            : [];
        }
        if (options.includeOutputPreview) {
          const last = Array.isArray(logs.commands) ? logs.commands.at(-1) : undefined;
          output = {
            stdout: streamPreview(last, 'stdout', previewMaxBytes, id),
            stderr: streamPreview(last, 'stderr', previewMaxBytes, id),
          };
          // Prefer structured process result without embedding full stdout/stderr text twice.
          if (result && typeof result === 'object') {
            const r = result as Record<string, unknown>;
            result = {
              kind: 'process',
              exit_code: r.exitCode ?? r.exit_code ?? null,
              signal: r.signal ?? null,
              timed_out: r.timedOut ?? r.timed_out ?? false,
            };
          } else if (last) {
            result = {
              kind: 'process',
              exit_code: last.exitCode ?? last.exit_code ?? null,
              signal: last.signal ?? null,
              timed_out: false,
            };
          }
        }
      }
      return {
        task: refreshed,
        remote,
        ...(result !== undefined ? { result } : {}),
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
    input: {
      stream: 'stdout' | 'stderr';
      offset?: number;
      maxBytes?: number;
      expectedStreamVersion?: string;
    },
  ): Promise<unknown> {
    const task = await this.get(id);
    const backend = await this.backends.get(task.backendId);
    try {
      const payload = (await this.client.getLogs(backend, id, {
        stream: input.stream,
        offset: input.offset ?? 0,
        maxBytes: input.maxBytes ?? 65_536,
        ...(input.expectedStreamVersion
          ? { expectedStreamVersion: input.expectedStreamVersion }
          : {}),
      })) as Record<string, unknown>;
      // Schema v3: only `content` — no data alias, no legacy normalize.
      const { data: _rejectData, ...rest } = payload;
      return {
        ...rest,
        ...(payload.content !== undefined ? { content: payload.content } : {}),
        encoding: (payload.encoding as string | undefined) ?? 'utf-8',
      };
    } catch (error) {
      if (error instanceof AppError && error.code === 'stream_version_conflict') throw error;
      // Backend may return current_stream_version in details/message body.
      throw error;
    }
  }

  async logs(id: string): Promise<unknown> {
    const task = await this.get(id);
    return this.client.getLogs(await this.backends.get(task.backendId), id);
  }

  async cancel(
    id: string,
    options: { idempotencyKey?: string } = {},
  ): Promise<{
    task: TaskIndex;
    result?: unknown;
    already_terminal?: boolean;
    cancelled?: boolean;
    idempotency?: { key: string; replayed: boolean; request_hash: string };
  }> {
    const requestHash = await hashOpaque({ operation: 'tasks.cancel', task_id: id });
    const scope = 'tasks.cancel';

    if (options.idempotencyKey) {
      const cached = await this.loadOpIdempotency(scope, options.idempotencyKey);
      if (cached) {
        if (cached.requestHash !== requestHash) {
          throw new AppError(
            'idempotency_conflict',
            'The idempotency key was previously used with different arguments.',
            409,
          );
        }
        return {
          ...(cached.result as {
            task: TaskIndex;
            result?: unknown;
            already_terminal?: boolean;
            cancelled?: boolean;
          }),
          idempotency: {
            key: options.idempotencyKey,
            replayed: true,
            request_hash: requestHash,
          },
        };
      }
    }

    const task = await this.get(id);
    let body: {
      task: TaskIndex;
      result?: unknown;
      already_terminal?: boolean;
      cancelled?: boolean;
    };
    // Natural idempotency: already terminal → stable cancelled-like result.
    if (task.status === 'cancelled') {
      body = { task, already_terminal: true, cancelled: true };
    } else if (['succeeded', 'failed', 'timed_out', 'dispatch_failed'].includes(task.status)) {
      body = { task, already_terminal: true, cancelled: false };
    } else {
      const result = (await this.client.cancelTask(
        await this.backends.get(task.backendId),
        id,
      )) as { cancelled?: boolean; state?: string };
      if (result?.cancelled !== false) {
        await this.setStatus(id, 'cancelled');
      }
      body = { task: await this.get(id), result, cancelled: result?.cancelled !== false };
    }

    if (options.idempotencyKey) {
      await this.storeOpIdempotency(scope, options.idempotencyKey, requestHash, body);
      return {
        ...body,
        idempotency: {
          key: options.idempotencyKey,
          replayed: false,
          request_hash: requestHash,
        },
      };
    }
    return body;
  }

  async retry(
    id: string,
    options: { idempotencyKey?: string } = {},
  ): Promise<{
    task: TaskIndex;
    result?: unknown;
    retry_of_task_id: string;
    idempotency?: { key: string; replayed: boolean; request_hash: string };
  }> {
    const requestHash = await hashOpaque({ operation: 'tasks.retry', task_id: id });
    const scope = 'tasks.retry';

    if (options.idempotencyKey) {
      const cached = await this.loadOpIdempotency(scope, options.idempotencyKey);
      if (cached) {
        if (cached.requestHash !== requestHash) {
          throw new AppError(
            'idempotency_conflict',
            'The idempotency key was previously used with different arguments.',
            409,
          );
        }
        return {
          ...(cached.result as {
            task: TaskIndex;
            result?: unknown;
            retry_of_task_id: string;
          }),
          idempotency: {
            key: options.idempotencyKey,
            replayed: true,
            request_hash: requestHash,
          },
        };
      }
    }

    const original = await this.get(id);
    const backend = await this.backends.get(original.backendId);
    // Agent creates a new task_id; CP records a fresh index row from that result when possible.
    const result = (await this.client.retryTask(backend, id)) as {
      task_id?: string;
      status?: string;
    };
    let body: { task: TaskIndex; result?: unknown; retry_of_task_id: string };
    if (typeof result?.task_id === 'string' && result.task_id !== id) {
      const now = new Date().toISOString();
      await this.db
        .prepare(
          `INSERT OR IGNORE INTO tasks (
             id, backend_id, type, kind, source, profile, name, summary, status, schedule_id,
             retry_of_task_id, created_at, updated_at
           ) VALUES (?, ?, ?, ?, 'mcp', ?, ?, ?, 'queued', ?, ?, ?, ?)`,
        )
        .bind(
          result.task_id,
          original.backendId,
          original.kind === 'agent' ? 'agent' : 'shell',
          original.kind,
          original.profile,
          original.name ?? null,
          original.summary ?? null,
          original.scheduleId ?? null,
          id,
          now,
          now,
        )
        .run()
        .catch(() => undefined);
      body = {
        task: await this.get(result.task_id).catch(() => ({
          ...original,
          id: result.task_id!,
          status: 'queued' as TaskStatus,
          retryOfTaskId: id,
        })),
        result,
        retry_of_task_id: id,
      };
    } else {
      await this.setStatus(id, 'queued');
      body = { task: await this.get(id), result, retry_of_task_id: id };
    }

    if (options.idempotencyKey) {
      await this.storeOpIdempotency(scope, options.idempotencyKey, requestHash, body);
      return {
        ...body,
        idempotency: {
          key: options.idempotencyKey,
          replayed: false,
          request_hash: requestHash,
        },
      };
    }
    return body;
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

  private async loadOpIdempotency(
    scope: string,
    key: string,
  ): Promise<{ requestHash: string; result: unknown } | undefined> {
    const row = await this.db
      .prepare(
        'SELECT request_hash, result_json FROM operation_idempotency WHERE scope = ? AND idempotency_key = ?',
      )
      .bind(scope, key)
      .first<{ request_hash: string; result_json: string }>();
    if (!row) return undefined;
    try {
      return { requestHash: row.request_hash, result: JSON.parse(row.result_json) };
    } catch {
      return undefined;
    }
  }

  private async storeOpIdempotency(
    scope: string,
    key: string,
    requestHash: string,
    result: unknown,
  ): Promise<void> {
    await this.db
      .prepare(
        `INSERT OR REPLACE INTO operation_idempotency
          (scope, idempotency_key, request_hash, result_json, created_at)
         VALUES (?, ?, ?, ?, ?)`,
      )
      .bind(scope, key, requestHash, JSON.stringify(result), new Date().toISOString())
      .run();
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
    kind: row.kind ?? row.type,
    source: row.source,
    profile: row.profile,
    ...(row.name ? { name: row.name } : {}),
    ...(row.summary ? { summary: row.summary } : {}),
    status: row.status,
    ...(row.schedule_id ? { scheduleId: row.schedule_id } : {}),
    ...(row.idempotency_key ? { idempotencyKey: row.idempotency_key } : {}),
    ...(row.request_hash ? { requestHash: row.request_hash } : {}),
    ...(row.retry_of_task_id ? { retryOfTaskId: row.retry_of_task_id } : {}),
    createdAt: row.created_at,
    updatedAt: row.updated_at,
    ...(row.finished_at ? { finishedAt: row.finished_at } : {}),
  };
}

/** Canonical SHA-256 of Schema v3 create payload. */
export async function hashTaskRequest(input: CreateTaskInput): Promise<string> {
  const { idempotency_key: _drop, ...rest } = input;
  return hashOpaque(rest);
}

async function hashOpaque(value: unknown): Promise<string> {
  const hex = await sha256Hex(stableStringify(value));
  return `sha256:${hex}`;
}

function stableStringify(value: unknown): string {
  if (value === null || typeof value !== 'object') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map((item) => stableStringify(item)).join(',')}]`;
  const record = value as Record<string, unknown>;
  const keys = Object.keys(record).sort();
  return `{${keys.map((key) => `${JSON.stringify(key)}:${stableStringify(record[key])}`).join(',')}}`;
}

async function sha256Hex(text: string): Promise<string> {
  const data = new TextEncoder().encode(text);
  const digest = await crypto.subtle.digest('SHA-256', data);
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, '0')).join('');
}

function utf8ByteLength(text: string): number {
  return new TextEncoder().encode(text).length;
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
  return typeof status === 'string' && (valid as string[]).includes(status)
    ? (status as TaskStatus)
    : undefined;
}

function streamPreview(
  command: Record<string, unknown> | undefined,
  stream: 'stdout' | 'stderr',
  maxBytes: number,
  taskId: string,
) {
  if (!command) {
    return {
      available: false,
      bytes: 0,
      complete: false,
      truncated: false,
      preview: '',
      resource_uri: `vacps://tasks/${taskId}/output/${stream}`,
    };
  }
  const previewKey = stream === 'stdout' ? 'stdoutPreview' : 'stderrPreview';
  const bytesKey = stream === 'stdout' ? 'stdoutBytes' : 'stderrBytes';
  const text =
    typeof command[stream] === 'string'
      ? (command[stream] as string)
      : typeof command[previewKey] === 'string'
        ? (command[previewKey] as string)
        : '';
  const bytes =
    typeof command[bytesKey] === 'number' ? (command[bytesKey] as number) : utf8ByteLength(text);
  const truncated = utf8ByteLength(text) > maxBytes || bytes > maxBytes;
  const preview =
    utf8ByteLength(text) > maxBytes
      ? new TextDecoder().decode(new TextEncoder().encode(text).slice(0, maxBytes))
      : text;
  return {
    available: Boolean(text) || bytes > 0,
    bytes,
    complete: command.status === 'succeeded' || command.status === 'failed',
    truncated,
    preview,
    resource_uri: `vacps://tasks/${taskId}/output/${stream}`,
  };
}

function normalizeCommandEntry(cmd: Record<string, unknown>, taskId: string) {
  return {
    id: cmd.id ?? null,
    sequence: cmd.sequence ?? cmd.sequenceNumber ?? null,
    command: cmd.command ?? cmd.program ?? null,
    working_directory: cmd.cwd ?? cmd.workingDirectory ?? cmd.working_directory ?? null,
    status: cmd.status ?? null,
    exit_code: cmd.exitCode ?? cmd.exit_code ?? null,
    started_at: cmd.startedAt ?? cmd.started_at ?? null,
    finished_at: cmd.finishedAt ?? cmd.finished_at ?? null,
    stdout_resource_uri: `vacps://tasks/${taskId}/output/stdout`,
    stderr_resource_uri: `vacps://tasks/${taskId}/output/stderr`,
    // Do not embed stdout/stderr body or local log paths.
  };
}
