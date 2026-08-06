/**
 * vacps:process — subprocess capability (create-at-JS-call).
 *
 * Surface: class Process + run(command, args?, options?).
 * JS Process object is the resource handle (no registry id API).
 *
 * ProcessOptions is honest and narrow:
 * - cwd?, timeoutMs?, stdin?: 'pipe'|'ignore', maxStdoutBytes?, maxStderrBytes?
 * - env is not supported — providing it throws TypeError.
 * - stdout/stderr mode keys are not supported (always captured pipes).
 * Defaults: Process class stdin pipe/open; run() stdin ignore/closed.
 * timeoutMs 0/omit = none. Capture caps 0..64 MiB (0 retains nothing).
 */
declare module 'vacps:process' {
  export interface ProcessOptions {
    readonly cwd?: string;
    /**
     * 'pipe' keeps stdin open for write(); 'ignore' closes stdin after spawn.
     * Default: Process class → 'pipe'; run() → 'ignore'.
     */
    readonly stdin?: 'pipe' | 'ignore';
    /** Kill after this many ms (0 / omit = no timeout). */
    readonly timeoutMs?: number;
    /** Cap retained stdout bytes (default 16 MiB; 0 captures nothing). */
    readonly maxStdoutBytes?: number;
    /** Cap retained stderr bytes (default 16 MiB; 0 captures nothing). */
    readonly maxStderrBytes?: number;
  }

  export interface ProcessResult {
    readonly exitCode: number;
    readonly timedOut: boolean;
    readonly stdout: string;
    readonly stderr: string;
    /** Exact bytes drained from stdout/stderr, including discarded bytes. */
    readonly stdoutBytes: number;
    readonly stderrBytes: number;
    /** True when the native capture cap or global process budget discarded bytes. */
    readonly stdoutTruncated: boolean;
    readonly stderrTruncated: boolean;
  }

  export type ProcessStatus = 'running' | 'exited' | 'signaled' | 'timed_out' | 'cancelled';

  export interface ProcessExit {
    readonly status: ProcessStatus;
    readonly exitCode: number | null;
    readonly signal: string | null;
    readonly timedOut: boolean;
  }

  export interface ProcessExitWait extends ProcessExit {
    readonly completed: boolean;
  }

  export interface ProcessReadOptions {
    readonly sequence?: number;
    readonly byteOffset?: number;
    readonly maxBytes?: number;
    readonly waitMs?: number;
  }

  export interface ProcessOutputChunk {
    readonly sequence: number;
    readonly stream: 'stdout' | 'stderr';
    readonly data: string;
    readonly observedAtMs: number;
    readonly offsetStart: number;
    readonly offsetEnd: number;
  }

  export interface ProcessReadResult extends ProcessExit {
    readonly chunks: readonly ProcessOutputChunk[];
    readonly nextSequence: number;
    readonly nextByteOffset: number;
    readonly eof: boolean;
    readonly returnedBytes: number;
  }

  export interface ProcessSnapshotOptions {
    readonly stdoutMaxBytes?: number;
    readonly stderrMaxBytes?: number;
  }

  export interface ProcessSnapshot extends ProcessExit {
    readonly stdinAvailable: boolean;
    readonly stdout: string;
    readonly stderr: string;
    readonly stdoutBytes: number;
    readonly stderrBytes: number;
    readonly stdoutTruncated: boolean;
    readonly stderrTruncated: boolean;
  }

  /**
   * Long-lived process handle. Construct with command/args; not spawned until start().
   * JS object is the resource handle (no registry id).
   */
  export class Process {
    constructor(command: string, args?: readonly string[], options?: ProcessOptions);

    start(): Promise<void>;
    write(data: string | ArrayBuffer | ArrayBufferView, closeStdin?: boolean): Promise<number>;
    read(options?: ProcessReadOptions): Promise<ProcessReadResult>;
    waitForExit(timeoutMs?: number): Promise<ProcessExitWait>;
    snapshot(options?: ProcessSnapshotOptions): ProcessSnapshot;
    wait(): Promise<ProcessResult>;
    terminate(signal?: 'SIGTERM' | 'SIGINT' | 'SIGKILL', gracePeriodMs?: number): Promise<void>;
    close(): Promise<void>;
  }

  /**
   * Convenience: construct → start → wait → close.
   * Signature: run(command, args?, options?) only (no second-arg overload).
   */
  export function run(
    command: string,
    args?: readonly string[],
    options?: ProcessOptions,
  ): Promise<ProcessResult>;
}
