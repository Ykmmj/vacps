/**
 * Minimal in-memory D1 for ScheduleService unit tests (ack / cursor CAS).
 * Not a general SQL engine — only the statements ScheduleService uses for get + ack.
 */

export type FakeScheduleRow = {
  id: string;
  backend_id: string;
  name: string;
  cron: string;
  timezone: string;
  task_json: string;
  enabled: number;
  revision: number;
  policy_json: string;
  idempotency_key: string | null;
  request_hash: string | null;
  last_run_at: string | null;
  next_run_at: string | null;
  created_at: string;
  updated_at: string;
};

function norm(sql: string): string {
  return sql.replace(/\s+/g, ' ').trim();
}

export class FakeScheduleD1 {
  schedules: FakeScheduleRow[] = [];

  prepare(sql: string) {
    const statement = norm(sql);
    const exec = (binds: unknown[]) => this.execute(statement, binds);
    return {
      bind: (...binds: unknown[]) => ({
        run: async () => {
          const r = exec(binds);
          return { meta: { changes: r.changes } };
        },
        first: async <T>() => (exec(binds).first as T | null) ?? null,
        all: async <T>() => ({ results: (exec(binds).all as T[]) ?? [] }),
      }),
      run: async () => {
        const r = exec([]);
        return { meta: { changes: r.changes } };
      },
      first: async <T>() => (exec([]).first as T | null) ?? null,
      all: async <T>() => ({ results: (exec([]).all as T[]) ?? [] }),
    };
  }

  private execute(
    sql: string,
    binds: unknown[],
  ): { first: FakeScheduleRow | null; all: FakeScheduleRow[]; changes: number } {
    if (sql === 'SELECT * FROM schedules WHERE id = ?') {
      const [id] = binds;
      const row = this.schedules.find((s) => s.id === id) ?? null;
      return { first: row, all: row ? [row] : [], changes: 0 };
    }

    if (
      sql.startsWith('UPDATE schedules') &&
      sql.includes('SET last_run_at = ?') &&
      sql.includes('next_run_at = ?') &&
      sql.includes('WHERE id = ? AND revision = ? AND next_run_at = ?')
    ) {
      const [last_run_at, next_run_at, updated_at, id, revision, next_token] = binds as [
        string,
        string | null,
        string,
        string,
        number,
        string,
      ];
      const row = this.schedules.find(
        (s) =>
          s.id === id &&
          s.revision === revision &&
          s.next_run_at === next_token,
      );
      if (!row) return { first: null, all: [], changes: 0 };
      row.last_run_at = last_run_at;
      row.next_run_at = next_run_at;
      row.updated_at = updated_at;
      return { first: row, all: [row], changes: 1 };
    }

    if (
      sql.startsWith('UPDATE schedules') &&
      sql.includes('SET last_run_at = ?') &&
      sql.includes('WHERE id = ? AND revision = ?') &&
      !sql.includes('next_run_at = ? WHERE')
    ) {
      // runNow path without next CAS (revision only)
      const [last_run_at, next_run_at, updated_at, id, revision] = binds as [
        string,
        string | null,
        string,
        string,
        number,
      ];
      const row = this.schedules.find((s) => s.id === id && s.revision === revision);
      if (!row) return { first: null, all: [], changes: 0 };
      row.last_run_at = last_run_at;
      row.next_run_at = next_run_at;
      row.updated_at = updated_at;
      return { first: row, all: [row], changes: 1 };
    }

    if (sql.includes('FROM schedules WHERE backend_id = ? AND idempotency_key = ?')) {
      const [backend_id, key] = binds;
      const row =
        this.schedules.find(
          (s) => s.backend_id === backend_id && s.idempotency_key === key,
        ) ?? null;
      return { first: row, all: row ? [row] : [], changes: 0 };
    }

    throw new Error(`FakeScheduleD1 unsupported SQL: ${sql}\n binds=${JSON.stringify(binds)}`);
  }
}

export function asD1(db: FakeScheduleD1): D1Database {
  return db as unknown as D1Database;
}

const DEFAULT_TASK = {
  kind: 'command',
  backend_id: 'backend-1',
  program: 'true',
  arguments: [],
  working_directory: '/tmp',
  timeout_seconds: 30,
  profile: 'full',
  output: {
    capture_stdout: true,
    capture_stderr: true,
    preview_max_bytes: 8192,
    retention_seconds: 86_400,
    hard_max_bytes: 10_485_760,
  },
};

export function seedSchedule(
  db: FakeScheduleD1,
  partial: Partial<FakeScheduleRow> & Pick<FakeScheduleRow, 'id' | 'backend_id'>,
): FakeScheduleRow {
  const now = new Date().toISOString();
  const row: FakeScheduleRow = {
    id: partial.id,
    backend_id: partial.backend_id,
    name: partial.name ?? 'test-schedule',
    cron: partial.cron ?? '0 * * * *',
    timezone: partial.timezone ?? 'UTC',
    task_json: partial.task_json ?? JSON.stringify(DEFAULT_TASK),
    enabled: partial.enabled ?? 1,
    revision: partial.revision ?? 1,
    policy_json:
      partial.policy_json ??
      JSON.stringify({
        concurrency: 'forbid',
        misfire: 'run_once',
        max_catchup_runs: 1,
      }),
    idempotency_key: partial.idempotency_key ?? null,
    request_hash: partial.request_hash ?? null,
    last_run_at: partial.last_run_at ?? null,
    next_run_at: partial.next_run_at ?? null,
    created_at: partial.created_at ?? now,
    updated_at: partial.updated_at ?? now,
  };
  db.schedules.push(row);
  return row;
}
