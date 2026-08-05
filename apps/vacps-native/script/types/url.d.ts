/**
 * WHATWG URL / URLSearchParams subset installed by vacps-native (Ada 4.x).
 *
 * Intentionally incomplete vs browsers/Node — declare only what the runtime
 * actually implements so TypeScript does not imply full DOM URL.
 *
 * Implemented:
 * - URL: parse absolute/relative+base, getters listed below, `search` setter,
 *   live `searchParams`, `canParse`, `toString` / `toJSON`
 * - URLSearchParams: string init, CRUD + sort + size, entries/keys/values,
 *   forEach, Symbol.iterator (for-of)
 *
 * Not implemented (will not typecheck / may throw or be missing at runtime):
 * - URL setters other than `search` (href, protocol, username, password, host,
 *   hostname, port, pathname, hash are getters only)
 * - URLSearchParams init from record / sequence / another URLSearchParams
 * - URL.parse static (use `new URL` + try/catch or `URL.canParse`)
 */

declare class URL {
  constructor(input: string, base?: string);

  readonly href: string;
  readonly origin: string;
  readonly protocol: string;
  /** Getter only — Ada-backed; no setter in this runtime. */
  readonly username: string;
  /** Getter only — Ada-backed; no setter in this runtime. */
  readonly password: string;
  readonly host: string;
  readonly hostname: string;
  readonly port: string;
  readonly pathname: string;
  /** Live getter/setter; assignment re-parses into `searchParams`. */
  search: string;
  readonly hash: string;
  /** Same live object for the lifetime of this URL (identity-stable). */
  readonly searchParams: URLSearchParams;

  toString(): string;
  toJSON(): string;

  static canParse(input: string, base?: string): boolean;
}

declare class URLSearchParams {
  /**
   * `init` omitted / null / undefined → empty.
   * String only (leading `?` allowed). Record/sequence init is not supported.
   */
  constructor(init?: string | null);

  readonly size: number;

  append(name: string, value: string): void;
  /** Optional value matches WHATWG delete(name, value?). */
  delete(name: string, value?: string): void;
  get(name: string): string | null;
  getAll(name: string): string[];
  has(name: string, value?: string): boolean;
  set(name: string, value: string): void;
  sort(): void;
  toString(): string;

  entries(): IterableIterator<[string, string]>;
  keys(): IterableIterator<string>;
  values(): IterableIterator<string>;
  forEach(
    callback: (value: string, name: string, parent: URLSearchParams) => void,
    thisArg?: unknown,
  ): void;
  [Symbol.iterator](): IterableIterator<[string, string]>;
}
