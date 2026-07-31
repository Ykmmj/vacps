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
  private constructor(private readonly db: Store) {}

  static async create(db: Store): Promise<TaskStore> {
    await migrateAgentDb(db);
    return new TaskStore(db);
  }

  /**
   * Insert task. Returns false if task_id already exists.
   * If idempotency_key is set and already mapped, returns false (caller should load existing).
   */
  async createTask(
    task: TaskDispatch,
    status: TaskStatus = 'queued',
    occurrence?: {
      scheduleId: string;
      scheduleRevision: number;
      scheduledForMs: number;
    },
  ): Promise<boolean> {
    const now = new Date().toISOString();
    const idem = task.idempotency_key ?? null;
    // Single atomic unit: insert task only if id and optional idempotency key are free.
    const steps = [
      {
        sql: `INSERT INTO tasks(
            id, backend_id, kind, status, profile, input_json,
            cancel_requested, created_at, updated_at,
            schedule_id, schedule_revision, scheduled_for_ms
          )
          SELECT ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?
          WHERE NOT EXISTS (SELECT 1 FROM tasks WHERE id = ?)
            AND (
              ? IS NULL
              OR NOT EXISTS (
                SELECT 1 FROM task_idempotency WHERE idempotency_key = ?
              )
            );`,
        params: [
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
          task.task_id,
          idem,
          idem,
        ],
      },
    ];
    if (idem) {
      steps.push({
        sql: `INSERT INTO task_idempotency(idempotency_key, task_id, created_at)
              SELECT ?, ?, ?
              WHERE EXISTS (
                SELECT 1 FROM tasks WHERE id = ? AND created_at = ?
              )
              AND NOT EXISTS (
                SELECT 1 FROM task_idempotency WHERE idempotency_key = ?
              );`,
        params: [idem, task.task_id, now, task.task_id, now, idem],
      });
    }
    const results = await this.db.transaction(steps);
    return (results[0]?.changes ?? 0) === 1;
  }

  /**
   * Insert schedule occurrence task. Idempotent on task_id / occurrence unique index.
   * Throws on unexpected constraint errors (caller must rollback claim txn).
   * Returns true if a new row was inserted.
   */
  async insertOccurrenceTask(
    task: TaskDispatch,
    occurrence: {
      scheduleId: string;
      scheduleRevision: number;
      scheduledForMs: number;
    },
  ): Promise<boolean> {
    const now = new Date().toISOString();
    const result = await this.db.run(
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

  async findByIdempotencyKey(key: string): Promise<StoredTask | undefined> {
    const rows = await this.db.query(
      'SELECT task_id FROM task_idempotency WHERE idempotency_key = ?;',
      [key],
    );
    if (rows.length === 0) return undefined;
    return this.getTask(String(rows[0]!['task_id']));
  }

  /**
   * On agent start: tasks left in `running` must NOT re-execute (side effects).
   * Mark failed with agent_restarted (design §21).
   */
  async recoverInterruptedOnBoot(): Promise<number> {
    const rows = await this.db.query(
      `SELECT id FROM tasks WHERE status = 'running' OR status = 'starting';`,
    );
    const now = new Date().toISOString();
    let n = 0;
    for (const row of rows) {
      const id = String(row['id']);
      await this.db.run(
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
      await this.appendLog(id, 'system', 'interrupted by agent restart (agent_restarted)');
      n += 1;
    }
    return n;
  }

  /** Clone dispatch with a new task_id for retry. */
  async enqueueRetryOf(taskId: string, newTaskId: string): Promise<StoredTask | undefined> {
    const cur = await this.getTask(taskId);
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
    await this.createTask(next, 'queued');
    await this.appendLog(newTaskId, 'system', `retry_of=${taskId}`);
    return this.getTask(newTaskId);
  }

  async getTask(taskId: string): Promise<StoredTask | undefined> {
    const rows = await this.db.query('SELECT * FROM tasks WHERE id = ?;', [taskId]);
    if (rows.length === 0) return undefined;
    return rowToStored(rows[0]!);
  }

  /**
   * Atomically claim the oldest queued task (UPDATE … RETURNING).
   * Success is solely whether a row is returned — never re-query status.
   */
  async claimNextQueued(): Promise<StoredTask | undefined> {
    const now = new Date().toISOString();
    const rows = await this.db.query(
      `UPDATE tasks SET status = 'running', started_at = ?, updated_at = ?
       WHERE id = (
         SELECT id FROM tasks
         WHERE status = 'queued' AND cancel_requested = 0
         ORDER BY created_at ASC
         LIMIT 1
       )
       AND status = 'queued'
       RETURNING *;`,
      [now, now],
    );
    if (rows.length === 0) return undefined;
    return rowToStored(rows[0]!);
  }

  async updateTask(
    taskId: string,
    update: {
      status?: TaskStatus;
      result?: unknown;
      error?: TaskError;
      startedAt?: string;
      finishedAt?: string;
    },
  ): Promise<void> {
    const cur = await this.getTask(taskId);
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

    await this.db.run(
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

  async requestCancel(taskId: string): Promise<boolean> {
    const cur = await this.getTask(taskId);
    if (!cur) return false;
    if (isTerminalTaskStatus(cur.status)) return false;
    await this.db.run(`UPDATE tasks SET cancel_requested = 1, updated_at = ? WHERE id = ?;`, [
      new Date().toISOString(),
      taskId,
    ]);
    if (cur.status === 'queued') {
      await this.updateTask(taskId, {
        status: 'cancelled',
        finishedAt: new Date().toISOString(),
        error: { code: 'cancelled', message: 'Cancelled before start.' },
      });
    }
    return true;
  }

  async isCancelRequested(taskId: string): Promise<boolean> {
    const rows = await this.db.query('SELECT cancel_requested FROM tasks WHERE id = ?;', [taskId]);
    if (rows.length === 0) return false;
    return Number(rows[0]!['cancel_requested']) === 1;
  }

  async appendLog(
    taskId: string,
    stream: 'stdout' | 'stderr' | 'system',
    data: string,
  ): Promise<void> {
    const seqRows = await this.db.query(
      'SELECT COALESCE(MAX(sequence), 0) AS m FROM task_logs WHERE task_id = ?;',
      [taskId],
    );
    const next = Number(seqRows[0]?.['m'] ?? 0) + 1;
    await this.db.run(
      `INSERT INTO task_logs(task_id, sequence, stream, data, created_at)
       VALUES(?, ?, ?, ?, ?);`,
      [taskId, next, stream, data, new Date().toISOString()],
    );
  }

  async listLogs(
    taskId: string,
    opts?: { stream?: string; offset?: number; limit?: number },
  ): Promise<Array<{ sequence: number; stream: string; data: string; createdAt: string }>> {
    const offset = opts?.offset ?? 0;
    const limit = opts?.limit ?? 500;
    const rows = opts?.stream
      ? await this.db.query(
          `SELECT * FROM task_logs
           WHERE task_id = ? AND stream = ? AND sequence > ?
           ORDER BY sequence ASC LIMIT ?;`,
          [taskId, opts.stream, offset, limit],
        )
      : await this.db.query(
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

  async claimNonce(nonce: string, ttlSeconds = 600): Promise<boolean> {
    const now = Math.floor(Date.now() / 1000);
    const results = await this.db.transaction([
      { sql: 'DELETE FROM request_nonces WHERE expires_at < ?;', params: [now] },
      {
        sql: `INSERT INTO request_nonces(nonce, expires_at)
              SELECT ?, ?
              WHERE NOT EXISTS (SELECT 1 FROM request_nonces WHERE nonce = ?);`,
        params: [nonce, now + ttlSeconds, nonce],
      },
    ]);
    return (results[1]?.changes ?? 0) === 1;
  }

  async hasRunning(): Promise<boolean> {
    const rows = await this.db.query("SELECT 1 AS x FROM tasks WHERE status = 'running' LIMIT 1;");
    return rows.length > 0;
  }

  /** Counts for /metrics queue block. */
  async queueCounts(): Promise<{ waiting: number; active: number; failed: number }> {
    const one = async (status: string) => {
      const rows = await this.db.query('SELECT COUNT(*) AS c FROM tasks WHERE status = ?;', [
        status,
      ]);
      return Number(rows[0]?.['c'] ?? 0) || 0;
    };
    return {
      waiting: await one('queued'),
      active: (await one('running')) + (await one('starting')),
      failed: await one('failed'),
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
