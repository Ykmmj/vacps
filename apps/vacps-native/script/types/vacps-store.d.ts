declare module 'vacps:store' {
  export type SqlParam = null | number | string | ArrayBuffer | Uint8Array;

  export interface RunResult {
    readonly changes: number;
    readonly lastInsertRowid: number;
  }

  /**
   * Store instance created by JS via open() — not a process singleton from C++.
   */
  export interface Store {
    exec(sql: string): void;
    run(sql: string, params?: readonly SqlParam[]): RunResult;
    query(sql: string, params?: readonly SqlParam[]): Array<Record<string, unknown>>;
    begin(): void;
    commit(): void;
    rollback(): void;
    path(): string;
    close(): void;
  }

  /** Factory: create a new SQLite connection (C++ capability, JS owns the instance). */
  export function open(path: string): Store;
}
