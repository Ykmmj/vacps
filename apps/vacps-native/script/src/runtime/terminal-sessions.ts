import * as host from 'vacps:host';
import * as log from 'vacps:log';
import { Terminal, type TerminalSignal, type TerminalSnapshot } from 'vacps:terminal';
import { sleep } from 'vacps:timer';

import { requireAbsolutePath } from '../util/absolute-path';
import { resolveExecutable } from '../util/resolve-executable';
import { utf8Decode, utf8Encode, utf8PrefixEnd } from '../util/utf8';
import { randomUuidV4 } from '../util/uuid';
import type { ShellPath } from './process-exec';

const SESSION_CAPACITY = 64;
const FINISHED_RETENTION_MS = 10 * 60 * 1000;
const DEFAULT_IDLE_TIMEOUT_MS = 30 * 60 * 1000;

export type OpenTerminalInput =
  | {
      kind: 'command';
      program: string;
      arguments?: readonly string[];
      workingDirectory?: string;
      columns: number;
      rows: number;
      timeoutMs: number;
      idleTimeoutMs: number;
      maxBufferBytes: number;
    }
  | {
      kind: 'shell';
      shell: ShellPath;
      login: boolean;
      workingDirectory?: string;
      columns: number;
      rows: number;
      timeoutMs: number;
      idleTimeoutMs: number;
      maxBufferBytes: number;
    };

export interface TerminalView {
  terminal_id: string;
  backend_id: string;
  status: TerminalSnapshot['status'];
  exit_code: number | null;
  signal: string | null;
  timed_out: boolean;
  columns: number;
  rows: number;
  next_cursor: string;
  available_from: string;
  buffered_bytes: number;
  created_at: string;
  finished_at: string | null;
  idle_timeout_ms: number;
}

class TerminalSession {
  readonly completion: Promise<void>;
  lastActivityMs: number;
  finishedAtMs: number | null = null;

  constructor(
    readonly id: string,
    readonly terminal: Terminal,
    readonly createdAtMs: number,
    readonly idleTimeoutMs: number,
  ) {
    this.lastActivityMs = createdAtMs;
    this.completion = terminal.waitForExit().then(() => {
      this.finishedAtMs = host.nowMs();
    });
    void this.completion.catch((error: unknown) => {
      log.warn(
        `terminal ${id} completion failed: ${error instanceof Error ? error.message : String(error)}`,
      );
    });
  }

  touch(): void {
    this.lastActivityMs = host.nowMs();
  }
}

/** Product-owned identities and retention over JS-owned native Terminal handles. */
export class TerminalSessions {
  private readonly sessions = new Map<string, TerminalSession>();
  private closed = false;

  constructor(private readonly backendId: string) {}

  async open(input: OpenTerminalInput): Promise<TerminalView> {
    await this.pruneForAdmission();
    if (this.sessions.size >= SESSION_CAPACITY) {
      throw protocolError(
        'terminal_capacity',
        `Terminal capacity is ${SESSION_CAPACITY}; close an existing live terminal.`,
        503,
      );
    }

    const cwd = input.workingDirectory ? requireAbsolutePath(input.workingDirectory) : '/tmp';
    let argv: [string, ...string[]];
    if (input.kind === 'command') {
      argv = [await resolveExecutable(input.program), ...(input.arguments ?? [])];
    } else {
      const shell = await resolveExecutable(input.shell);
      argv = [shell, ...interactiveShellFlags(input.shell, input.login)];
    }

    const [command, ...args] = argv;
    const terminal = new Terminal(command, args, {
      cwd,
      columns: input.columns,
      rows: input.rows,
      timeoutMs: input.timeoutMs,
      maxBufferBytes: input.maxBufferBytes,
    });
    try {
      await terminal.start();
    } catch (error) {
      await terminal.close(0);
      throw error;
    }

    const now = host.nowMs();
    const session = new TerminalSession(
      `term_${randomUuidV4().replaceAll('-', '')}`,
      terminal,
      now,
      input.idleTimeoutMs || DEFAULT_IDLE_TIMEOUT_MS,
    );
    this.sessions.set(session.id, session);
    void this.reapWhenIdle(session).catch((error: unknown) => {
      if (!this.closed) {
        log.warn(
          `terminal ${session.id} idle reaper failed: ${
            error instanceof Error ? error.message : String(error)
          }`,
        );
      }
    });
    return this.view(session);
  }

  get(terminalId: string): TerminalView {
    const session = this.require(terminalId);
    session.touch();
    return this.view(session);
  }

  list(): TerminalView[] {
    return [...this.sessions.values()].map((session) => this.view(session));
  }

