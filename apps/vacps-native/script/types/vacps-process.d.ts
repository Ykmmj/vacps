declare module 'vacps:process' {
  export interface RunOptions {
    readonly cwd?: string;
    readonly timeoutMs?: number;
    /** Cap retained stdout (default 16 MiB). Excess discarded; stdoutTruncated set. */
    readonly maxStdoutBytes?: number;
    /** Cap retained stderr (default 16 MiB). */
    readonly maxStderrBytes?: number;
  }

  export interface RunResult {
    readonly exitCode: number;
    readonly timedOut: boolean;
    readonly stdout: string;
    readonly stderr: string;
    /** Bytes observed on stdout (includes discarded after cap). */
    readonly stdoutProduced?: number;
    readonly stderrProduced?: number;
    readonly stdoutTruncated?: boolean;
    readonly stderrTruncated?: boolean;
  }

  export interface StartOptions {
    readonly cwd?: string;
    readonly timeoutMs?: number;
    /** Default true. */
    readonly closeStdin?: boolean;
    /** Cap accumulated stdout bytes (process continues after cap). */
    readonly hardMaxStdout?: number;
    /** Cap accumulated stderr bytes. */
    readonly hardMaxStderr?: number;
  }

  export interface StartResult {
    readonly id: string;
    readonly pid: number;
  }

  export interface ReadOptions {
    readonly waitMs?: number;
    readonly maxBytes?: number;
    readonly stdoutOffset?: number;
    readonly stderrOffset?: number;
  }

  export interface ReadResult {
    readonly status: 'running' | 'exited' | 'timed_out' | 'cancelled' | string;
    readonly exitCode: number;
    readonly timedOut: boolean;
    /** True only when process exited, both pipes EOF, and no unread buffer. */
    readonly eof: boolean;
    readonly stdinOpen: boolean;
    readonly stdout: string;
    readonly stderr: string;
    /** Bytes retained in the agent buffer (capped by hard_max). */
    readonly stdoutTotal: number;
    readonly stderrTotal: number;
    /** Bytes observed on the pipe (includes discarded after hard_max). */
    readonly stdoutProduced?: number;
    readonly stderrProduced?: number;
    readonly stdoutTruncated?: boolean;
    readonly stderrTruncated?: boolean;
    readonly nextStdoutOffset: number;
    readonly nextStderrOffset: number;
  }

  export interface WriteOptions {
    readonly close?: boolean;
    /** Stdin write deadline in ms (0 = no timeout). Default 30000. */
    readonly timeoutMs?: number;
    /** Max payload size in bytes (default 1 MiB). */
    readonly maxBytes?: number;
  }

  export interface TerminateOptions {
    readonly signal?: 'SIGTERM' | 'SIGINT' | 'SIGKILL' | string;
    readonly graceMs?: number;
  }

  /**
   * Run argv without a shell (Boost.Process v2 + Asio).
   * Returns a Promise; does not block the event loop.
   */
  export function run(argv: readonly string[], options?: RunOptions): Promise<RunResult>;

  /** Start a long-lived process (process group leader). */
  export function start(argv: readonly string[], options?: StartOptions): Promise<StartResult>;

  /** Read accumulated stdout/stderr slices (optional wait). */
  export function read(id: string, options?: ReadOptions): Promise<ReadResult>;

  /** Write to stdin. */
  export function write(
    id: string,
    data: string,
    options?: WriteOptions,
  ): Promise<{ writtenBytes: number }>;

  /** Signal process group; grace kill with SIGKILL after graceMs. */
  export function terminate(
    id: string,
    options?: TerminateOptions,
  ): Promise<{ requested: boolean; status?: string; exitCode?: number }>;

  /**
   * Free registry buffers for a process. If still running, kills it first.
   * Idempotent: unknown id → closed: false.
   */
  export function close(id: string): Promise<{ closed: boolean }>;
}
