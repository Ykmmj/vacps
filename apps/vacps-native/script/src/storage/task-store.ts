import type { TaskDispatch, TaskError, TaskStatus } from '@vacps/contracts';
import { isTerminalTaskStatus } from '@vacps/contracts';
import type { Store } from 'vacps:store';

import { migrateAgentDb } from './schema';

export interface StoredTask {
  task: TaskDispatch;
  status: TaskStatus;
  result?: unknown;
  error?: TaskError;
  cancelRequested: boolean;
  createdAt: string;
  startedAt?: string;
  finishedAt?: string;
  updatedAt: string;
}

export class TaskStore {
  constructor(private readonly db: Store) {
    migrateAgentDb(db);
  }

  /**
   * Insert task. Returns false if task_id already exists.
   * If idempotency_key is set and already mapped, returns false (caller should load existing).
   */
  createTask(
    task: TaskDispatch,
    status: TaskStatus = 'queued',
    occurrence?: {
      scheduleId: string;
      scheduleRevision: number;
      scheduledForMs: number;
    },
  ): boolean {
    const now = new Date().toISOString();
    if (task.idempotency_key) {
      const byKey = this.findByIdempotencyKey(task.idempotency_key);
      if (byKey) return false;
    }
    const existing = this.getTask(task.task_id);
    if (existing) return false;
    this.db.run(
      `INSERT INTO tasks(
        id, backend_id, kind, status, profile, input_json,
        cancel_requested, created_at, updated_at,
        schedule_id, schedule_revision, scheduled_for_ms
      ) VALUES(?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?);`,
      [
        task.task_id,
        task.backend_id,
        task.kind,
        status,
        task.profile,
        JSON.stringify(task),
        now,
        now,
        occurrence?.scheduleId ?? null,
        occurrence?.scheduleRevision ?? null,
        occurrence?.scheduledForMs ?? null,
      ],
    );
    if (task.idempotency_key) {
      this.db.run(
        `INSERT INTO task_idempotency(idempotency_key, task_id, created_at)
         VALUES(?, ?, ?);`,
        [task.idempotency_key, task.task_id, now],
      );
    }
    return true;
  }

  /**
   * Insert schedule occurrence task. Idempotent on task_id / occurrence unique index.
   * Throws on unexpected constraint errors (caller must rollback claim txn).
   * Returns true if a new row was inserted.
   */
  insertOccurrenceTask(
    task: TaskDispatch,
    occurrence: {
      scheduleId: string;
      scheduleRevision: number;
      scheduledForMs: number;
    },
  ): boolean {
    const now = new Date().toISOString();
    const result = this.db.run(
      `INSERT INTO tasks(
        id, backend_id, kind, status, profile, input_json,
        cancel_requested, created_at, updated_at,
        schedule_id, schedule_revision, scheduled_for_ms
      ) VALUES(?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?)
      ON CONFLICT(id) DO NOTHING;`,
      [
        task.task_id,
        task.backend_id,
        task.kind,
        'queued',
        task.profile,
        JSON.stringify(task),
        now,
        now,
        occurrence.scheduleId,
        occurrence.scheduleRevision,
        occurrence.scheduledForMs,
      ],
    );
    return result.changes === 1;
  }

  findByIdempotencyKey(key: string): StoredTask | undefined {
    const rows = this.db.query('SELECT task_id FROM task_idempotency WHERE idempotency_key = ?;', [
      key,
    ]);
    if (rows.length === 0) return undefined;
    return this.getTask(String(rows[0]!['task_id']));
  }

