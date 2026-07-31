/**
 * Store adapter over Node's built-in SQLite for unit tests (no vacps:store host).
 * Matches the async vacps:store surface (Promises) while staying in-process sync underneath.
 */
import { DatabaseSync } from 'node:sqlite';

import type {
  ExpectedChanges,
  RunResult,
  SqlParam,
  Store,
  TransactionResult,
  TransactionStep,
} from 'vacps:store';

function checkExpectedChanges(exp: ExpectedChanges, changes: number): void {
  if ('exactly' in exp && changes !== exp.exactly) {
    throw new Error(
      `store.transaction: expectedChanges exactly ${exp.exactly} but got ${changes}`,
    );
  }
  if ('atLeast' in exp && changes < exp.atLeast) {
    throw new Error(
      `store.transaction: expectedChanges atLeast ${exp.atLeast} but got ${changes}`,
    );
  }
  if ('atMost' in exp && changes > exp.atMost) {
    throw new Error(
      `store.transaction: expectedChanges atMost ${exp.atMost} but got ${changes}`,
    );
  }
}

export function openMemoryStore(): Store {
  const db = new DatabaseSync(':memory:');
  let closed = false;

  const store: Store = {
    get path(): string {
      return ':memory:';
    },
    get closed(): boolean {
      return closed;
    },
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
    async transaction(steps: readonly TransactionStep[]): Promise<TransactionResult[]> {
      db.exec('BEGIN IMMEDIATE;');
      try {
        const out: TransactionResult[] = [];
        for (const step of steps) {
          if (step.type === 'query') {
            const rows = db.prepare(step.sql).all(...((step.params ?? []) as SqlParam[]));
            if (step.expectedChanges) {
              // Node sqlite does not expose changes() after SELECT; treat as 0.
              checkExpectedChanges(step.expectedChanges, 0);
            }
            out.push(rows as Array<Record<string, unknown>>);
          } else {
            const info = db.prepare(step.sql).run(...((step.params ?? []) as SqlParam[]));
            const rr: RunResult = {
              changes: Number(info.changes),
              lastInsertRowid: Number(info.lastInsertRowid),
            };
            if (step.expectedChanges) {
              checkExpectedChanges(step.expectedChanges, rr.changes);
            }
            out.push(rr);
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
    async close(): Promise<void> {
      if (!closed) {
        db.close();
        closed = true;
      }
    },
  };
  return store;
}
