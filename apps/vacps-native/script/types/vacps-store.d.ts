declare module 'vacps:store' {
  export type SqlParam = null | number | string | ArrayBuffer | Uint8Array;

  export interface RunResult {
    readonly changes: number;
    readonly lastInsertRowid: number;
  }

  /**
   * Store instance created by JS via open() — not a process singleton from C++.
   * All SQLite I/O runs on a serial host db thread; every mutator returns a Promise.
   */
  export interface Store {
    exec(sql: string): Promise<void>;
    run(sql: string, params?: readonly SqlParam[]): Promise<RunResult>;
    query(sql: string, params?: readonly SqlParam[]): Promise<Array<Record<string, unknown>>>;
    begin(): Promise<void>;
    commit(): Promise<void>;
    rollback(): Promise<void>;
    /** Sync path string (no I/O). */
    path(): string;
    close(): Promise<void>;
  }

  /** Factory: open a new SQLite connection on the serial db pool. */
  export function open(path: string): Promise<Store>;
}
