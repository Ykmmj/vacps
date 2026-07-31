declare module 'vacps:host' {
  /** Thin process info (not HTTP / SQL / process / fs / listen policy). */
  export function version(): string;
  export function dataDir(): string;
  export function nowMs(): number;
  export function platform(): string;
  /** Process environment variable, or null if unset. */
  export function getenv(name: string): string | null;
}
