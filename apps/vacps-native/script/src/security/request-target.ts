/**
 * Shared request-target helpers for vacps-request-v2 signatures.
 * Pure helpers (no vacps:* imports) so Node/Vitest can cover them.
 * Normalization matches Node/Worker via the WHATWG URL global.
 */

/**
 * Exact normalized request target from a URL string:
 * pathname + search (search includes leading `?` when non-empty).
 * Fragments are never signed.
 *
 * Matches apps/vacps and apps/control-worker requestTargetOf
 * (`new URL(url, 'http://vacps.invalid')` → pathname + search).
 */
export function requestTargetOf(url: string): string {
  const parsed = new URL(url, 'http://vacps.invalid');
  return `${parsed.pathname}${parsed.search}`;
}

/**
 * Build request target from already-split path and query (query without leading `?`).
 * Path is used as-is (no trailing-slash stripping) so it matches the pre-router raw path.
 */
export function requestTargetFromParts(path: string, query: string | undefined): string {
  const normalizedPath = path && path.length > 0 ? path : '/';
  if (!query) return normalizedPath;
  const q = query.startsWith('?') ? query.slice(1) : query;
  return q.length > 0 ? `${normalizedPath}?${q}` : normalizedPath;
}
