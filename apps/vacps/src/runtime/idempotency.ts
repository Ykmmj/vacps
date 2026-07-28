import { createHash } from 'node:crypto';

export interface IdempotencyMeta {
  key: string;
  replayed: boolean;
  request_hash: string;
}

interface RecordEntry {
  requestHash: string;
  result: unknown;
  createdAt: number;
}

/**
 * In-memory idempotency for mutating tools on a single agent process.
 * Keyed by tool_name + idempotency_key; bound to a canonical request hash.
 */
export class IdempotencyStore {
  private readonly records = new Map<string, RecordEntry>();

  /**
   * @returns cached result when key+hash match; throws on conflict; null when first use
   */
  lookup(toolName: string, key: string | undefined, requestHash: string): unknown | null {
    if (!key) return null;
    const storeKey = `${toolName}\0${key}`;
    const existing = this.records.get(storeKey);
    if (!existing) return null;
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
    return existing.result;
  }

  store(toolName: string, key: string | undefined, requestHash: string, result: unknown): void {
    if (!key) return;
    this.records.set(`${toolName}\0${key}`, {
      requestHash,
      result,
      createdAt: Date.now(),
    });
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
}

export function hashRequest(payload: unknown): string {
  return createHash('sha256').update(stableStringify(payload)).digest('hex');
}

function stableStringify(value: unknown): string {
  if (value === null || typeof value !== 'object') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map((item) => stableStringify(item)).join(',')}]`;
  const record = value as Record<string, unknown>;
  const keys = Object.keys(record).sort();
  return `{${keys.map((key) => `${JSON.stringify(key)}:${stableStringify(record[key])}`).join(',')}}`;
}
