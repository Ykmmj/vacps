/**
 * Unified MCP Tool result envelope (Schema v2).
 * structuredContent is the machine-authoritative result; content[0].text mirrors it as compact JSON.
 */

export const SCHEMA_VERSION = '2.0';

export type ErrorCategory =
  | 'validation'
  | 'not_found'
  | 'conflict'
  | 'permission'
  | 'rate_limit'
  | 'timeout'
  | 'dependency'
  | 'unavailable'
  | 'internal';

export type SuccessEnvelopeBase = {
  ok: true;
  schema_version: typeof SCHEMA_VERSION;
  request_id: string;
  trace_id: string;
  generated_at: string;
  warnings: string[];
  backend_id?: string;
};

export type ErrorEnvelope = {
  ok: false;
  schema_version: typeof SCHEMA_VERSION;
  request_id: string;
  trace_id: string;
  generated_at: string;
  backend_id?: string;
  error: {
    code: string;
    message: string;
    category: ErrorCategory;
    retryable: boolean;
    retry_after_ms: number | null;
    details: Record<string, unknown>;
  };
};

export function newIds(): { request_id: string; trace_id: string } {
  return {
    request_id: crypto.randomUUID(),
    trace_id: crypto.randomUUID(),
  };
}

export function successMeta(extra?: { backend_id?: string; warnings?: string[] }): SuccessEnvelopeBase {
  const ids = newIds();
  return {
    ok: true,
    schema_version: SCHEMA_VERSION,
    request_id: ids.request_id,
    trace_id: ids.trace_id,
    generated_at: new Date().toISOString(),
    warnings: extra?.warnings ?? [],
    ...(extra?.backend_id ? { backend_id: extra.backend_id } : {}),
  };
}

export function categoryFor(code: string): ErrorCategory {
  if (
    code.includes('validation') ||
    code === 'invalid_request' ||
    code.startsWith('invalid_') ||
    code === 'path_not_allowed' ||
    code === 'path_not_directory' ||
    code === 'path_not_found'
  ) {
    return 'validation';
  }
  if (code.includes('not_found')) return 'not_found';
  if (
    code.includes('disabled') ||
    code.includes('conflict') ||
    code.includes('mismatch') ||
    code.includes('idempotency') ||
    code.includes('revision')
  ) {
    return 'conflict';
  }
  if (code.includes('unauthorized') || code.includes('forbidden') || code.includes('permission')) {
    return 'permission';
  }
  if (code.includes('rate_limit') || code.includes('too_many')) return 'rate_limit';
  if (code.includes('timeout') || code.includes('timed_out')) return 'timeout';
  if (code.includes('dependency')) return 'dependency';
  if (
    code.includes('unreachable') ||
    code.includes('unavailable') ||
    code === 'backend_request_failed'
  ) {
    return 'unavailable';
  }
  return 'internal';
}

export function isRetryable(code: string): boolean {
  return [
    'backend_unavailable',
    'backend_unreachable',
    'backend_request_failed',
    'rate_limit',
    'timeout',
    'timed_out',
  ].includes(code);
}

export function toolResultContent(structured: Record<string, unknown>): {
  type: 'text';
  text: string;
}[] {
  return [{ type: 'text' as const, text: JSON.stringify(structured) }];
}
