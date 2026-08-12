/** Linux PTY-backed interactive terminal. Raw output is bytes; stdout/stderr merge. */
declare module 'vacps:terminal' {
  export type TerminalStatus = 'running' | 'exited' | 'signaled' | 'timed_out';
  export type TerminalSignal = 'SIGINT' | 'SIGTERM' | 'SIGHUP' | 'SIGKILL' | 'SIGTSTP' | 'SIGCONT';

  export interface TerminalOptions {
    readonly cwd?: string;
    /** Overrides inherited environment variables for the child. */
    readonly environment?: Readonly<Record<string, string>>;
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
    /** PTY slave VERASE byte used for semantic Backspace input. */
    readonly eraseCharacter: number;
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
    /** Exact number of bytes lost before returned data due to rolling retention. */
    readonly droppedBytes: number;
    /** True when bytes between the requested offset and returned data were evicted. */
    readonly dropped: boolean;
    readonly eof: boolean;
  }

  export interface TerminalExitWait extends TerminalExit {
    readonly completed: boolean;
  }

  export interface TerminalCloseResult extends TerminalExit {
    /**
     * True only when this close operation's grace deadline sent SIGKILL.
     * It remains false if the independent TerminalOptions timeout sent SIGKILL.
     */
    readonly escalated: boolean;
    /** Signal sent by this close operation's escalation, not the process exit signal. */
    readonly finalSignal: 'SIGKILL' | null;
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
    close(gracePeriodMs?: number): Promise<TerminalCloseResult>;
  }
}
