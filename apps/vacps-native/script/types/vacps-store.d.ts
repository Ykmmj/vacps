/**
 * vacps:store — SQLite capability (create-at-JS-call).
 *
 * ModuleCatalog registers this specifier and exports only class Store
 * (static open; no free open(); no begin/commit/rollback).
 *
 * Ownership: JS opaque is ClassHolder → shared_ptr<module-private StoreNative>;
 * async frames / workers retain that owner. Explicit close() is the awaitable,
 * run_blocking, reportable path. ClassBuilder finalizer only deletes the
 * holder (drops one shared_ptr); domain ~Store is a noexcept best-effort RAII
 * fallback and does not call QuickJS or invoke close() as a business method.
 */
declare module 'vacps:store' {
  /**
   * SQL bind values.
   * - `number`: finite; integral values must be JS safe integers (→ INTEGER),
   *   non-integrals → REAL. Unsafe integral Numbers are rejected (use bigint).
   * - `bigint`: accepted only when the value fits signed int64 (SQLite INTEGER).
   */
  export type SqlParam = null | number | bigint | string | ArrayBuffer | Uint8Array;

  /**
   * Result cell encode: null | number | bigint | string | ArrayBuffer.
   * INTEGER / rowid: Number when inside JS safe-integer range, otherwise bigint
   * so the full signed int64 range round-trips.
   */
  export type SqlValue = null | number | bigint | string | ArrayBuffer;

  export type StoreOpenMode = 'read-only' | 'read-write' | 'read-write-create';

  export interface StoreOpenOptions {
    mode?: StoreOpenMode;
  }

  export interface QueryOptions {
    /** Max rows returned; default 10_000. Exceeded → reject. */
    maxRows?: number;
    /** Approximate payload budget (columns + cells); exceeded → reject. */
    maxBytes?: number;
  }

  /** Integer fields: Number in safe-integer range, otherwise bigint. */
  export interface RunResult {
    readonly changes: number | bigint;
    readonly lastInsertRowid: number | bigint;
  }

  /**
   * Exactly one key must be present: exactly | atLeast | atMost
   * (nonnegative safe integer). Checked after each run step's changes().
   */
  export type ExpectedChanges =
    | { exactly: number }
    | { atLeast: number }
    | { atMost: number };

  /**
   * One step inside Store.transaction() — whole array is one atomic unit.
   * type: 'run' (default, bound DML/DDL) or 'query'.
   *
   * Cross-field validation at bind decode:
   * - expectedChanges only on run (invalid on type: 'query')
   * - maxRows / maxBytes only on query (invalid on run)
   */
  export interface TransactionStep {
    readonly sql: string;
    readonly params?: readonly SqlParam[];
    readonly type?: 'run' | 'query';
    /**
     * Fail-and-rollback if sqlite changes() do not match (run steps only).
     * Invalid on type: 'query' — binding rejects with a clear error.
     */
    readonly expectedChanges?: ExpectedChanges;
    /** Query steps only: max rows returned; exceeded → fail and rollback. */
    readonly maxRows?: number;
    /** Query steps only: approximate payload budget; exceeded → fail and rollback. */
    readonly maxBytes?: number;
  }

  export type Row = Record<string, unknown>;

  /** Per-step result of transaction(); run → RunResult, query → Row[]. */
  export type TransactionResult = RunResult | Row[];

  /**
   * Store instance created by JS via Store.open — not a process singleton.
   * All SQLite I/O is serialized per Store; every mutator returns a Promise.
   *
   * Prefer transaction([...]) for multi-step atomic units.
   *
   * SQL bind values: null | finite number | bigint (signed int64) | string |
   * ArrayBuffer | Uint8Array. Rejects undefined, boolean, non-finite number,
   * unsafe integral Number, and bigint outside signed int64.
   * transaction([]) is rejected synchronously at decode.
   */
  export class Store {
    private constructor();

    static open(path: string, options?: StoreOpenOptions): Promise<Store>;

    /** Configured path returned by Store (as passed to open). */
    readonly path: string;

    /** True after close() completes (and after destructor teardown). */
    readonly closed: boolean;

    exec(sql: string): Promise<void>;
    run(sql: string, params?: readonly SqlParam[]): Promise<RunResult>;

    /**
     * Fixed signature: query(sql, params?, options?).
     * Second argument is always params (omit/null/undefined → no binds);
     * third is QueryOptions. There is no query(sql, options) form.
     */
    query(
      sql: string,
      params?: readonly SqlParam[],
      options?: QueryOptions,
    ): Promise<Row[]>;

    /**
     * Atomic multi-step unit: BEGIN IMMEDIATE + steps + COMMIT.
     * expectedChanges is checked after each run step; mismatch → rollback.
     * expectedChanges on query steps is rejected at decode (not checked against changes()).
     */
    transaction(steps: readonly TransactionStep[]): Promise<TransactionResult[]>;

    /**
     * Release the sqlite connection. Idempotent.
     * Awaitable / run_blocking / errors reportable to JS.
     * Not invoked by the ClassBuilder GC finalizer (finalizer only drops ClassHolder).
     */
    close(): Promise<void>;
  }
}
