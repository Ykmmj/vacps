import type { ServerRequest } from 'vacps:http';

import type { HostRequest } from '../contracts/http';

/** Alias of native vacps:http ServerRequest (type-only; no runtime module load). */
export type InboundServerRequest = ServerRequest;

export type InboundRequestAdapter = (req: ServerRequest) => HostRequest;

function findHeader(headers: Readonly<Record<string, string>>, name: string): string | undefined {
  const want = name.toLowerCase();
  const direct = headers[want];
  if (direct !== undefined) return direct;
  for (const [k, v] of Object.entries(headers)) {
    if (k.toLowerCase() === want) return v;
  }
  return undefined;
}

/**
 * Create an adapter that maps native inbound ServerRequest → HostRequest.
 *
 * Owns a private deterministic request-id sequence for this instance.
 *
 * - Splits raw url on the first `?` into path + query (query without leading `?`,
 *   matching router parseQuery which also accepts a leading `?`).
 * - UTF-8-decodes body with TextDecoder at this product boundary.
 * - Passes headers through unchanged (router lowercases for its own map).
 * - requestId from x-request-id when non-empty/non-whitespace, else `req-<seq>`.
 */
export function createInboundRequestAdapter(): InboundRequestAdapter {
  let fallbackRequestIdSeq = 0;

  return (req: ServerRequest): HostRequest => {
    const qIdx = req.url.indexOf('?');
    const path = qIdx >= 0 ? req.url.slice(0, qIdx) : req.url;
    // Without leading '?'; parseQuery also strips one if present.
    const query = qIdx >= 0 ? req.url.slice(qIdx + 1) : '';

    const idHeader = findHeader(req.headers, 'x-request-id');
    const trimmedId = idHeader?.trim();
    const requestId =
      trimmedId !== undefined && trimmedId.length > 0 ? trimmedId : `req-${++fallbackRequestIdSeq}`;

    const body = new TextDecoder('utf-8').decode(req.body);

    return {
      method: req.method,
      path,
      query,
      headers: req.headers,
      body,
      requestId,
    };
  };
}
