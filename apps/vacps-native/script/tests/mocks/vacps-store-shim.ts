/** Runtime shim — real Store instances come from openMemoryStore in tests. */
export type SqlParam = null | number | string | ArrayBuffer | Uint8Array;

export interface RunResult {
  readonly changes: number;
  readonly lastInsertRowid: number;
}

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

export function open(_path: string): Store {
  throw new Error("vacps:store.open is not available in unit tests; use openMemoryStore()");
}
