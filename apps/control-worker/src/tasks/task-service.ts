import {
  taskSummary,
  type CreateTaskInput,
  type TaskSource,
  type TaskStatus,
} from '@vacps/contracts';

import { AppError } from '../lib/http.js';
import type { BackendClient } from '../registry/backend-client.js';
import type { BackendRepository } from '../registry/repository.js';
import {
  CLEANUP_BATCH_SIZE,
  CLEANUP_MAX_PER_RUN,
  SOFT_DELETE_GRACE_HOURS,
  addHours,
  computeExpiresAt,
  environmentFromLabels,
  isTerminalTaskStatus,
  isTestTask,
  parseLabelsJson,
  retentionClassFor,
  scopeCountAcceptable,
  type RetentionClass,
} from './retention.js';

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
  terminal_at: string | null;
  expires_at: string | null;
  labels_json: string | null;
  environment: string | null;
  retention_class: string | null;
  deleted_at: string | null;
  deleted_by: string | null;
  deletion_reason: string | null;
  cleanup_state: string | null;
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
  terminalAt?: string;
  expiresAt?: string;
  labels?: Record<string, string>;
  environment?: string;
  retentionClass?: string;
  deletedAt?: string;
  deletedBy?: string;
  deletionReason?: string;
  cleanupState?: string;
}

export type TaskListQuery = {
  limit?: number;
  offset?: number;
  backendId?: string;
  type?: string;
  kind?: string;
  status?: string;
  source?: string;
  environment?: string;
  scheduleId?: string;
  /** When true, only terminal statuses; when false, only non-terminal. */
  terminal?: boolean;
  /** Exact label key/value matches (AND). */
  labels?: Record<string, string>;
  createdAfter?: string;
  createdBefore?: string;
  terminalBefore?: string;
  expiresBefore?: string;
  /** Default false — soft-deleted rows are hidden. */
  includeDeleted?: boolean;
  /** When true, omit environment=test / test retention_class. */
  hideTest?: boolean;
};

