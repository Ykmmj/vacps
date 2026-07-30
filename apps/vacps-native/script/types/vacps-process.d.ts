declare module "vacps:process" {
  export interface RunOptions {
    readonly cwd?: string;
    readonly timeoutMs?: number;
  }

  export interface RunResult {
    readonly exitCode: number;
    readonly timedOut: boolean;
    readonly stdout: string;
    readonly stderr: string;
  }

  export interface StartOptions {
    readonly cwd?: string;
    readonly timeoutMs?: number;
    /** Default true. */
    readonly closeStdin?: boolean;
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
    readonly status: "running" | "exited" | "timed_out" | "cancelled" | string;
    readonly exitCode: number;
    readonly timedOut: boolean;
    readonly eof: boolean;
    readonly stdinOpen: boolean;
    readonly stdout: string;
    readonly stderr: string;
    readonly stdoutTotal: number;
    readonly stderrTotal: number;
    readonly nextStdoutOffset: number;
    readonly nextStderrOffset: number;
  }

  export interface WriteOptions {
    readonly close?: boolean;
  }

  export interface TerminateOptions {
    readonly signal?: "SIGTERM" | "SIGINT" | "SIGKILL" | string;
    readonly graceMs?: number;
  }

  /**
   * Run argv without a shell (Boost.Process v2 + Asio).
   * Returns a Promise; does not block the event loop.
   */
  export function run(
    argv: readonly string[],
    options?: RunOptions,
  ): Promise<RunResult>;

  /** Start a long-lived process (process group leader). */
  export function start(
    argv: readonly string[],
    options?: StartOptions,
  ): Promise<StartResult>;

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
}
