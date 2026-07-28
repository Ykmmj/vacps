import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';

import Database from 'better-sqlite3';
import type { CommandExecution, TaskDispatch, TaskError, TaskStatus } from '@vacps/contracts';

export interface StoredTask {
  task: TaskDispatch;
  status: TaskStatus;
  graphNode?: string;
  result?: unknown;
  error?: TaskError;
  createdAt: string;
  startedAt?: string;
  finishedAt?: string;
}

interface TaskRow {
  id: string;
  input_json: string;
  status: TaskStatus;
  graph_node: string | null;
  result_json: string | null;
  error_json: string | null;
  created_at: string;
  started_at: string | null;
  finished_at: string | null;
}

interface CommandRow {
  id: string;
  sequence: number;
  command: string;
  cwd: string;
  status: CommandExecution['status'];
  exit_code: number | null;
  stdout_path: string | null;
  stderr_path: string | null;
  started_at: string;
  finished_at: string | null;
}

export class TaskStore {
  private readonly db: Database.Database;

  constructor(databasePath: string) {
    mkdirSync(dirname(databasePath), { recursive: true });
    this.db = new Database(databasePath);
    this.db.pragma('journal_mode = WAL');
    this.db.pragma('foreign_keys = ON');
    this.migrate();
  }

  close(): void {
    this.db.close();
  }

  createTask(task: TaskDispatch, status: TaskStatus = 'queued'): void {
    const now = new Date().toISOString();
    this.db
      .prepare(
        `INSERT INTO tasks (id, bull_job_id, type, profile, input_json, status, created_at)
         VALUES (@id, @bullJobId, @type, @profile, @inputJson, @status, @createdAt)
         ON CONFLICT(id) DO NOTHING`,
      )
      .run({
        id: task.taskId,
        bullJobId: task.taskId,
        type: task.type,
        profile: task.profile,
        inputJson: JSON.stringify(task),
        status,
        createdAt: now,
      });
  }

  updateTask(
    taskId: string,
    update: {
      status?: TaskStatus;
      graphNode?: string;
      result?: unknown;
      error?: TaskError;
      startedAt?: string;
      finishedAt?: string;
    },
  ): void {
    const fields: string[] = [];
    const values: Record<string, unknown> = { id: taskId };
    if (update.status !== undefined) {
      fields.push('status = @status');
      values.status = update.status;
    }
    if (update.graphNode !== undefined) {
      fields.push('graph_node = @graphNode');
      values.graphNode = update.graphNode;
    }
    if (update.result !== undefined) {
      fields.push('result_json = @resultJson');
      values.resultJson = JSON.stringify(update.result);
    }
    if (update.error !== undefined) {
      fields.push('error_json = @errorJson');
      values.errorJson = JSON.stringify(update.error);
    }
    if (update.startedAt !== undefined) {
      fields.push('started_at = @startedAt');
      values.startedAt = update.startedAt;
    }
    if (update.finishedAt !== undefined) {
      fields.push('finished_at = @finishedAt');
      values.finishedAt = update.finishedAt;
    }
    if (fields.length === 0) return;
    this.db.prepare(`UPDATE tasks SET ${fields.join(', ')} WHERE id = @id`).run(values);
  }

  getTask(taskId: string): StoredTask | undefined {
    const row = this.db.prepare('SELECT * FROM tasks WHERE id = ?').get(taskId) as
      TaskRow | undefined;
    if (!row) return undefined;
    return {
      task: JSON.parse(row.input_json) as TaskDispatch,
      status: row.status,
      ...(row.graph_node ? { graphNode: row.graph_node } : {}),
      ...(row.result_json ? { result: JSON.parse(row.result_json) as unknown } : {}),
      ...(row.error_json ? { error: JSON.parse(row.error_json) as TaskError } : {}),
      createdAt: row.created_at,
      ...(row.started_at ? { startedAt: row.started_at } : {}),
      ...(row.finished_at ? { finishedAt: row.finished_at } : {}),
    };
  }

