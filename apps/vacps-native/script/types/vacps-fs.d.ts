declare module "vacps:fs" {
  export interface DirEntry {
    readonly name: string;
    readonly isDir: boolean;
    readonly isFile: boolean;
    readonly size: number;
  }

  export interface FileStat {
    readonly path: string;
    readonly type: "file" | "directory" | "symlink" | "other" | string;
    readonly size: number;
    readonly mtimeMs: number;
    readonly readable: boolean;
    readonly writable: boolean;
    readonly isSymlink: boolean;
  }

  /** Content I/O: Asio stream_file when io_uring available; else thread_pool. */
  export function readText(path: string): Promise<string>;
  export function writeText(path: string, data: string): Promise<void>;
  export function appendText(path: string, data: string): Promise<void>;
  export function readBytes(path: string): Promise<ArrayBuffer>;
  export function writeBytes(
    path: string,
    data: ArrayBuffer | Uint8Array,
  ): Promise<void>;

  /** Metadata / dirs: always offloaded to Host thread_pool. */
  export function mkdir(path: string): Promise<void>;
  export function exists(path: string): Promise<boolean>;
  export function stat(path: string): Promise<FileStat>;
  export function remove(path: string): Promise<void>;
  export function rename(from: string, to: string): Promise<void>;
  export function list(path: string): Promise<DirEntry[]>;
}