export type CleanupFilters = {
  backendId?: string;
  scheduleId?: string;
  status?: string;
  source?: string;
  environment?: string;
  labels?: Record<string, string>;
  createdBefore?: string;
  createdAfter?: string;
  terminalBefore?: string;
  expiresBefore?: string;
  /** Only expired (expires_at <= now). */
  expiredOnly?: boolean;
  /** Only test tasks (environment/retention_class). */
  testOnly?: boolean;
  /** Include soft-deleted (for hard purge). Default false. */
  includeDeleted?: boolean;
  /** Soft-deleted before this time (hard purge grace). */
  deletedBefore?: string;
};

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
    const labels = input.labels ?? {};
    const labelsJson = Object.keys(labels).length > 0 ? JSON.stringify(labels) : null;
    const environment = environmentFromLabels(labels);
    const initialRetentionClass: RetentionClass | null = isTestTask(labels, environment)
      ? 'test'
      : null;
    try {
      await this.db
        .prepare(
          `INSERT INTO tasks (
             id, backend_id, type, kind, source, profile, name, summary, status, schedule_id,
             idempotency_key, request_hash, created_at, updated_at,
             labels_json, environment, retention_class, cleanup_state
           ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'created', ?, ?, ?, ?, ?, ?, ?, ?, 'none')`,
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
          labelsJson,
          environment,
          initialRetentionClass,
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

  async list(query: number | TaskListQuery = 50): Promise<TaskIndex[]> {
    const opts: TaskListQuery = typeof query === 'number' ? { limit: query } : query;
    const limit = Math.min(Math.max(opts.limit ?? 50, 1), 200);
    const offset = Math.max(opts.offset ?? 0, 0);
    const { clauses, binds } = buildListClauses(opts);
    const where = clauses.length ? `WHERE ${clauses.join(' AND ')}` : '';
    const rows = await this.db
      .prepare(`SELECT * FROM tasks ${where} ORDER BY created_at DESC LIMIT ? OFFSET ?`)
      .bind(...binds, limit, offset)
      .all<TaskRow>();
    return rows.results.map(toTaskIndex);
  }

  async listPage(
    query: TaskListQuery = {},
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

  async get(id: string, options: { includeDeleted?: boolean } = {}): Promise<TaskIndex> {
    const row = await this.db.prepare('SELECT * FROM tasks WHERE id = ?').bind(id).first<TaskRow>();
    if (!row) throw new AppError('task_not_found', `Task '${id}' was not found.`, 404);
    if (row.deleted_at && !options.includeDeleted) {
      throw new AppError('task_not_found', `Task '${id}' was not found.`, 404);
    }
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
    } else if (isTerminalTaskStatus(task.status)) {
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
      const labelsJson =
        original.labels && Object.keys(original.labels).length > 0
          ? JSON.stringify(original.labels)
          : null;
      await this.db
        .prepare(
          `INSERT OR IGNORE INTO tasks (
             id, backend_id, type, kind, source, profile, name, summary, status, schedule_id,
             retry_of_task_id, created_at, updated_at,
             labels_json, environment, retention_class, cleanup_state
           ) VALUES (?, ?, ?, ?, 'mcp', ?, ?, ?, 'queued', ?, ?, ?, ?, ?, ?, ?, 'none')`,
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
          labelsJson,
          original.environment ?? null,
          original.retentionClass === 'test' ? 'test' : null,
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

  /**
   * Soft-delete a terminal task (or hard-delete when mode=hard).
   * Active tasks → 409 task_not_terminal. Already deleted → already_deleted.
   */
  async delete(
    id: string,
    options: {
      mode?: 'soft' | 'hard';
      reason?: string;
      deletedBy?: string;
      idempotencyKey?: string;
    } = {},
  ): Promise<{
    task_id: string;
    deleted: boolean;
    already_deleted: boolean;
    mode: 'soft' | 'hard';
    task?: TaskIndex;
    idempotency?: { key: string; replayed: boolean; request_hash: string };
  }> {
    const mode = options.mode ?? 'soft';
    const reason = options.reason?.trim() || 'manual_cleanup';
    const deletedBy = options.deletedBy?.trim() || 'mcp';
    const requestHash = await hashOpaque({
      operation: 'tasks.delete',
      task_id: id,
      mode,
      reason,
    });
    const scope = 'tasks.delete';

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
            task_id: string;
            deleted: boolean;
            already_deleted: boolean;
            mode: 'soft' | 'hard';
            task?: TaskIndex;
          }),
          idempotency: {
            key: options.idempotencyKey,
            replayed: true,
            request_hash: requestHash,
          },
        };
      }
    }

    const row = await this.db.prepare('SELECT * FROM tasks WHERE id = ?').bind(id).first<TaskRow>();
    if (!row) throw new AppError('task_not_found', `Task '${id}' was not found.`, 404);

    let body: {
      task_id: string;
      deleted: boolean;
      already_deleted: boolean;
      mode: 'soft' | 'hard';
      task?: TaskIndex;
    };

    if (row.deleted_at && mode === 'soft') {
      body = {
        task_id: id,
        deleted: false,
        already_deleted: true,
        mode: 'soft',
        task: toTaskIndex(row),
      };
    } else if (!isTerminalTaskStatus(row.status) && !row.deleted_at) {
      throw new AppError(
        'task_not_terminal',
        `Task '${id}' is still active (${row.status}); cancel it before delete.`,
        409,
        { task_id: id, status: row.status },
      );
    } else if (mode === 'hard') {
      await this.db.prepare('DELETE FROM tasks WHERE id = ?').bind(id).run();
      body = { task_id: id, deleted: true, already_deleted: false, mode: 'hard' };
    } else {
      const now = new Date().toISOString();
      await this.db
        .prepare(
          `UPDATE tasks
           SET deleted_at = ?, deleted_by = ?, deletion_reason = ?,
               cleanup_state = 'deleted', updated_at = ?
           WHERE id = ? AND deleted_at IS NULL`,
        )
        .bind(now, deletedBy, reason, now, id)
        .run();
      const refreshed = await this.db
        .prepare('SELECT * FROM tasks WHERE id = ?')
        .bind(id)
        .first<TaskRow>();
      body = {
        task_id: id,
        deleted: true,
        already_deleted: false,
        mode: 'soft',
        ...(refreshed ? { task: toTaskIndex(refreshed) } : {}),
      };
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

  /** Preview how many terminal tasks match cleanup filters. */
  async cleanupPreview(
    filters: CleanupFilters = {},
    options: { limit?: number; sampleLimit?: number } = {},
  ): Promise<{
    matched_count: number;
    deletable_count: number;
    protected_count: number;
    status_breakdown: Record<string, number>;
    sample_task_ids: string[];
    filters: CleanupFilters;
  }> {
    const sampleLimit = Math.min(Math.max(options.sampleLimit ?? 20, 0), 100);
    const scanLimit = Math.min(
      Math.max(options.limit ?? CLEANUP_MAX_PER_RUN, 1),
      CLEANUP_MAX_PER_RUN,
    );
    const { clauses, binds } = buildCleanupClauses(filters, { forDelete: false });
    // Always only consider terminal for cleanup preview (non-terminal → protected separately).
    clauses.push(
      `status IN ('succeeded','failed','cancelled','timed_out','dispatch_failed')`,
    );
    if (!filters.includeDeleted) {
      clauses.push('deleted_at IS NULL');
    }
    const where = clauses.length ? `WHERE ${clauses.join(' AND ')}` : '';

    const rows = await this.db
      .prepare(
        `SELECT id, status FROM tasks ${where} ORDER BY created_at DESC LIMIT ?`,
      )
      .bind(...binds, scanLimit)
      .all<{ id: string; status: string }>();

    const status_breakdown: Record<string, number> = {};
    for (const row of rows.results) {
      status_breakdown[row.status] = (status_breakdown[row.status] ?? 0) + 1;
    }
    const matched_count = rows.results.length;
    return {
      matched_count,
      deletable_count: matched_count,
      protected_count: 0,
      status_breakdown,
      sample_task_ids: rows.results.slice(0, sampleLimit).map((r) => r.id),
      filters,
    };
  }

  /**
   * Soft-delete (or hard-delete) terminal tasks matching filters.
   * Uses expected_matched_count to guard against scope drift after preview.
   */
  async cleanupRun(
    filters: CleanupFilters = {},
    options: {
      mode?: 'soft' | 'hard';
      reason?: string;
      deletedBy?: string;
      expectedMatchedCount?: number;
      limit?: number;
      idempotencyKey?: string;
    } = {},
  ): Promise<{
    matched_count: number;
    deleted_count: number;
    skipped_count: number;
    mode: 'soft' | 'hard';
    reason: string;
    sample_deleted_ids: string[];
    idempotency?: { key: string; replayed: boolean; request_hash: string };
  }> {
    const mode = options.mode ?? 'soft';
    const reason = options.reason?.trim() || 'bulk_cleanup';
    const deletedBy = options.deletedBy?.trim() || 'mcp';
    const batchLimit = Math.min(
      Math.max(options.limit ?? CLEANUP_BATCH_SIZE, 1),
      CLEANUP_MAX_PER_RUN,
    );
    const requestHash = await hashOpaque({
      operation: 'tasks.cleanup.run',
      filters,
      mode,
      reason,
      expectedMatchedCount: options.expectedMatchedCount ?? null,
      limit: batchLimit,
    });
    const scope = 'tasks.cleanup.run';

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
            matched_count: number;
            deleted_count: number;
            skipped_count: number;
            mode: 'soft' | 'hard';
            reason: string;
            sample_deleted_ids: string[];
          }),
          idempotency: {
            key: options.idempotencyKey,
            replayed: true,
            request_hash: requestHash,
          },
        };
      }
    }

    const preview = await this.cleanupPreview(filters, { limit: batchLimit });
    if (
      options.expectedMatchedCount !== undefined &&
      !scopeCountAcceptable(options.expectedMatchedCount, preview.matched_count)
    ) {
      throw new AppError(
        'cleanup_scope_changed',
        `Cleanup scope changed: expected ~${options.expectedMatchedCount}, found ${preview.matched_count}.`,
        409,
        {
          expected_matched_count: options.expectedMatchedCount,
          actual_matched_count: preview.matched_count,
        },
      );
    }

    const { clauses, binds } = buildCleanupClauses(filters, { forDelete: true });
    clauses.push(`status IN ('succeeded','failed','cancelled','timed_out','dispatch_failed')`);
    if (mode === 'soft' || !filters.includeDeleted) {
      clauses.push('deleted_at IS NULL');
    }
    const where = clauses.length ? `WHERE ${clauses.join(' AND ')}` : '';

    const candidates = await this.db
      .prepare(`SELECT id FROM tasks ${where} ORDER BY created_at ASC LIMIT ?`)
      .bind(...binds, batchLimit)
      .all<{ id: string }>();

    const now = new Date().toISOString();
    let deleted_count = 0;
    const sample_deleted_ids: string[] = [];
    for (const row of candidates.results) {
      try {
        if (mode === 'hard') {
          await this.db.prepare('DELETE FROM tasks WHERE id = ?').bind(row.id).run();
        } else {
          await this.db
            .prepare(
              `UPDATE tasks
               SET deleted_at = ?, deleted_by = ?, deletion_reason = ?,
                   cleanup_state = 'deleted', updated_at = ?
               WHERE id = ? AND deleted_at IS NULL
                 AND status IN ('succeeded','failed','cancelled','timed_out','dispatch_failed')`,
            )
            .bind(now, deletedBy, reason, now, row.id)
            .run();
        }
        deleted_count += 1;
        if (sample_deleted_ids.length < 20) sample_deleted_ids.push(row.id);
      } catch {
        // skip single-row failure
      }
    }

    const body = {
      matched_count: preview.matched_count,
      deleted_count,
      skipped_count: Math.max(0, candidates.results.length - deleted_count),
      mode,
      reason,
      sample_deleted_ids,
    };

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

  /**
   * Cron: soft-delete expired *test* tasks; hard-delete soft-deleted past grace.
   * Production auto-retention stays off until Dry Run phase lands.
   */
  async purgeExpired(): Promise<{
    soft_deleted: number;
    hard_deleted: number;
  }> {
    const now = new Date().toISOString();
    const graceCutoff = addHours(now, -SOFT_DELETE_GRACE_HOURS);

    // Soft-delete expired test terminal tasks only (Phase 0–1 safety).
    const softCandidates = await this.db
      .prepare(
        `SELECT id FROM tasks
         WHERE deleted_at IS NULL
           AND terminal_at IS NOT NULL
           AND expires_at IS NOT NULL
           AND expires_at <= ?
           AND status IN ('succeeded','failed','cancelled','timed_out','dispatch_failed')
           AND (
             environment = 'test'
             OR retention_class = 'test'
           )
         ORDER BY expires_at ASC
         LIMIT ?`,
      )
      .bind(now, CLEANUP_BATCH_SIZE)
      .all<{ id: string }>();

    let soft_deleted = 0;
    for (const row of softCandidates.results) {
      try {
        await this.db
          .prepare(
            `UPDATE tasks
             SET deleted_at = ?, deleted_by = 'system', deletion_reason = 'retention_expired',
                 cleanup_state = 'deleted', updated_at = ?
             WHERE id = ? AND deleted_at IS NULL`,
          )
          .bind(now, now, row.id)
          .run();
        soft_deleted += 1;
      } catch {
        // continue
      }
    }

    // Hard-delete past soft-delete grace (all environments once soft-deleted).
    const hardCandidates = await this.db
      .prepare(
        `SELECT id FROM tasks
         WHERE deleted_at IS NOT NULL
           AND deleted_at <= ?
         ORDER BY deleted_at ASC
         LIMIT ?`,
      )
      .bind(graceCutoff, CLEANUP_BATCH_SIZE)
      .all<{ id: string }>();

    let hard_deleted = 0;
    for (const row of hardCandidates.results) {
      try {
        await this.db.prepare('DELETE FROM tasks WHERE id = ?').bind(row.id).run();
        hard_deleted += 1;
      } catch {
        // continue
      }
    }

    return { soft_deleted, hard_deleted };
  }

  private async findByIdempotency(
    backendId: string,
    idempotencyKey: string,
  ): Promise<TaskIndex | undefined> {
    // Include soft-deleted so idempotency survives soft delete until hard purge.
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
    if (!isTerminalTaskStatus(status)) {
      await this.db
        .prepare('UPDATE tasks SET status = ?, updated_at = ? WHERE id = ?')
        .bind(status, now, id)
        .run();
      return;
    }

    const row = await this.db
      .prepare('SELECT labels_json, environment, terminal_at, retention_class FROM tasks WHERE id = ?')
      .bind(id)
      .first<{
        labels_json: string | null;
        environment: string | null;
        terminal_at: string | null;
        retention_class: string | null;
      }>();
    const labels = parseLabelsJson(row?.labels_json);
    const environment = row?.environment ?? environmentFromLabels(labels);
    const retentionClass = retentionClassFor(status, labels, environment);
    const terminalAt = row?.terminal_at ?? now;
    const expiresAt = computeExpiresAt(terminalAt, retentionClass, status);

    await this.db
      .prepare(
        `UPDATE tasks SET
           status = ?,
           updated_at = ?,
           finished_at = COALESCE(finished_at, ?),
           terminal_at = COALESCE(terminal_at, ?),
           expires_at = COALESCE(expires_at, ?),
           retention_class = COALESCE(NULLIF(retention_class, ''), ?),
           cleanup_state = CASE
             WHEN cleanup_state IS NULL OR cleanup_state = 'none' THEN 'eligible'
             ELSE cleanup_state
           END
         WHERE id = ?`,
      )
      .bind(status, now, now, terminalAt, expiresAt, retentionClass, id)
      .run();
  }
}

