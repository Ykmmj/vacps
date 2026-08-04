/**
 * Vitest mock for vacps:store — real Store instances come from openMemoryStore().
 * Matches vacps:store surface: class Store with static open only.
 */

export type SqlParam = null | number | bigint | string | ArrayBuffer | Uint8Array;

/** Result cell encode: null | number | bigint | string | ArrayBuffer. */
export type SqlValue = null | number | bigint | string | ArrayBuffer;

export interface RunResult {
  readonly changes: number | bigint;
  readonly lastInsertRowid: number | bigint;
}

export type ExpectedChanges =
  | { exactly: number }
  | { atLeast: number }
  | { atMost: number };

export interface QueryOptions {
  /** Max rows returned; default 10_000. Exceeded → reject. */
  maxRows?: number;
  /** Approximate payload budget (columns + cells); exceeded → reject. */
  maxBytes?: number;
}

export interface TransactionStep {
  readonly sql: string;
  readonly params?: readonly SqlParam[];
  readonly type?: 'run' | 'query';
  /** Run steps only; invalid with type: 'query'. */
  readonly expectedChanges?: ExpectedChanges;
  /** Query steps only: max rows returned; exceeded → fail and rollback. */
  readonly maxRows?: number;
  /** Query steps only: approximate payload budget; exceeded → fail and rollback. */
  readonly maxBytes?: number;
}

export type Row = Record<string, unknown>;
export type TransactionResult = RunResult | Row[];

export type StoreOpenMode = 'read-only' | 'read-write' | 'read-write-create';

export interface StoreOpenOptions {
  mode?: StoreOpenMode;
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
    _options?: QueryOptions,
  ): Promise<Row[]> {
    throw new Error('unreachable');
  }
  transaction(_steps: readonly TransactionStep[]): Promise<TransactionResult[]> {
    throw new Error('unreachable');
  }
  close(): Promise<void> {
    throw new Error('unreachable');
  }
}
