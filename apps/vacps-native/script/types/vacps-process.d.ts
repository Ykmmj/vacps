/**
 * vacps:process — subprocess capability (create-at-JS-call).
 *
 * Surface: class Process + run(command, args?, options?).
 * JS Process object is the resource handle (no registry id API).
 */
declare module 'vacps:process' {
  export type StdioMode = 'pipe' | 'inherit' | 'ignore';

  export interface ProcessOptions {
    readonly cwd?: string;
    readonly env?: Readonly<Record<string, string>>;
    readonly stdin?: StdioMode;
    readonly stdout?: StdioMode;
    readonly stderr?: StdioMode;
    /** Kill after this many ms (0 / omit = no timeout). */
    readonly timeoutMs?: number;
    /** Cap retained stdout (default 16 MiB). */
    readonly maxStdoutBytes?: number;
    /** Cap retained stderr (default 16 MiB). */
    readonly maxStderrBytes?: number;
  }

  export interface ProcessResult {
    readonly exitCode: number;
    readonly timedOut: boolean;
    readonly stdout: string;
    readonly stderr: string;
    readonly stdoutProduced?: number;
    readonly stderrProduced?: number;
    readonly stdoutTruncated?: boolean;
    readonly stderrTruncated?: boolean;
  }

  /**
   * Long-lived process handle. Construct with command/args; not spawned until start().
   * JS object is the resource handle (no registry id).
   */
  export class Process {
    constructor(command: string, args?: readonly string[], options?: ProcessOptions);

    /** OS pid after start(); null before spawn / after reaped. */
    readonly pid: number | null;
    readonly running: boolean;

    start(): Promise<void>;
    read(stream?: 'stdout' | 'stderr'): Promise<Uint8Array>;
    write(data: ArrayBufferView | ArrayBuffer | string): Promise<number>;
    wait(): Promise<ProcessResult>;
    terminate(signal?: string): Promise<void>;
    close(): Promise<void>;
  }

  /**
   * Convenience: construct → start → drain → wait → close.
   * Signature: run(command, args?, options?).
   */
  export function run(
    command: string,
    args?: readonly string[],
    options?: ProcessOptions,
  ): Promise<ProcessResult>;
}
