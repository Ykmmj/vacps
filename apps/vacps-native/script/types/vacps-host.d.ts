declare module "vacps:host" {
  /** Thin process info (not HTTP / SQL / process / fs). */
  export function version(): string;
  export function dataDir(): string;
  export function listenHost(): string;
  export function listenPort(): number;
  export function nowMs(): number;
  export function platform(): string;
  /** Process environment variable, or null if unset. */
  export function getenv(name: string): string | null;
}
