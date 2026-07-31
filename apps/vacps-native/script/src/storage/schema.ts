import type { Store } from 'vacps:store';

/** Apply ordered schema migrations (idempotent). */
export async function migrateAgentDb(db: Store): Promise<void> {
  await db.exec(`
    PRAGMA foreign_keys = ON;
    CREATE TABLE IF NOT EXISTS schema_migrations (
      version INTEGER PRIMARY KEY NOT NULL,
      applied_at TEXT NOT NULL
    );
  `);

  const applied = new Set(
    (await db.query('SELECT version FROM schema_migrations;')).map((r) => Number(r['version'])),
  );

  const migrations: Array<{ version: number; sql: string }> = [
    {
      version: 1,
      sql: `
        CREATE TABLE IF NOT EXISTS agent_state (
          key TEXT PRIMARY KEY NOT NULL,
          value TEXT NOT NULL
        );
      `,
    },
    {
      version: 2,
      sql: `
        CREATE TABLE IF NOT EXISTS tasks (
          id TEXT PRIMARY KEY NOT NULL,
          backend_id TEXT NOT NULL,
          kind TEXT NOT NULL,
          status TEXT NOT NULL,
          profile TEXT,
          input_json TEXT NOT NULL,
          result_json TEXT,
          error_json TEXT,
          cancel_requested INTEGER NOT NULL DEFAULT 0,
          created_at TEXT NOT NULL,
          started_at TEXT,
          finished_at TEXT,
          updated_at TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS tasks_status_created
          ON tasks(status, created_at);
        CREATE TABLE IF NOT EXISTS task_logs (
          task_id TEXT NOT NULL,
          sequence INTEGER NOT NULL,
          stream TEXT NOT NULL,
          data TEXT NOT NULL,
          created_at TEXT NOT NULL,
          PRIMARY KEY (task_id, sequence),
          FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS request_nonces (
          nonce TEXT PRIMARY KEY NOT NULL,
          expires_at INTEGER NOT NULL
        );
      `,
    },
    {
      version: 3,
      sql: `
        CREATE TABLE IF NOT EXISTS task_idempotency (
          idempotency_key TEXT PRIMARY KEY NOT NULL,
          task_id TEXT NOT NULL,
          created_at TEXT NOT NULL,
          FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE
        );
      `,
    },
    {
      version: 4,
      sql: `
        CREATE TABLE IF NOT EXISTS schedulers (
          id TEXT PRIMARY KEY NOT NULL,
          cron TEXT NOT NULL,
          timezone TEXT NOT NULL,
          enabled INTEGER NOT NULL DEFAULT 1,
          task_json TEXT NOT NULL,
          last_fired_minute TEXT,
          updated_at TEXT NOT NULL
        );
      `,
    },
    {
      version: 5,
      sql: `
        ALTER TABLE schedulers ADD COLUMN next_run_at TEXT;
      `,
    },
    {
      version: 6,
      sql: `
        ALTER TABLE schedulers ADD COLUMN revision INTEGER NOT NULL DEFAULT 1;
        ALTER TABLE schedulers ADD COLUMN last_claimed_at TEXT;
        ALTER TABLE schedulers ADD COLUMN policy_json TEXT;
        ALTER TABLE tasks ADD COLUMN schedule_id TEXT;
        ALTER TABLE tasks ADD COLUMN schedule_revision INTEGER;
        ALTER TABLE tasks ADD COLUMN scheduled_for_ms INTEGER;
        CREATE UNIQUE INDEX IF NOT EXISTS tasks_schedule_occurrence_unique
          ON tasks(schedule_id, schedule_revision, scheduled_for_ms)
          WHERE schedule_id IS NOT NULL AND scheduled_for_ms IS NOT NULL;
      `,
    },
  ];

  for (const m of migrations) {
    if (applied.has(m.version)) continue;
    await db.begin();
    try {
      await db.exec(m.sql);
      await db.run('INSERT INTO schema_migrations(version, applied_at) VALUES(?, ?);', [
        m.version,
        new Date().toISOString(),
      ]);
      await db.commit();
    } catch (e) {
      await db.rollback();
      throw e;
    }
  }
}
