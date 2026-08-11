/** Linux PTY-backed interactive terminal. Raw output is bytes; stdout/stderr merge. */
declare module 'vacps:terminal' {
  export type TerminalStatus = 'running' | 'exited' | 'signaled' | 'timed_out' | 'closed';
  export type TerminalSignal = 'SIGINT' | 'SIGTERM' | 'SIGHUP' | 'SIGKILL' | 'SIGTSTP' | 'SIGCONT';

  export interface TerminalOptions {
    readonly cwd?: string;
    readonly columns?: number;
    readonly rows?: number;
    /** Kill the terminal process group after this duration; 0/omit disables it. */
    readonly timeoutMs?: number;
    /** Rolling raw-output retention, 64 KiB..16 MiB; default 4 MiB. */
    readonly maxBufferBytes?: number;
  }

  export interface TerminalExit {
    readonly status: TerminalStatus;
    readonly exitCode: number | null;
    readonly signal: string | null;
    readonly timedOut: boolean;
  }

  export interface TerminalSnapshot extends TerminalExit {
    readonly columns: number;
    readonly rows: number;
    /** Absolute byte offset immediately after all output observed so far. */
    readonly nextOffset: number;
    /** Oldest retained absolute byte offset. */
    readonly availableFrom: number;
    readonly bufferedBytes: number;
  }

  export interface TerminalReadOptions {
    readonly offset?: number;
    readonly maxBytes?: number;
    readonly waitMs?: number;
  }

  export interface TerminalReadResult extends TerminalExit {
    readonly data: ArrayBuffer;
    readonly nextOffset: number;
    readonly availableFrom: number;
    /** True when bytes between the requested offset and returned data were evicted. */
    readonly dropped: boolean;
    readonly eof: boolean;
  }

  export interface TerminalExitWait extends TerminalExit {
    readonly completed: boolean;
  }

  export class Terminal {
    constructor(command: string, args?: readonly string[], options?: TerminalOptions);
    start(): Promise<void>;
    write(data: string | ArrayBuffer | ArrayBufferView): Promise<number>;
    read(options?: TerminalReadOptions): Promise<TerminalReadResult>;
    resize(columns: number, rows: number): Promise<void>;
    signal(signal: TerminalSignal): Promise<void>;
    snapshot(): TerminalSnapshot;
    waitForExit(timeoutMs?: number): Promise<TerminalExitWait>;
    close(gracePeriodMs?: number): Promise<void>;
  }
}
