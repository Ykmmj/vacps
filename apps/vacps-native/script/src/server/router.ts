import * as log from 'vacps:log';

import type { HostRequest, HostResponse } from '../contracts/http';

/**
 * Minimal Fastify-shaped router for inbound JS onRequest callbacks.
 * Native transport has zero product routes; script registers them with app.get/post/…
 *
 * Contract: Wide at the HTTP boundary — malformed URI encoding and unexpected
 * handler failures are mapped to stable status codes (never crash the callback).
 */

export type RouteParams = Record<string, string>;
export type QueryParams = Record<string, string>;

export interface AppRequest {
  method: string;
  /** Path without query (normalized, no trailing slash except `/`). */
  url: string;
  path: string;
  headers: Record<string, string>;
  /** Parsed JSON when Content-Type is json; otherwise raw string. */
  body: unknown;
  params: RouteParams;
  query: QueryParams;
  requestId: string;
  raw: HostRequest;
}

export class Reply {
  statusCode = 200;
  private payload: unknown = undefined;
  private _sent = false;

  get sent(): boolean {
    return this._sent;
  }

  code(status: number): this {
    this.statusCode = status;
    return this;
  }

  send(body?: unknown): this {
    this.payload = body;
    this._sent = true;
    return this;
  }

  /** Fastify-style: handler may return a value instead of reply.send. */
  toHostResponse(handlerResult?: unknown): HostResponse {
    const body = this._sent ? this.payload : handlerResult;
    if (body === undefined || body === null) {
      return {
        status: this.statusCode,
        headers: { 'content-type': 'application/json; charset=utf-8' },
        body: '',
      };
    }
    if (typeof body === 'string') {
      return {
        status: this.statusCode,
        headers: { 'content-type': 'text/plain; charset=utf-8' },
        body,
      };
    }
    return {
      status: this.statusCode,
      headers: { 'content-type': 'application/json; charset=utf-8' },
      body: JSON.stringify(body),
    };
  }
}

export type RouteHandler = (request: AppRequest, reply: Reply) => unknown | Promise<unknown>;

type HookName = 'preValidation';

interface CompiledRoute {
  method: string;
  keys: string[];
  pattern: RegExp;
  handler: RouteHandler;
}

const INVALID_JSON = Symbol('vacps.invalid_json');

class InvalidUriError extends Error {
  readonly code = 'invalid_uri';
  readonly statusCode = 400;
  constructor() {
    super('Malformed percent-encoding in request URI.');
    this.name = 'InvalidUriError';
  }
}

function compilePath(path: string): { keys: string[]; pattern: RegExp } {
  const keys: string[] = [];
  const parts = path.split('/').map((seg) => {
    if (seg.startsWith(':')) {
      keys.push(seg.slice(1));
      return '([^/]+)';
    }
    return seg.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  });
  return {
    keys,
    pattern: new RegExp(`^${parts.join('/')}$`),
  };
}

function normalizePath(path: string): string {
  const p = path.split('?')[0] ?? path;
  if (p.length > 1 && p.endsWith('/')) return p.slice(0, -1);
  return p || '/';
}

function headerMap(h: Readonly<Record<string, string>>): Record<string, string> {
  const out: Record<string, string> = {};
  for (const [k, v] of Object.entries(h)) out[k.toLowerCase()] = v;
  return out;
}

function decodeUriComponent(value: string): string {
  try {
    return decodeURIComponent(value);
  } catch {
    throw new InvalidUriError();
  }
}

function parseQuery(query: string): QueryParams {
  const out: QueryParams = {};
  const q = query.startsWith('?') ? query.slice(1) : query;
  if (!q) return out;
  for (const part of q.split('&')) {
    if (!part) continue;
    const eq = part.indexOf('=');
    const k = decodeUriComponent(eq >= 0 ? part.slice(0, eq) : part);
    const v = decodeUriComponent(eq >= 0 ? part.slice(eq + 1) : '');
    if (k) out[k] = v;
  }
  return out;
}