  /**
   * On agent start: tasks left in `running` must NOT re-execute (side effects).
   * Mark failed with agent_restarted (design §21).
   */
  recoverInterruptedOnBoot(): number {
    const rows = this.db.query(
      `SELECT id FROM tasks WHERE status = 'running' OR status = 'starting';`,
    );
    const now = new Date().toISOString();
    let n = 0;
    for (const row of rows) {
      const id = String(row['id']);
      this.db.run(
        `UPDATE tasks SET
          status = 'failed',
          error_json = ?,
          finished_at = COALESCE(finished_at, ?),
          updated_at = ?,
          cancel_requested = 0
         WHERE id = ? AND status IN ('running', 'starting');`,
        [
          JSON.stringify({
            code: 'agent_restarted',
            message: 'Agent restarted while task was running; not re-executed.',
          }),
          now,
          now,
          id,
        ],
      );
      this.appendLog(id, 'system', 'interrupted by agent restart (agent_restarted)');
      n += 1;
    }
    return n;
  }

  /** Clone dispatch with a new task_id for retry. */
  enqueueRetryOf(taskId: string, newTaskId: string): StoredTask | undefined {
    const cur = this.getTask(taskId);
    if (!cur) return undefined;
    if (!isTerminalTaskStatus(cur.status)) {
      throw new Error('Can only retry terminal tasks.');
    }
    const { task_id: _old, idempotency_key: _idem, ...rest } = cur.task;
    const next = {
      ...rest,
      task_id: newTaskId,
      source: cur.task.source ?? 'api',
    } as TaskDispatch;
    this.createTask(next, 'queued');
    this.appendLog(newTaskId, 'system', `retry_of=${taskId}`);
    return this.getTask(newTaskId);
  }

  getTask(taskId: string): StoredTask | undefined {
    const rows = this.db.query('SELECT * FROM tasks WHERE id = ?;', [taskId]);
    if (rows.length === 0) return undefined;
    return rowToStored(rows[0]!);
  }

  /** Oldest queued task, or undefined. */
  claimNextQueued(): StoredTask | undefined {
    const rows = this.db.query(
      `SELECT * FROM tasks
       WHERE status = 'queued' AND cancel_requested = 0
       ORDER BY created_at ASC LIMIT 1;`,
    );
    if (rows.length === 0) return undefined;
    const task = rowToStored(rows[0]!);
    const now = new Date().toISOString();
    this.db.run(
      `UPDATE tasks SET status = 'running', started_at = ?, updated_at = ?
       WHERE id = ? AND status = 'queued';`,
      [now, now, task.task.task_id],
    );
    const again = this.getTask(task.task.task_id);
    return again?.status === 'running' ? again : undefined;
  }

  updateTask(
    taskId: string,
    update: {
      status?: TaskStatus;
      result?: unknown;
      error?: TaskError;
      startedAt?: string;
      finishedAt?: string;
    },
  ): void {
    const cur = this.getTask(taskId);
    if (!cur) return;
    const now = new Date().toISOString();
    const status = update.status ?? cur.status;
    const resultJson =
      update.result !== undefined
        ? JSON.stringify(update.result)
        : cur.result !== undefined
          ? JSON.stringify(cur.result)
          : null;
    const errorJson =
      update.error !== undefined
        ? JSON.stringify(update.error)
        : cur.error !== undefined
          ? JSON.stringify(cur.error)
          : null;
    const startedAt = update.startedAt ?? cur.startedAt ?? null;
    const finishedAt =
      update.finishedAt ??
      (update.status && isTerminalTaskStatus(update.status) ? now : (cur.finishedAt ?? null));

    this.db.run(
      `UPDATE tasks SET
        status = ?,
        result_json = ?,
        error_json = ?,
        started_at = ?,
        finished_at = ?,
        updated_at = ?
       WHERE id = ?;`,
      [status, resultJson, errorJson, startedAt, finishedAt, now, taskId],
    );
  }

  requestCancel(taskId: string): boolean {
    const cur = this.getTask(taskId);
    if (!cur) return false;
    if (isTerminalTaskStatus(cur.status)) return false;
    this.db.run(`UPDATE tasks SET cancel_requested = 1, updated_at = ? WHERE id = ?;`, [
      new Date().toISOString(),
      taskId,
    ]);
    if (cur.status === 'queued') {
      this.updateTask(taskId, {
        status: 'cancelled',
        finishedAt: new Date().toISOString(),
        error: { code: 'cancelled', message: 'Cancelled before start.' },
      });
    }
    return true;
  }

