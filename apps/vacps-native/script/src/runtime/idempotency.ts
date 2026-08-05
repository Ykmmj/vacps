import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';

export interface IdempotencyMeta {
  key: string;
  replayed: boolean;
  request_hash: string;
}

/** Fixed replay window from entry creation (not sliding). */
const TTL_MS = 10 * 60 * 1000;

/** Hard cap on live entries; eviction prefers oldest settled only. */
const CAPACITY = 32;

type StoreEntry =
  | {
      kind: 'pending';
      requestHash: string;
      createdAt: number;
      promise: Promise<unknown>;
    }
  | {
      kind: 'settled';
      requestHash: string;
      createdAt: number;
      result: unknown;
    };

/**
 * Bounded in-memory idempotency for mutating tools (apps/vacps IdempotencyStore).
 *
 * Semantics:
 * - One Map only; public API is execute / withIdempotencyMeta.
 * - True single-flight: same live key+hash joins a pending Promise or replays settled.
 * - Settled entries live for a fixed 10-minute window from creation (not sliding).
 * - Pending work is never TTL-expired.
 * - At most 32 live entries; expired settled are pruned, then oldest settled evicted.
 * - If all 32 slots are pending, a new keyed operation fails with 503.
 * - A live entry with a different request hash still yields 409 conflict.
 * - Failure removes the exact pending entry; all waiters observe the rejection.
 * - Non-keyed operations bypass the map.
 */
export class IdempotencyStore {
  private readonly records = new Map<string, StoreEntry>();

  /**
   * Run `run` under the idempotency key, or bypass the map when key is absent.
   * Contract: Wide for missing key / hash mismatch / capacity; single-flight join otherwise.
   */
  async execute<T>(
    toolName: string,
    key: string | undefined,
    requestHash: string,
    run: () => Promise<T>,
  ): Promise<{ result: T; replayed: boolean }> {
    if (!key) {
      return { result: await run(), replayed: false };
    }

    const nowMs = host.nowMs();
    this.pruneExpiredSettled(nowMs);
    const storeKey = `${toolName}\0${key}`;
    const existing = this.records.get(storeKey);
    if (existing) {
      if (existing.requestHash !== requestHash) {
        throw Object.assign(
          new Error('The idempotency key was previously used with different arguments.'),
          {
            code: 'idempotency_conflict',
            statusCode: 409,
            details: { tool_name: toolName, key },
          },
        );
      }
      if (existing.kind === 'pending') {
        const result = (await existing.promise) as T;
        return { result, replayed: true };
      }
      return { result: existing.result as T, replayed: true };
    }

    while (this.records.size >= CAPACITY) {
      if (!this.evictOldestSettled()) break;
    }
    if (this.records.size >= CAPACITY) {
      throw Object.assign(
        new Error('Idempotency store is at capacity with in-flight operations.'),
        {
          code: 'idempotency_capacity',
          statusCode: 503,
          details: { tool_name: toolName, key, capacity: CAPACITY },
        },
      );
    }

    const createdAt = host.nowMs();
    // Defer run via microtask so the pending entry is visible before the callback runs.
    // Promise.resolve().then(run) also turns a synchronous throw into rejection (no stuck pending).
    const pending: StoreEntry = {
      kind: 'pending',
      requestHash,
      createdAt,
      promise: Promise.resolve().then(run),
    };
    this.records.set(storeKey, pending);

    try {
      const result = (await pending.promise) as T;
      if (this.records.get(storeKey) === pending) {
        this.records.set(storeKey, {
          kind: 'settled',
          requestHash,
          createdAt,
          result,
        });
      }
      return { result, replayed: false };
    } catch (error) {
      if (this.records.get(storeKey) === pending) {
        this.records.delete(storeKey);
      }
      throw error;
    }
  }

  withIdempotencyMeta(
    key: string | undefined,
    requestHash: string,
    replayed: boolean,
    body: Record<string, unknown>,
  ): Record<string, unknown> {
    if (!key) return body;
    return {
      ...body,
      idempotency: {
        key,
        replayed,
        request_hash: requestHash,
      } satisfies IdempotencyMeta,
    };
  }

  /** Drop settled entries whose fixed creation TTL has elapsed. Pending is never expired. */
  private pruneExpiredSettled(nowMs: number): void {
    for (const [storeKey, entry] of this.records) {
      if (entry.kind !== 'settled') continue;
      if (nowMs - entry.createdAt >= TTL_MS) {
        this.records.delete(storeKey);
      }
    }
  }

  /** Evict one oldest settled entry (Map insertion order). Returns false if none. */
  private evictOldestSettled(): boolean {
    for (const [storeKey, entry] of this.records) {
      if (entry.kind !== 'settled') continue;
      this.records.delete(storeKey);
      return true;
    }
    return false;
  }
}

export function hashRequest(payload: unknown): string {
  return `sha256:${crypto.sha256Hex(stableStringify(payload))}`;
}

function stableStringify(value: unknown): string {
  if (value === null || typeof value !== 'object') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map((item) => stableStringify(item)).join(',')}]`;
  const record = value as Record<string, unknown>;
  const keys = Object.keys(record).sort();
  return `{${keys.map((key) => `${JSON.stringify(key)}:${stableStringify(record[key])}`).join(',')}}`;
}
