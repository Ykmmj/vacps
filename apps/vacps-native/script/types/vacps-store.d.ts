/**
 * vacps:store — SQLite capability (create-at-JS-call).
 *
 * Class Store with static open only. No free open(); no begin/commit/rollback.
 */
declare module 'vacps:store' {
  export type SqlParam = null | number | string | ArrayBuffer | Uint8Array;
  export type SqlValue = SqlParam;

  export type StoreOpenMode = 'read-only' | 'read-write' | 'read-write-create';

  export interface StoreOpenOptions {
    mode?: StoreOpenMode;
  }

  export interface QueryOptions {
    maxRows?: number;
    maxBytes?: number;
  }

  export interface RunResult {
    readonly changes: number;
    readonly lastInsertRowid: number;
  }

  export type ExpectedChanges =
    | { exactly: number }
    | { atLeast: number }
    | { atMost: number };

  /**
   * One step inside Store.transaction() — runs as a single db-pool unit.
   * type: 'run' (default, bound DML/DDL) or 'query'.
   */
  export interface TransactionStep {
    readonly sql: string;
    readonly params?: readonly SqlParam[];
    readonly type?: 'run' | 'query';
    /** Fail-and-rollback if changes do not match (checked after each step). */
    readonly expectedChanges?: ExpectedChanges;
  }

  export type Row = Record<string, unknown>;

  /** Per-step result of transaction(); run → RunResult, query → Row[]. */
  export type TransactionResult = RunResult | Row[];

  /**
   * Store instance created by JS via Store.open — not a process singleton.
   * All SQLite I/O is serialized per Store; every mutator returns a Promise.
   *
   * Prefer transaction([...]) for multi-step atomic units.
   */
  export class Store {
    private constructor();

    static open(path: string, options?: StoreOpenOptions): Promise<Store>;

    /** Absolute DB path. */
    readonly path: string;

    /** True after close(). */
    readonly closed: boolean;

    exec(sql: string): Promise<void>;
    run(sql: string, params?: readonly SqlParam[]): Promise<RunResult>;
    query(
      sql: string,
      params?: readonly SqlParam[],
      options?: QueryOptions,
    ): Promise<Row[]>;

    /**
     * Atomic multi-step unit: BEGIN IMMEDIATE + steps + COMMIT.
     * expectedChanges is checked after each step; mismatch → rollback.
     */
    transaction(steps: readonly TransactionStep[]): Promise<TransactionResult[]>;

    close(): Promise<void>;
  }
}
