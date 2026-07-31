/**
 * vacps:fs — filesystem capability (create-at-JS-call).
 *
 * Primary handle API: File (flags = numeric O_* / Asio file_base bitmask).
 * Namespace ops: mkdir / remove / rename / stat / exists / readDirectory.
 * Path allowlist is JS path-guard only (not C++).
 *
 * Content I/O is File only — no free path-level content helpers.
 * No string OpenMode — use O_RDONLY | O_CREAT | … or FileOpenOptions.flags.
 */
declare module 'vacps:fs' {
  // ── Open flags (Asio file_base / POSIX O_* numeric values on Linux) ─
  export const O_RDONLY: number;
  export const O_WRONLY: number;
  export const O_RDWR: number;
  export const O_CREAT: number;
  export const O_TRUNC: number;
  export const O_APPEND: number;
  export const O_EXCL: number;
  export const O_CLOEXEC: number;

  /** Optional object form of File.open (flags number form is preferred). */
  export interface FileOpenOptions {
    flags: number;
    /** Unix mode bits when creating (default 0o644; pool-backend open only). */
    mode?: number;
  }

  export interface ReadTextOptions {
    maxBytes?: number;
  }

  export interface RemoveOptions {
    recursive?: boolean;
  }

  export interface RenameOptions {
    /** When true, replace existing target (renameat2 RENAME_EXCHANGE / replace). */
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
   * otherwise thread_pool + sync I/O.
   */
  export class File {
    private constructor();

    /** Node-like: File.open(path, O_RDWR | O_CREAT, 0o644?) */
    static open(path: string, flags: number, mode?: number): Promise<File>;
    /** Object form: File.open(path, { flags, mode? }) */
    static open(path: string, options: FileOpenOptions): Promise<File>;

    readonly path: string;
    /** Open flags used at open (Asio Flags / O_* numeric values). */
    readonly flags: number;
    readonly closed: boolean;

    read(maxBytes?: number): Promise<Uint8Array>;
    readAt(offset: number, maxBytes: number): Promise<Uint8Array>;
    readText(options?: ReadTextOptions): Promise<string>;

    write(data: ArrayBufferView | ArrayBuffer): Promise<number>;
    writeAt(offset: number, data: ArrayBufferView | ArrayBuffer): Promise<number>;
    writeText(text: string): Promise<number>;

    truncate(size: number): Promise<void>;
    stat(): Promise<FileStat>;
    flush(): Promise<void>;
    close(): Promise<void>;
  }

  // ── Namespace ops ───────────────────────────────────────────────

  export function mkdir(
    path: string,
    options?: { recursive?: boolean; permissions?: number },
  ): Promise<void>;

  export function remove(path: string, options?: RemoveOptions): Promise<void>;

  export function rename(from: string, to: string, options?: RenameOptions): Promise<void>;

  export function stat(path: string): Promise<FileStat>;
  export function exists(path: string): Promise<boolean>;
  export function readDirectory(path: string): Promise<DirectoryEntry[]>;
}
