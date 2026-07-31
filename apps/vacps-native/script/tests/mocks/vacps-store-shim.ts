/** Runtime shim — real Store instances come from openMemoryStore in tests. */
export type SqlParam = null | number | string | ArrayBuffer | Uint8Array;

export interface RunResult {
  readonly changes: number;
  readonly lastInsertRowid: number;
}

export interface TransactionStep {
  readonly sql: string;
  readonly params?: readonly SqlParam[];
  readonly exec?: boolean;
}

export interface Store {
  exec(sql: string): Promise<void>;
  run(sql: string, params?: readonly SqlParam[]): Promise<RunResult>;
  query(sql: string, params?: readonly SqlParam[]): Promise<Array<Record<string, unknown>>>;
  transaction(steps: readonly TransactionStep[]): Promise<RunResult[]>;
  begin(): Promise<void>;
  commit(): Promise<void>;
  rollback(): Promise<void>;
  path(): string;
  close(): Promise<void>;
}

export function open(_path: string): Promise<Store> {
  throw new Error('vacps:store.open is not available in unit tests; use openMemoryStore()');
}
