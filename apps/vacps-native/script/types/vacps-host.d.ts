/**
 * vacps:host — thin process info (not HTTP / SQL / process / fs / listen policy).
 *
 * getenv: design returns string | undefined (unset → undefined).
 * Existing script uses `??` / `=== null`; both remain type-safe with undefined.
 */
declare module 'vacps:host' {
  export function version(): string;
  export function platform(): string;
  export function dataDir(): string;
  export function nowMs(): number;
  /** Request the generic host lifecycle to run script shutdown and stop. */
  export function requestStop(): void;
  /**
   * Process environment variable via live getenv.
   * Unset → undefined; set empty → "".
   */
  export function getenv(name: string): string | undefined;
}