  isCancelRequested(taskId: string): boolean {
    const rows = this.db.query('SELECT cancel_requested FROM tasks WHERE id = ?;', [taskId]);
    if (rows.length === 0) return false;
    return Number(rows[0]!['cancel_requested']) === 1;
  }

  appendLog(taskId: string, stream: 'stdout' | 'stderr' | 'system', data: string): void {
    const seqRows = this.db.query(
      'SELECT COALESCE(MAX(sequence), 0) AS m FROM task_logs WHERE task_id = ?;',
      [taskId],
    );
    const next = Number(seqRows[0]?.['m'] ?? 0) + 1;
    this.db.run(
      `INSERT INTO task_logs(task_id, sequence, stream, data, created_at)
       VALUES(?, ?, ?, ?, ?);`,
      [taskId, next, stream, data, new Date().toISOString()],
    );
  }

  listLogs(
    taskId: string,
    opts?: { stream?: string; offset?: number; limit?: number },
  ): Array<{ sequence: number; stream: string; data: string; createdAt: string }> {
    const offset = opts?.offset ?? 0;
    const limit = opts?.limit ?? 500;
    const rows = opts?.stream
      ? this.db.query(
          `SELECT * FROM task_logs
           WHERE task_id = ? AND stream = ? AND sequence > ?
           ORDER BY sequence ASC LIMIT ?;`,
          [taskId, opts.stream, offset, limit],
        )
      : this.db.query(
          `SELECT * FROM task_logs
           WHERE task_id = ? AND sequence > ?
           ORDER BY sequence ASC LIMIT ?;`,
          [taskId, offset, limit],
        );
    return rows.map((r) => ({
      sequence: Number(r['sequence']),
      stream: String(r['stream']),
      data: String(r['data']),
      createdAt: String(r['created_at']),
    }));
  }

  claimNonce(nonce: string, ttlSeconds = 600): boolean {
    const now = Math.floor(Date.now() / 1000);
    this.db.run('DELETE FROM request_nonces WHERE expires_at < ?;', [now]);
    const existing = this.db.query('SELECT nonce FROM request_nonces WHERE nonce = ?;', [nonce]);
    if (existing.length > 0) return false;
    this.db.run('INSERT INTO request_nonces(nonce, expires_at) VALUES(?, ?);', [
      nonce,
      now + ttlSeconds,
    ]);
    return true;
  }

  hasRunning(): boolean {
    const rows = this.db.query("SELECT 1 AS x FROM tasks WHERE status = 'running' LIMIT 1;");
    return rows.length > 0;
  }

  /** Counts for /metrics queue block. */
  queueCounts(): { waiting: number; active: number; failed: number } {
    const one = (status: string) => {
      const rows = this.db.query('SELECT COUNT(*) AS c FROM tasks WHERE status = ?;', [status]);
      return Number(rows[0]?.['c'] ?? 0) || 0;
    };
    return {
      waiting: one('queued'),
      active: one('running') + one('starting'),
      failed: one('failed'),
    };
  }
}

function rowToStored(row: Record<string, unknown>): StoredTask {
  const task = JSON.parse(String(row['input_json'])) as TaskDispatch;
  const out: StoredTask = {
    task,
    status: String(row['status']) as TaskStatus,
    cancelRequested: Number(row['cancel_requested']) === 1,
    createdAt: String(row['created_at']),
    updatedAt: String(row['updated_at']),
  };
  if (row['result_json']) out.result = JSON.parse(String(row['result_json']));
  if (row['error_json']) out.error = JSON.parse(String(row['error_json'])) as TaskError;
  if (row['started_at']) out.startedAt = String(row['started_at']);
  if (row['finished_at']) out.finishedAt = String(row['finished_at']);
  return out;
}
