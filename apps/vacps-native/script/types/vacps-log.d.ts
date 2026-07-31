/**
 * vacps:log — structured log sink (spdlog).
 *
 * message: unknown (stringified by host). flush returns Promise (design);
 * fire-and-forget callers that ignore the return value still typecheck.
 */
declare module 'vacps:log' {
  export function trace(message: unknown): void;
  export function debug(message: unknown): void;
  export function info(message: unknown): void;
  export function warn(message: unknown): void;
  export function error(message: unknown): void;
  /** Flush buffers. Design: Promise; may resolve immediately. */
  export function flush(): Promise<void>;
}
