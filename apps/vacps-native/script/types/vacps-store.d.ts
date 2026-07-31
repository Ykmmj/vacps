declare module 'vacps:store' {
  export type SqlParam = null | number | string | ArrayBuffer | Uint8Array;

  export interface RunResult {
    readonly changes: number;
    readonly lastInsertRowid: number;
  }

  /** One step inside store.transaction() — runs as a single db_pool unit. */
  export interface TransactionStep {
    readonly sql: string;
    readonly params?: readonly SqlParam[];
    /** When true, run as multi-statement exec (no binds). Default: run (bound DML). */
    readonly exec?: boolean;
  }

  /**
   * Store instance created by JS via open() — not a process singleton from C++.
   * All SQLite I/O runs on a serial host db thread; every mutator returns a Promise.
   *
   * Prefer transaction([...]) for multi-step atomic units. Do not interleave
   * begin()/run()/commit() across awaits — other requests can inject SQL between them.
   */
  export interface Store {
    exec(sql: string): Promise<void>;
    run(sql: string, params?: readonly SqlParam[]): Promise<RunResult>;
    query(sql: string, params?: readonly SqlParam[]): Promise<Array<Record<string, unknown>>>;
    /**
     * Atomic multi-step unit on the db thread: BEGIN IMMEDIATE + steps + COMMIT.
     * No other store op can interleave mid-transaction.
     */
    transaction(steps: readonly TransactionStep[]): Promise<RunResult[]>;
    /** @deprecated Prefer transaction([...]) for multi-step work. */
    begin(): Promise<void>;
    /** @deprecated Prefer transaction([...]) for multi-step work. */
    commit(): Promise<void>;
    /** @deprecated Prefer transaction([...]) for multi-step work. */
    rollback(): Promise<void>;
    /** Sync path string (no I/O). */
    path(): string;
    close(): Promise<void>;
  }

  /** Factory: open a new SQLite connection on the serial db pool. */
  export function open(path: string): Promise<Store>;
}