function buildListClauses(opts: TaskListQuery): { clauses: string[]; binds: unknown[] } {
  const clauses: string[] = [];
  const binds: unknown[] = [];
  if (!opts.includeDeleted) {
    clauses.push('deleted_at IS NULL');
  }
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
  if (opts.source) {
    clauses.push('source = ?');
    binds.push(opts.source);
  }
  if (opts.environment) {
    clauses.push('environment = ?');
    binds.push(opts.environment);
  }
  if (opts.scheduleId) {
    clauses.push('schedule_id = ?');
    binds.push(opts.scheduleId);
  }
  if (opts.terminal === true) {
    clauses.push(
      `status IN ('succeeded','failed','cancelled','timed_out','dispatch_failed')`,
    );
  } else if (opts.terminal === false) {
    clauses.push(
      `status NOT IN ('succeeded','failed','cancelled','timed_out','dispatch_failed')`,
    );
  }
  if (opts.createdAfter) {
    clauses.push('created_at >= ?');
    binds.push(opts.createdAfter);
  }
  if (opts.createdBefore) {
    clauses.push('created_at <= ?');
    binds.push(opts.createdBefore);
  }
  if (opts.terminalBefore) {
    clauses.push('terminal_at IS NOT NULL AND terminal_at <= ?');
    binds.push(opts.terminalBefore);
  }
  if (opts.expiresBefore) {
    clauses.push('expires_at IS NOT NULL AND expires_at <= ?');
    binds.push(opts.expiresBefore);
  }
  if (opts.hideTest) {
    clauses.push(
      `(environment IS NULL OR environment != 'test') AND (retention_class IS NULL OR retention_class != 'test')`,
    );
  }
  if (opts.labels) {
    for (const [key, value] of Object.entries(opts.labels)) {
      // json_extract works on D1/SQLite for labels_json object.
      clauses.push(`json_extract(labels_json, ?) = ?`);
      binds.push(`$.${key.replace(/[^A-Za-z0-9_]/g, '')}`, value);
    }
  }
  return { clauses, binds };
}

