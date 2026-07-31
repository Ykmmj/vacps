/**
 * Store adapter over Node's built-in SQLite for unit tests (no vacps:store host).
 * Matches the async vacps:store surface (Promises) while staying in-process sync underneath.
 */
import { DatabaseSync } from 'node:sqlite';

import type { RunResult, SqlParam, Store } from 'vacps:store';

export function openMemoryStore(): Store {
  const db = new DatabaseSync(':memory:');
  return {
    async exec(sql: string): Promise<void> {
      db.exec(sql);
    },
    async run(sql: string, params: readonly SqlParam[] = []): Promise<RunResult> {
      const info = db.prepare(sql).run(...(params as SqlParam[]));
      return {
        changes: Number(info.changes),
        lastInsertRowid: Number(info.lastInsertRowid),
      };
    },
    async query(
      sql: string,
      params: readonly SqlParam[] = [],
    ): Promise<Array<Record<string, unknown>>> {
      const rows = db.prepare(sql).all(...(params as SqlParam[]));
      return rows as Array<Record<string, unknown>>;
    },
    async transaction(steps) {
      db.exec('BEGIN IMMEDIATE;');
      try {
        const out: RunResult[] = [];
        for (const step of steps) {
          if (step.exec) {
            db.exec(step.sql);
            out.push({ changes: 0, lastInsertRowid: 0 });
          } else {
            const info = db.prepare(step.sql).run(...((step.params ?? []) as SqlParam[]));
            out.push({
              changes: Number(info.changes),
              lastInsertRowid: Number(info.lastInsertRowid),
            });
          }
        }
        db.exec('COMMIT;');
        return out;
      } catch (e) {
        try {
          db.exec('ROLLBACK;');
        } catch {
          /* ignore */
        }
        throw e;
      }
    },
    async begin(): Promise<void> {
      db.exec('BEGIN IMMEDIATE;');
    },
    async commit(): Promise<void> {
      db.exec('COMMIT;');
    },
    async rollback(): Promise<void> {
      db.exec('ROLLBACK;');
    },
    path(): string {
      return ':memory:';
    },
    async close(): Promise<void> {
      db.close();
    },
  };
}