  listCommands(taskId: string): CommandExecution[] {
    const rows = this.db
      .prepare('SELECT * FROM commands WHERE task_id = ? ORDER BY sequence ASC')
      .all(taskId) as CommandRow[];
    return rows.map((row) => ({
      id: row.id,
      sequence: row.sequence,
      command: row.command,
      cwd: row.cwd,
      status: row.status,
      ...(row.exit_code !== null ? { exitCode: row.exit_code } : {}),
      ...(row.stdout_path ? { stdoutPath: row.stdout_path } : {}),
      ...(row.stderr_path ? { stderrPath: row.stderr_path } : {}),
      startedAt: row.started_at,
      ...(row.finished_at ? { finishedAt: row.finished_at } : {}),
    }));
  }

  startCommand(
    input: Omit<CommandExecution, 'finishedAt' | 'exitCode'> & { taskId: string },
  ): void {
    this.db
      .prepare(
        `INSERT INTO commands
          (id, task_id, sequence, command, cwd, status, stdout_path, stderr_path, started_at)
         VALUES (@id, @taskId, @sequence, @command, @cwd, @status, @stdoutPath, @stderrPath, @startedAt)`,
      )
      .run(input);
  }

  finishCommand(input: {
    id: string;
    status: CommandExecution['status'];
    exitCode: number | null;
    finishedAt: string;
  }): void {
    this.db
      .prepare(
        'UPDATE commands SET status = @status, exit_code = @exitCode, finished_at = @finishedAt WHERE id = @id',
      )
      .run(input);
  }

  saveCheckpoint(taskId: string, graphNode: string, state: unknown): void {
    this.db
      .prepare(
        `INSERT INTO graph_checkpoints (task_id, graph_node, state_json, created_at)
         VALUES (?, ?, ?, ?)
         ON CONFLICT(task_id, graph_node) DO UPDATE SET state_json = excluded.state_json,
           created_at = excluded.created_at`,
      )
      .run(taskId, graphNode, JSON.stringify(state), new Date().toISOString());
  }

  /** Returns false when a signed control-plane request has already been accepted. */
  claimControlPlaneNonce(nonce: string, expiresAt: string): boolean {
    this.db
      .prepare('DELETE FROM control_plane_request_nonces WHERE expires_at <= ?')
      .run(new Date().toISOString());
    const result = this.db
      .prepare(
        `INSERT INTO control_plane_request_nonces (nonce, expires_at)
         VALUES (?, ?) ON CONFLICT(nonce) DO NOTHING`,
      )
      .run(nonce, expiresAt);
    return result.changes === 1;
  }

  private migrate(): void {
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS tasks (
        id TEXT PRIMARY KEY,
        bull_job_id TEXT NOT NULL,
        type TEXT NOT NULL,
        profile TEXT NOT NULL,
        input_json TEXT NOT NULL,
        status TEXT NOT NULL,
        graph_node TEXT,
        result_json TEXT,
        error_json TEXT,
        created_at TEXT NOT NULL,
        started_at TEXT,
        finished_at TEXT
      );
      CREATE TABLE IF NOT EXISTS commands (
        id TEXT PRIMARY KEY,
        task_id TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
        sequence INTEGER NOT NULL,
        command TEXT NOT NULL,
        cwd TEXT NOT NULL,
        status TEXT NOT NULL,
        exit_code INTEGER,
        stdout_path TEXT,
        stderr_path TEXT,
        started_at TEXT NOT NULL,
        finished_at TEXT,
        UNIQUE(task_id, sequence)
      );
      CREATE TABLE IF NOT EXISTS graph_checkpoints (
        task_id TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
        graph_node TEXT NOT NULL,
        state_json TEXT NOT NULL,
        created_at TEXT NOT NULL,
        PRIMARY KEY(task_id, graph_node)
      );
      CREATE INDEX IF NOT EXISTS commands_task_id_idx ON commands(task_id, sequence);
      CREATE TABLE IF NOT EXISTS control_plane_request_nonces (
        nonce TEXT PRIMARY KEY,
        expires_at TEXT NOT NULL
      );
      CREATE INDEX IF NOT EXISTS control_plane_request_nonces_expiry_idx
        ON control_plane_request_nonces(expires_at);
    `);
  }
}
