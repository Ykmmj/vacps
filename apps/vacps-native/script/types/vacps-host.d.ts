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
  /**
   * Process environment variable from bootstrap EnvironmentSnapshot.
   * Unset → undefined (design). Runtime may still yield null until C++ aligns;
   * callers should treat nullish as missing (`v == null` / `??`).
   */
  export function getenv(name: string): string | undefined;
}