function parseBody(raw: HostRequest, headers: Record<string, string>): unknown {
  const ct = headers['content-type'] ?? '';
  if (!raw.body) return undefined;
  if (
    ct.includes('application/json') ||
    raw.body.trimStart().startsWith('{') ||
    raw.body.trimStart().startsWith('[')
  ) {
    try {
      return JSON.parse(raw.body);
    } catch {
      return INVALID_JSON;
    }
  }
  return raw.body;
}

function jsonError(status: number, code: string, message: string): HostResponse {
  return {
    status,
    headers: { 'content-type': 'application/json; charset=utf-8' },
    body: JSON.stringify({ error: { code, message } }),
  };
}

function logAndInternalError(
  requestId: string,
  method: string,
  path: string,
  error: unknown,
): HostResponse {
  const detail = error instanceof Error ? error.message : String(error);
  log.error(`request failed id=${requestId} method=${method} path=${path}: ${detail}`);
  return jsonError(500, 'internal_error', 'Internal server error.');
}

export class App {
  private readonly routes: CompiledRoute[] = [];
  private readonly hooks: Partial<Record<HookName, RouteHandler[]>> = {};

  addHook(name: HookName, handler: RouteHandler): this {
    const list = this.hooks[name] ?? [];
    list.push(handler);
    this.hooks[name] = list;
    return this;
  }

  get(path: string, handler: RouteHandler): this {
    return this.route('GET', path, handler);
  }

  post(path: string, handler: RouteHandler): this {
    return this.route('POST', path, handler);
  }

  put(path: string, handler: RouteHandler): this {
    return this.route('PUT', path, handler);
  }

  delete(path: string, handler: RouteHandler): this {
    return this.route('DELETE', path, handler);
  }

  route(method: string, path: string, handler: RouteHandler): this {
    const { keys, pattern } = compilePath(normalizePath(path));
    this.routes.push({
      method: method.toUpperCase(),
      keys,
      pattern,
      handler,
    });
    return this;
  }

  async handleRequest(raw: HostRequest): Promise<HostResponse> {
    const method = raw.method.toUpperCase();
    const path = normalizePath(raw.path);
    const headers = headerMap(raw.headers);
    const body = parseBody(raw, headers);
    const reply = new Reply();

    if (body === INVALID_JSON) {
      reply.code(400).send({
        error: { code: 'invalid_json', message: 'Body must be JSON.' },
      });
      return reply.toHostResponse();
    }

    let query: QueryParams;
    try {
      query = parseQuery(raw.query ?? '');
    } catch (error) {
      if (error instanceof InvalidUriError) {
        return jsonError(400, 'invalid_uri', error.message);
      }
      return logAndInternalError(raw.requestId, method, path, error);
    }

    let match: CompiledRoute | undefined;
    let params: RouteParams = {};
    try {
      for (const r of this.routes) {
        if (r.method !== method) continue;
        const m = r.pattern.exec(path);
        if (!m) continue;
        match = r;
        params = {};
        for (let i = 0; i < r.keys.length; i++) {
          const key = r.keys[i]!;
          params[key] = decodeUriComponent(m[i + 1] ?? '');
        }
        break;
      }
    } catch (error) {
      if (error instanceof InvalidUriError) {
        return jsonError(400, 'invalid_uri', error.message);
      }
      return logAndInternalError(raw.requestId, method, path, error);
    }

    const request: AppRequest = {
      method,
      url: path,
      path,
      headers,
      body,
      params,
      query,
      requestId: raw.requestId,
      raw,
    };

    for (const hook of this.hooks.preValidation ?? []) {
      try {
        const out = await hook(request, reply);
        if (reply.sent) return reply.toHostResponse();
        if (out !== undefined && out !== null) return reply.toHostResponse(out);
      } catch (error) {
        return logAndInternalError(request.requestId, request.method, request.path, error);
      }
    }

    if (!match) {
      reply.code(404).send({
        error: { code: 'not_found', message: 'route not found' },
      });
      return reply.toHostResponse();
    }

    try {
      const out = await match.handler(request, reply);
      if (reply.sent) return reply.toHostResponse();
      return reply.toHostResponse(out);
    } catch (error) {
      return logAndInternalError(request.requestId, request.method, request.path, error);
    }
  }
}

export function createApp(): App {
  return new App();
}
