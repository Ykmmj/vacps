/**
 * vacps:fs — filesystem capability (create-at-JS-call).
 *
 * Primary handle API: File (string OpenMode).
 * Namespace ops: mkdir / remove / rename / stat / exists / readDirectory.
 * Path allowlist is JS path-guard only (not C++).
 *
 * Content I/O is File only — no free path-level content helpers.
 * OpenMode is a string union; O_* numeric flags are not exported.
 */
declare module 'vacps:fs' {
  /**
   * File open mode (mapped to Asio/POSIX open flags inside native File::open).
   * - "read"         — read-only (must exist)
   * - "read-write"   — read-write (must exist)
   * - "write"        — create/truncate write-only
   * - "write-new"    — create exclusive write-only (fails if exists)
   * - "append"       — create/append write-only
   * - "append-read"  — create/append read-write
   */
  export type FileOpenMode =
    | 'read'
    | 'read-write'
    | 'write'
    | 'write-new'
    | 'append'
    | 'append-read';

  export interface FileOpenOptions {
    mode: FileOpenMode;
    /** Unix mode bits when creating (default 0o644; pool-backend open only). */
    permissions?: number;
  }

  export interface ReadTextOptions {
    /** Default 16 MiB; hard reject above 64 MiB. */
    maxBytes?: number;
  }

  export interface MkdirOptions {
    /** false/omit: create last component only; true: create intermediate dirs. */
    recursive?: boolean;
    /** Unix mode bits when creating (host may ignore if unsupported). */
    permissions?: number;
  }

  export interface RemoveOptions {
    /** false/omit: file or empty directory only; true: remove tree. */
    recursive?: boolean;
  }

  export interface RenameOptions {
    /** false/omit: fail if target exists; true: allow overwrite. */
    replace?: boolean;
  }

  export interface DirEntry {
    readonly name: string;
    readonly isDir: boolean;
    readonly isFile: boolean;
    readonly size: number;
  }

  /** Design name; alias of DirEntry. */
  export type DirectoryEntry = DirEntry;

  export interface FileStat {
    readonly path: string;
    readonly type: 'file' | 'directory' | 'symlink' | 'other' | string;
    readonly size: number;
    readonly mtimeMs: number;
    readonly readable: boolean;
    readonly writable: boolean;
    readonly isSymlink: boolean;
  }

  /**
   * Opened file handle. Construct only via File.open (private constructor).
   * I/O is async; host uses Asio random_access_file (io_uring) when available,
   * otherwise thread_pool + sync I/O. Per-handle ops are serialized (strand);
   * concurrent read/write/close are safe (ordered, no data races).
   *
   * Read limits: omitted maxBytes defaults to 16 MiB; values above 64 MiB
   * are rejected (hard cap).
   */
  export class File {
    private constructor();

    /** File.open(path, "read" | …) */
    static open(path: string, mode: FileOpenMode): Promise<File>;
    /** File.open(path, { mode, permissions? }) */
    static open(path: string, options: FileOpenOptions): Promise<File>;

    readonly path: string;
    /** Open mode used at open. */
    readonly mode: FileOpenMode;
    readonly closed: boolean;

    /** @param maxBytes default 16 MiB; hard reject above 64 MiB */
    read(maxBytes?: number): Promise<Uint8Array>;
    /** @param maxBytes hard reject above 64 MiB */
    readAt(offset: number, maxBytes: number): Promise<Uint8Array>;
    /** options.maxBytes default 16 MiB; hard reject above 64 MiB */
    readText(options?: ReadTextOptions): Promise<string>;

    write(data: ArrayBufferView | ArrayBuffer): Promise<number>;
    writeAt(offset: number, data: ArrayBufferView | ArrayBuffer): Promise<number>;
    writeText(text: string): Promise<number>;

    truncate(size: number): Promise<void>;
    stat(): Promise<FileStat>;
    flush(): Promise<void>;
    /** Idempotent; concurrent close with I/O is safe (I/O may fail with closed). */
    close(): Promise<void>;
  }

  // ── Namespace ops ───────────────────────────────────────────────

  export function mkdir(path: string, options?: MkdirOptions): Promise<void>;

  export function remove(path: string, options?: RemoveOptions): Promise<void>;

  export function rename(from: string, to: string, options?: RenameOptions): Promise<void>;

  export function stat(path: string): Promise<FileStat>;
  /**
   * true if path exists; false if not found (ENOENT/ENOTDIR).
   * Rejects on permission and other I/O errors (does not treat them as false).
   */
  export function exists(path: string): Promise<boolean>;
  export function readDirectory(path: string): Promise<DirectoryEntry[]>;
}