  async read(
    terminalId: string,
    cursor: string | undefined,
    maxBytes: number,
    waitMs: number,
  ): Promise<Record<string, unknown>> {
    const session = this.require(terminalId);
    session.touch();
    const result = await session.terminal.read({
      offset: parseCursor(cursor),
      maxBytes,
      waitMs,
    });
    const data = new Uint8Array(result.data);
    const completeBytes = result.eof ? data.byteLength : utf8PrefixEnd(data, data.byteLength);
    const trailingBytes = data.byteLength - completeBytes;
    session.touch();
    return {
      terminal_id: session.id,
      status: result.status,
      exit_code: result.exitCode,
      signal: result.signal,
      timed_out: result.timedOut,
      content: utf8Decode(data.subarray(0, completeBytes)),
      next_cursor: String(result.nextOffset - trailingBytes),
      available_from: String(result.availableFrom),
      dropped: result.dropped,
      eof: result.eof && trailingBytes === 0,
    };
  }

  async write(terminalId: string, data: string): Promise<number> {
    const session = this.require(terminalId);
    session.touch();
    const bytes = utf8Encode(data);
    if (bytes.byteLength > 1_048_576) {
      throw protocolError(
        'validation_error',
        'terminal write data must be at most 1 MiB of UTF-8 text.',
        400,
      );
    }
    const written = await session.terminal.write(bytes);
    session.touch();
    return written;
  }

  async resize(terminalId: string, columns: number, rows: number): Promise<TerminalView> {
    const session = this.require(terminalId);
    await session.terminal.resize(columns, rows);
    session.touch();
    return this.view(session);
  }

  async signal(terminalId: string, signal: TerminalSignal): Promise<TerminalView> {
    const session = this.require(terminalId);
    await session.terminal.signal(signal);
    session.touch();
    return this.view(session);
  }

  async closeTerminal(terminalId: string, gracePeriodMs: number): Promise<TerminalView> {
    const session = this.require(terminalId);
    const snapshot = session.terminal.snapshot();
    await session.terminal.close(gracePeriodMs);
    this.sessions.delete(session.id);
    return {
      ...this.viewSnapshot(session, snapshot),
      status: 'closed',
      finished_at: new Date(host.nowMs()).toISOString(),
    };
  }

  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    const sessions = [...this.sessions.values()];
    this.sessions.clear();
    await Promise.all(sessions.map(async (session) => await session.terminal.close(0)));
  }

  private view(session: TerminalSession): TerminalView {
    return this.viewSnapshot(session, session.terminal.snapshot());
  }

  private viewSnapshot(session: TerminalSession, snapshot: TerminalSnapshot): TerminalView {
    return {
      terminal_id: session.id,
      backend_id: this.backendId,
      status: snapshot.status,
      exit_code: snapshot.exitCode,
      signal: snapshot.signal,
      timed_out: snapshot.timedOut,
      columns: snapshot.columns,
      rows: snapshot.rows,
      next_cursor: String(snapshot.nextOffset),
      available_from: String(snapshot.availableFrom),
      buffered_bytes: snapshot.bufferedBytes,
      created_at: new Date(session.createdAtMs).toISOString(),
      finished_at:
        session.finishedAtMs === null ? null : new Date(session.finishedAtMs).toISOString(),
      idle_timeout_ms: session.idleTimeoutMs,
    };
  }

  private require(terminalId: string): TerminalSession {
    const session = this.sessions.get(terminalId);
    if (session === undefined) {
      throw protocolError('terminal_not_found', `Terminal '${terminalId}' was not found.`, 404);
    }
    return session;
  }

  private async pruneForAdmission(): Promise<void> {
    const now = host.nowMs();
    for (const session of [...this.sessions.values()]) {
      if (session.finishedAtMs !== null && now - session.finishedAtMs >= FINISHED_RETENTION_MS) {
        await this.remove(session, 0);
      }
    }
  }

  private async reapWhenIdle(session: TerminalSession): Promise<void> {
    for (;;) {
      await sleep(30_000);
      if (this.closed || this.sessions.get(session.id) !== session) return;
      const now = host.nowMs();
      if (session.finishedAtMs !== null && now - session.finishedAtMs >= FINISHED_RETENTION_MS) {
        await this.remove(session, 0);
        return;
      }
      if (session.finishedAtMs === null && now - session.lastActivityMs >= session.idleTimeoutMs) {
        await this.remove(session, 1000);
        return;
      }
    }
  }

  private async remove(session: TerminalSession, gracePeriodMs: number): Promise<void> {
    if (this.sessions.get(session.id) !== session) return;
    this.sessions.delete(session.id);
    await session.terminal.close(gracePeriodMs);
  }
}

function interactiveShellFlags(shell: ShellPath, login: boolean): string[] {
  if (shell === '/bin/bash') {
    return login ? ['-l'] : ['--noprofile', '--norc'];
  }
  return login ? ['-l'] : [];
}

function parseCursor(cursor: string | undefined): number {
  if (cursor === undefined) return 0;
  if (!/^(0|[1-9][0-9]*)$/.test(cursor)) {
    throw protocolError('validation_error', 'cursor must be a decimal byte offset.', 400);
  }
  const offset = Number(cursor);
  if (!Number.isSafeInteger(offset)) {
    throw protocolError('validation_error', 'cursor exceeds the safe integer range.', 400);
  }
  return offset;
}

function protocolError(
  code: string,
  message: string,
  statusCode: number,
): Error & { code: string; statusCode: number } {
  return Object.assign(new Error(message), { code, statusCode });
}
