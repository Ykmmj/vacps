/**
 * Vitest mock for vacps:store — real Store instances come from openMemoryStore().
 * Matches vacps:store surface: class Store with static open only.
 */

export type SqlParam = null | number | string | ArrayBuffer | Uint8Array;

export interface RunResult {
  readonly changes: number;
  readonly lastInsertRowid: number;
}

export type ExpectedChanges =
  | { exactly: number }
  | { atLeast: number }
  | { atMost: number };

export interface TransactionStep {
  readonly sql: string;
  readonly params?: readonly SqlParam[];
  readonly type?: 'run' | 'query';
  readonly expectedChanges?: ExpectedChanges;
}

export type Row = Record<string, unknown>;
export type TransactionResult = RunResult | Row[];

export interface StoreOpenOptions {
  mode?: 'read-only' | 'read-write' | 'read-write-create';
}

export class Store {
  private constructor() {
    throw new Error('Store cannot be constructed with new; use Store.open(path, options?)');
  }

  static open(_path: string, _options?: StoreOpenOptions): Promise<Store> {
    throw new Error(
      'vacps:store Store.open is not available in unit tests; use openMemoryStore()',
    );
  }

  readonly path!: string;
  readonly closed!: boolean;

  exec(_sql: string): Promise<void> {
    throw new Error('unreachable');
  }
  run(_sql: string, _params?: readonly SqlParam[]): Promise<RunResult> {
    throw new Error('unreachable');
  }
  query(
    _sql: string,
    _params?: readonly SqlParam[],
  ): Promise<Array<Record<string, unknown>>> {
    throw new Error('unreachable');
  }
  transaction(_steps: readonly TransactionStep[]): Promise<TransactionResult[]> {
    throw new Error('unreachable');
  }
  close(): Promise<void> {
    throw new Error('unreachable');
  }
}
