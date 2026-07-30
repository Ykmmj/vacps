declare module "vacps:http" {
  export interface ServerOptions {
    readonly host?: string;
    readonly port?: number;
  }

  /**
   * Inbound HTTP transport (Asio/Beast). JS owns the instance.
   * Product routes stay in handleRequest — Server has zero business routes.
   */
  export interface Server {
    listen(): void;
    close(): void;
    isListening(): boolean;
  }

  /** Factory: create an inbound server (default host/port from process config). */
  export function createServer(options?: ServerOptions): Server;

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