function buildCleanupClauses(
  filters: CleanupFilters,
  _opts: { forDelete: boolean },
): { clauses: string[]; binds: unknown[] } {
  const clauses: string[] = [];
  const binds: unknown[] = [];
  if (filters.backendId) {
    clauses.push('backend_id = ?');
    binds.push(filters.backendId);
  }
  if (filters.scheduleId) {
    clauses.push('schedule_id = ?');
    binds.push(filters.scheduleId);
  }
  if (filters.status) {
    clauses.push('status = ?');
    binds.push(filters.status);
  }
  if (filters.source) {
    clauses.push('source = ?');
    binds.push(filters.source);
  }
  if (filters.environment) {
    clauses.push('environment = ?');
    binds.push(filters.environment);
  }
  if (filters.createdAfter) {
    clauses.push('created_at >= ?');
    binds.push(filters.createdAfter);
  }
  if (filters.createdBefore) {
    clauses.push('created_at <= ?');
    binds.push(filters.createdBefore);
  }
  if (filters.terminalBefore) {
    clauses.push('terminal_at IS NOT NULL AND terminal_at <= ?');
    binds.push(filters.terminalBefore);
  }
  if (filters.expiresBefore) {
    clauses.push('expires_at IS NOT NULL AND expires_at <= ?');
    binds.push(filters.expiresBefore);
  }
  if (filters.expiredOnly) {
    clauses.push('expires_at IS NOT NULL AND expires_at <= ?');
    binds.push(new Date().toISOString());
  }
  if (filters.testOnly) {
    clauses.push(`(environment = 'test' OR retention_class = 'test')`);
  }
  if (filters.deletedBefore) {
    clauses.push('deleted_at IS NOT NULL AND deleted_at <= ?');
    binds.push(filters.deletedBefore);
  }
  if (filters.labels) {
    for (const [key, value] of Object.entries(filters.labels)) {
      clauses.push(`json_extract(labels_json, ?) = ?`);
      binds.push(`$.${key.replace(/[^A-Za-z0-9_]/g, '')}`, value);
    }
  }
  return { clauses, binds };
}

function toTaskIndex(row: TaskRow): TaskIndex {
  const labels = parseLabelsJson(row.labels_json);
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
    ...(row.terminal_at ? { terminalAt: row.terminal_at } : {}),
    ...(row.expires_at ? { expiresAt: row.expires_at } : {}),
    ...(Object.keys(labels).length ? { labels } : {}),
    ...(row.environment ? { environment: row.environment } : {}),
    ...(row.retention_class ? { retentionClass: row.retention_class } : {}),
    ...(row.deleted_at ? { deletedAt: row.deleted_at } : {}),
    ...(row.deleted_by ? { deletedBy: row.deleted_by } : {}),
    ...(row.deletion_reason ? { deletionReason: row.deletion_reason } : {}),
    ...(row.cleanup_state ? { cleanupState: row.cleanup_state } : {}),
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
