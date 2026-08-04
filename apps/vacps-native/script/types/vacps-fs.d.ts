/**
 * vacps:fs — filesystem capability (create-at-JS-call).
 *
 * Primary handle API: File (string OpenMode, bytes-first I/O).
 * Namespace ops: mkdir / remove / rename / stat / exists / readDirectory.
 * Pure I/O: no path allowlist in C++ or the module surface.
 *
 * Text encode/decode: use global TextEncoder / TextDecoder (not native methods).
 */
declare module 'vacps:fs' {
  /**
   * File open mode (mapped to POSIX open flags inside native File::open).
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
    /** Unix mode bits when creating (default 0o644). */
    permissions?: number;
  }

  export interface MkdirOptions {
    /** false/omit: create last component only; true: create intermediate dirs. */
    recursive?: boolean;
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
    /** True when the directory entry itself is a symlink (lstat). */
    readonly isSymlink: boolean;
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
   * otherwise worker-pool POSIX I/O. Per-handle ops are serialized by the JS
   * module FileHandle operation queue (domain File requires external serialization).
   *
   * Read limits: omitted maxBytes defaults to 16 MiB; values above 64 MiB
   * are rejected (hard cap). Bytes-first — use TextEncoder/TextDecoder for text.
   */
  export class File {
    private constructor();

    /** Canonical open: options object only (no mode-string overload). */
    static open(path: string, options: FileOpenOptions): Promise<File>;

    readonly path: string;
    /** Open mode used at open. */
    readonly mode: FileOpenMode;
    readonly closed: boolean;

    /** @param maxBytes default 16 MiB; hard reject above 64 MiB */
    read(maxBytes?: number): Promise<ArrayBuffer>;
    /** @param maxBytes hard reject above 64 MiB */
    readAt(offset: number, maxBytes: number): Promise<ArrayBuffer>;

    /**
     * Sequential / append write. On append-mode handles each underlying
     * write(2) is atomically positioned by O_APPEND; a multi-partial logical
     * buffer is not promised as one indivisible append.
     */
    write(data: ArrayBufferView | ArrayBuffer): Promise<number>;
    /**
     * Positioned write. Rejected for append-mode handles
     * (use write() with O_APPEND positioning per write syscall).
     */
    writeAt(offset: number, data: ArrayBufferView | ArrayBuffer): Promise<number>;

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
