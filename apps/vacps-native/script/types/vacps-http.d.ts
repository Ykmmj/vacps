/**
 * vacps:http — inbound Server + outbound request (create-at-JS-call).
 *
 * Final API: class Server(options), listening, listen()/close() → Promise;
 * namespace request().
 */
declare module 'vacps:http' {
  export interface ServerOptions {
    readonly host?: string;
    /** Required. Bind port in range 1–65535. */
    readonly port: number;
  }

  /**
   * Inbound HTTP transport (Asio/Beast). JS owns the instance.
   * Product routes stay in handleRequest — Server has zero business routes.
   *
   * Constructor only stores config; bind happens in listen().
   */
  export class Server {
    constructor(options: ServerOptions);

    /** True while accepting connections. */
    readonly listening: boolean;

    listen(): Promise<void>;
    close(): Promise<void>;
  }

  // ── Outbound client (HTTP/HTTPS) ────────────────────────────────

  export interface HttpRequest {
    readonly method?: string;
    readonly url: string;
    readonly headers?: Readonly<Record<string, string>>;
    /** UTF-8 string or raw bytes. */
    readonly body?: string | ArrayBuffer | Uint8Array;
    /** Default 30000. */
    readonly timeoutMs?: number;
    /** Default 8 MiB. */
    readonly maxResponseBytes?: number;
  }

  /** Design alias. */
  export type HttpRequestOptions = HttpRequest;

  export interface HttpResponse {
    readonly status: number;
    readonly headers: Readonly<Record<string, string>>;
    readonly body: ArrayBuffer;
  }

  /**
   * One-shot outbound request (Boost.Beast + Asio SSL).
   * HTTPS: verify_peer + SNI + hostname verification; CA from VACPS_CA_BUNDLE
   * or platform defaults (fail-closed if missing). Does not follow redirects.
   */
  export function request(options: HttpRequest): Promise<HttpResponse>;
}
