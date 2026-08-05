/**
 * vacps:http — outbound request + inbound Server.
 *
 * Inbound: native transport raises each request into a JS onRequest callback.
 * Product routing / JSON policy stay in script — Server has zero business routes.
 * Outbound: pooled request() on Runtime::Async / host Asio.
 */
declare module 'vacps:http' {
  // ── Inbound Server ──────────────────────────────────────────────

  export interface ServerOptions {
    /**
     * Numeric IPv4/IPv6 bind literal only (never DNS / hostnames).
     * Validated synchronously at `new Server` via the native address parser;
     * e.g. `"localhost"` throws TypeError before any Promise is created.
     * Default `"127.0.0.1"`.
     */
    readonly host?: string;
    /** Bind port; 0 = ephemeral. */
    readonly port: number;
    /** Max request body bytes. */
    readonly maxRequestBytes?: number;
    /** Max header block bytes. */
    readonly maxHeaderBytes?: number;
    /** Max response body bytes accepted from the handler. */
    readonly maxResponseBytes?: number;
    /** Per read/write idle budget (ms). */
    readonly ioTimeoutMs?: number;
    /**
     * Wall-clock deadline (ms) for one onRequest invocation. Starts a timer
     * that requests cooperative cancellation; not a hard completion bound —
     * `close()` still waits for handler drain. Default 30000.
     */
    readonly handlerTimeoutMs?: number;
    /** listen(2) backlog. */
    readonly backlog?: number;
    readonly reuseAddress?: boolean;
  }

  export interface ServerRequest {
    readonly method: string;
    /** Request-target (path + optional query). */
    readonly url: string;
    /** HTTP version string (e.g. "1.0", "1.1"). */
    readonly httpVersion: string;
    readonly headers: Readonly<Record<string, string>>;
    readonly body: ArrayBuffer;
    readonly remoteAddress: string;
  }

  export interface ServerResponse {
    readonly status: number;
    readonly headers?: Readonly<Record<string, string>>;
    readonly body?: string | ArrayBuffer | ArrayBufferView;
  }

  export type ServerRequestHandler = (
    request: ServerRequest,
  ) => ServerResponse | Promise<ServerResponse>;

  export interface ListenAddress {
    /**
     * Raw numeric bound address (IPv6 is unbracketed).
     * Callers must format when building a URL
     * (e.g. IPv6 → `"[" + host + "]:" + port`).
     */
    readonly host: string;
    readonly port: number;
  }

  /**
   * Inbound HTTP/1 transport (Asio/Beast). Construct with options + onRequest;
   * bind in listen(). Native event → JS callback; no product routes here.
   */
  export class Server {
    constructor(options: ServerOptions, onRequest: ServerRequestHandler);

    /** True while accepting connections. */
    readonly listening: boolean;

    /** Effective bind address while listening; undefined otherwise. */
    readonly address: ListenAddress | undefined;

    /** Bind + accept. Resolves to the effective address (port may be ephemeral). */
    listen(): Promise<ListenAddress>;

    close(): Promise<void>;
  }

  // ── Outbound client ─────────────────────────────────────────────

  export interface HttpRequest {
    readonly method?: string;
    readonly url: string;
    readonly headers?: Readonly<Record<string, string>>;
    /**
     * Request body: UTF-8 string, ArrayBuffer, or TypedArray
     * (typed as ArrayBufferView).
     */
    readonly body?: string | ArrayBuffer | ArrayBufferView;
    /**
     * One absolute wall-clock budget for the whole request.
     * Range 1..3600000. Default 30000.
     */
    readonly timeoutMs?: number;
    /**
     * Max response body bytes (Beast body_limit while reading).
     * Range 1..67108864 (64 MiB). Default 8 MiB.
     */
    readonly maxResponseBytes?: number;
  }

  export interface HttpResponse {
    readonly status: number;
    readonly headers: Readonly<Record<string, string>>;
    readonly body: ArrayBuffer;
  }

  /**
   * Pooled outbound HTTP/HTTPS request. HTTP/1.1 connections are reused by
   * origin; one connection carries one active request at a time. The native
   * client bounds per-origin and global concurrency plus retained idle
   * descriptors.
   * Options are decoded synchronously before the Promise is created.
   * Runs on Runtime::Async / host Asio (direct path, not worker run_blocking).
   * Binary response body as ArrayBuffer. stop-token cancellation;
   * one timeoutMs budget; maxResponseBytes cap.
   * HTTPS: verify_peer + SNI + hostname verification; CA path from host
   * bootstrap composition (not a JS option). A failed stale connection is
   * discarded without transparent request replay. Does not follow redirects.
   */
  export function request(options: HttpRequest): Promise<HttpResponse>;
}
