/**
 * Store adapter over Node's built-in SQLite for unit tests (no vacps:store host).
 */
import { DatabaseSync } from "node:sqlite";

import type { RunResult, SqlParam, Store } from "vacps:store";

export function openMemoryStore(): Store {
  const db = new DatabaseSync(":memory:");
  return {
    exec(sql: string): void {
      db.exec(sql);
    },
    run(sql: string, params: readonly SqlParam[] = []): RunResult {
      const info = db.prepare(sql).run(...(params as SqlParam[]));
      return {
        changes: Number(info.changes),
        lastInsertRowid: Number(info.lastInsertRowid),
      };
    },
    query(sql: string, params: readonly SqlParam[] = []): Array<Record<string, unknown>> {
      const rows = db.prepare(sql).all(...(params as SqlParam[]));
      return rows as Array<Record<string, unknown>>;
    },
    begin(): void {
      db.exec("BEGIN IMMEDIATE;");
    },
    commit(): void {
      db.exec("COMMIT;");
    },
    rollback(): void {
      db.exec("ROLLBACK;");
    },
    path(): string {
      return ":memory:";
    },
    close(): void {
      db.close();
    },
  };
}
