import * as host from 'vacps:host';
import * as log from 'vacps:log';
import {
  Terminal,
  type TerminalCloseResult,
  type TerminalSignal,
  type TerminalSnapshot,
  type TerminalStatus,
} from 'vacps:terminal';
import { sleep } from 'vacps:timer';

import { requireAbsolutePath } from '../util/absolute-path';
import { resolveExecutable } from '../util/resolve-executable';
import {
  utf8ByteLengthOfString,
  utf8ByteSlice,
  utf8Decode,
  utf8Encode,
  utf8PrefixEnd,
} from '../util/utf8';
import { randomUuidV4 } from '../util/uuid';
import type { ShellPath } from './process-exec';
import {
  encodeTerminalKeys,
  type TerminalKeyEvent,
  type TerminalInputModes,
} from './terminal-key-encoder';
import { TerminalScreen, type TerminalScreenView } from './terminal-screen';

const SESSION_CAPACITY = 64;
const FINISHED_RETENTION_MS = 10 * 60 * 1000;
const DEFAULT_IDLE_TIMEOUT_MS = 30 * 60 * 1000;
const EXPECT_WINDOW_MAX_BYTES = 16 * 1024 * 1024;
const SCREEN_READ_BYTES = 64 * 1024;

export type TerminalProcessState = 'running' | 'exited' | 'signaled' | 'timed_out';
export type TerminalSessionState = 'open' | 'closing' | 'closed';

type TerminalEnvironment = Readonly<Record<string, string>>;

export type OpenTerminalInput =
  | {
      kind: 'command';
      program: string;
      arguments?: readonly string[];
      workingDirectory?: string;
      environment?: TerminalEnvironment;
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
      environment?: TerminalEnvironment;
      columns: number;
      rows: number;
      timeoutMs: number;
      idleTimeoutMs: number;
      maxBufferBytes: number;
    };

export interface TerminalView {
  terminal_id: string;
  backend_id: string;
  session_state: TerminalSessionState;
  process_state: TerminalProcessState;
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

export interface TerminalExpectInput {
  cursor?: string;
  pattern: string;
  mode: 'literal' | 'regex';
  regexFlags: string;
  timeoutMs: number;
}

export interface TerminalListInput {
  status?: TerminalProcessState;
  createdAfterMs?: number;
}

interface Utf8ReadResult {
  status: TerminalStatus;
  exitCode: number | null;
  signal: string | null;
  timedOut: boolean;
  content: string;
  contentStart: number;
  nextCursor: number;
  availableFrom: number;
  dropped: boolean;
  droppedBytes: number;
  eof: boolean;
}

interface ProgressSignal {
  promise: Promise<void>;
  resolve: () => void;
}

class TerminalSession {
  readonly completion: Promise<void>;
  lastActivityMs: number;
  finishedAtMs: number | null = null;
  private closing = false;
  private closePromise: Promise<TerminalCloseResult> | undefined;
  private closingSnapshot: TerminalSnapshot | undefined;

  private readonly screen: TerminalScreen;
  private readonly screenPump: Promise<void>;
  private screenCursor = 0;
  private screenDone = false;
  private screenError: unknown;
  private screenProgress = progressSignal();

  constructor(
    readonly id: string,
    readonly terminal: Terminal,
    readonly createdAtMs: number,
    readonly idleTimeoutMs: number,
    columns: number,
    rows: number,
  ) {
    this.lastActivityMs = createdAtMs;
    this.screen = new TerminalScreen(columns, rows);
    this.screenPump = this.pumpScreen().catch((error: unknown) => {
      this.screenError = error;
      this.notifyScreenProgress();
      log.warn(
        `terminal ${id} screen pump failed: ${error instanceof Error ? error.message : String(error)}`,
      );
    });
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

  sessionState(): Exclude<TerminalSessionState, 'closed'> {
    return this.closing ? 'closing' : 'open';
  }

  snapshot(): TerminalSnapshot {
    return this.closingSnapshot ?? this.terminal.snapshot();
  }

  requireOpen(): void {
    if (this.closing) {
      throw protocolError(
        'terminal_closing',
        `Terminal '${this.id}' is closing and accepts no new operations.`,
        409,
      );
    }
  }

  isClosing(): boolean {
    return this.closing;
  }

  resizeScreen(columns: number, rows: number): void {
    this.screen.resize(columns, rows);
  }

  async screenView(): Promise<TerminalScreenView> {
    const target = this.terminal.snapshot().nextOffset;
    while (this.screenCursor < target && !this.screenDone && this.screenError === undefined) {
      const progress = this.screenProgress.promise;
      if (this.screenCursor >= target || this.screenDone) break;
      await progress;
    }
    if (this.screenError !== undefined) throw this.screenError;
    return this.screen.snapshot();
  }

  async inputModes(): Promise<Omit<TerminalInputModes, 'eraseCharacter'>> {
    await this.screenView();
    return this.screen.inputModes();
  }

  async close(gracePeriodMs: number): Promise<TerminalCloseResult> {
    if (this.closePromise !== undefined) return await this.closePromise;
    this.closingSnapshot = this.terminal.snapshot();
    this.closing = true;
    this.closePromise = (async () => {
      const result = await this.terminal.close(gracePeriodMs);
      await this.screenPump;
      return result;
    })();
    return await this.closePromise;
  }

  private async pumpScreen(): Promise<void> {
    let decoder = new TextDecoder('utf-8');
    for (;;) {
      if (this.closing) {
        this.screenDone = true;
        this.notifyScreenProgress();
        return;
      }
      const result = await this.terminal.read({
        offset: this.screenCursor,
        maxBytes: SCREEN_READ_BYTES,
        waitMs: 60_000,
      });
      if (result.dropped) {
        this.screen.lostOutput();
        decoder = new TextDecoder('utf-8');
      }
      const bytes = new Uint8Array(result.data);
      const text = decoder.decode(bytes, { stream: !result.eof });
      if (text.length > 0) {
        this.screen.feed(text);
        const response = this.screen.takeResponses();
        if (response.length > 0) await this.terminal.write(utf8Encode(response));
      }
      this.screenCursor = result.nextOffset;
      this.notifyScreenProgress();
      if (result.eof) {
        this.screenDone = true;
        this.notifyScreenProgress();
        return;
      }
    }
  }

  private notifyScreenProgress(): void {
    const progress = this.screenProgress;
    this.screenProgress = progressSignal();
    progress.resolve();
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
    const environment: TerminalEnvironment = {
      TERM: 'xterm-256color',
      ...(input.environment ?? {}),
    };
    const terminal = new Terminal(command, args, {
      cwd,
      environment,
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
      input.columns,
      input.rows,
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

  list(input: TerminalListInput = {}): TerminalView[] {
    return [...this.sessions.values()]
      .filter(
        (session) =>
          (input.createdAfterMs === undefined || session.createdAtMs > input.createdAfterMs) &&
          (input.status === undefined || processState(session.snapshot().status) === input.status),
      )
      .map((session) => this.view(session));
  }

  async read(
    terminalId: string,
    cursor: string | undefined,
    maxBytes: number,
    waitMs: number,
  ): Promise<Record<string, unknown>> {
    const session = this.requireOpen(terminalId);
    session.touch();
    const offset = parseCursor(cursor);
    requireAvailableCursor(offset, session.snapshot().nextOffset);
    const result = await readUtf8(
      session.terminal,
      offset,
      maxBytes,
      waitMs,
      () => !session.isClosing(),
    );
    session.touch();
    return {
      terminal_id: session.id,
      session_state: session.sessionState(),
      process_state: processState(result.status),
      exit_code: result.exitCode,
      signal: result.signal,
      timed_out: result.timedOut,
      content: result.content,
      next_cursor: String(result.nextCursor),
      available_from: String(result.availableFrom),
      dropped: result.dropped,
      dropped_bytes: result.droppedBytes,
      eof: result.eof,
    };
  }

  async expect(terminalId: string, input: TerminalExpectInput): Promise<Record<string, unknown>> {
    const session = this.requireOpen(terminalId);
    session.touch();
    const expression = compileExpectation(input);
    const deadline = host.nowMs() + input.timeoutMs;
    let cursor = parseCursor(input.cursor);
    requireAvailableCursor(cursor, session.snapshot().nextOffset);
    let scanBase = cursor;
    let text = '';
    let dropped = false;
    let droppedBytes = 0;
    let availableFrom = cursor;
    let processExitBoundary: number | undefined;
    let timeoutBoundary: number | undefined;

    for (;;) {
      const beforeRead = session.snapshot();
      if (beforeRead.status !== 'running' && processExitBoundary === undefined) {
        processExitBoundary = beforeRead.nextOffset;
      }
      const remaining = Math.max(0, deadline - host.nowMs());
      const result = await readUtf8(
        session.terminal,
        cursor,
        1_048_576,
        processExitBoundary === undefined ? Math.min(remaining, 60_000) : 0,
        () => !session.isClosing(),
      );
      availableFrom = result.availableFrom;
      if (result.status !== 'running' && processExitBoundary === undefined) {
        // Drain everything already observed when the process exited, but do
        // not wait indefinitely for descendants that retained the PTY slave.
        processExitBoundary = session.snapshot().nextOffset;
      }
      if (host.nowMs() >= deadline && timeoutBoundary === undefined) {
        // A zero/expired timeout still scans all text that already exists.
        timeoutBoundary = session.snapshot().nextOffset;
      }
      if (result.dropped) {
        dropped = true;
        droppedBytes += result.droppedBytes;
        text = '';
        scanBase = result.contentStart;
      } else if (text.length === 0) {
        scanBase = result.contentStart;
      }
      text += result.content;

      const match = findExpectation(expression, text);
      if (match !== null) {
        const startCursor = scanBase + utf8ByteLengthOfString(text.slice(0, match.index));
        const endCursor = startCursor + utf8ByteLengthOfString(match.text);
        session.touch();
        return {
          terminal_id: session.id,
          session_state: session.sessionState(),
          process_state: processState(result.status),
          exit_code: result.exitCode,
          signal: result.signal,
          timed_out: result.timedOut,
          matched: true,
          match: match.text,
          start_cursor: String(startCursor),
          end_cursor: String(endCursor),
          next_cursor: String(endCursor),
          available_from: String(availableFrom),
          dropped,
          dropped_bytes: droppedBytes,
          eof: result.eof,
          timeout: false,
        };
      }
      ({ text, scanBase } = trimExpectationWindow(text, scanBase));

      const previousCursor = cursor;
      cursor = result.nextCursor;
      session.requireOpen();
      // An incomplete trailing UTF-8 sequence deliberately does not advance
      // the public cursor. Once exit/timeout fixes the scan boundary, lack of
      // progress is nevertheless terminal for this expect call.
      const cannotDrainFurther = cursor === previousCursor;
      const processExited =
        processExitBoundary !== undefined && (cursor >= processExitBoundary || cannotDrainFurther);
      const timedOut =
        timeoutBoundary !== undefined && (cursor >= timeoutBoundary || cannotDrainFurther);
      if (result.eof || processExited || timedOut) {
        session.touch();
        return {
          terminal_id: session.id,
          session_state: session.sessionState(),
          process_state: processState(result.status),
          exit_code: result.exitCode,
          signal: result.signal,
          timed_out: result.timedOut,
          matched: false,
          match: null,
          start_cursor: null,
          end_cursor: null,
          next_cursor: String(cursor),
          available_from: String(availableFrom),
          dropped,
          dropped_bytes: droppedBytes,
          eof: result.eof,
          timeout: timedOut && !result.eof && !processExited,
        };
      }
    }
  }

  async write(terminalId: string, data: string): Promise<number> {
    const session = this.requireOpen(terminalId);
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

  async sendKeys(terminalId: string, keys: readonly TerminalKeyEvent[]): Promise<number> {
    const session = this.requireOpen(terminalId);
    session.touch();
    const inputModes = await session.inputModes();
    session.requireOpen();
    const snapshot = session.terminal.snapshot();
    let data: Uint8Array;
    try {
      data = encodeTerminalKeys(keys, {
        ...inputModes,
        eraseCharacter: snapshot.eraseCharacter,
      });
    } catch (error) {
      throw protocolError(
        'validation_error',
        error instanceof Error ? error.message : String(error),
        400,
      );
    }
    if (data.byteLength > 1_048_576) {
      throw protocolError(
        'validation_error',
        'encoded terminal key input must be at most 1 MiB.',
        400,
      );
    }
    const written = await session.terminal.write(data);
    session.touch();
    return written;
  }

  async screen(terminalId: string): Promise<Record<string, unknown>> {
    const session = this.requireOpen(terminalId);
    session.touch();
    const [screen, view] = await Promise.all([
      session.screenView(),
      Promise.resolve(this.view(session)),
    ]);
    session.touch();
    return {
      terminal_id: session.id,
      session_state: view.session_state,
      process_state: view.process_state,
      exit_code: view.exit_code,
      signal: view.signal,
      timed_out: view.timed_out,
      rows: screen.rows,
      columns: screen.columns,
      cursor: screen.cursor,
      lines: screen.lines,
      generation: screen.generation,
      dropped: screen.dropped,
    };
  }

  async resize(terminalId: string, columns: number, rows: number): Promise<TerminalView> {
    const session = this.requireOpen(terminalId);
    const previous = session.snapshot();
    session.resizeScreen(columns, rows);
    try {
      await session.terminal.resize(columns, rows);
    } catch (error) {
      session.resizeScreen(previous.columns, previous.rows);
      throw error;
    }
    session.touch();
    return this.view(session);
  }

  async signal(terminalId: string, signal: TerminalSignal): Promise<TerminalView> {
    const session = this.requireOpen(terminalId);
    await session.terminal.signal(signal);
    session.touch();
    return this.view(session);
  }

  async closeTerminal(terminalId: string, gracePeriodMs: number): Promise<Record<string, unknown>> {
    const session = this.require(terminalId);
    const snapshot = session.snapshot();
    const result = await session.close(gracePeriodMs);
    this.sessions.delete(session.id);
    return {
      ...this.viewSnapshot(session, snapshot, 'closed'),
      process_state: processState(result.status),
      exit_code: result.exitCode,
      signal: result.signal,
      timed_out: result.timedOut,
      finished_at: new Date(host.nowMs()).toISOString(),
      escalated: result.escalated,
      final_signal: result.finalSignal,
    };
  }

  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    const sessions = [...this.sessions.values()];
    this.sessions.clear();
    await Promise.all(sessions.map(async (session) => await session.close(0)));
  }

  private view(session: TerminalSession): TerminalView {
    return this.viewSnapshot(session, session.snapshot(), session.sessionState());
  }

  private viewSnapshot(
    session: TerminalSession,
    snapshot: TerminalSnapshot,
    sessionState: TerminalSessionState,
  ): TerminalView {
    return {
      terminal_id: session.id,
      backend_id: this.backendId,
      session_state: sessionState,
      process_state: processState(snapshot.status),
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

  private requireOpen(terminalId: string): TerminalSession {
    const session = this.require(terminalId);
    session.requireOpen();
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
    await session.close(gracePeriodMs);
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

function requireAvailableCursor(cursor: number, nextOffset: number): void {
  if (cursor > nextOffset) {
    throw protocolError(
      'validation_error',
      `cursor ${cursor} is ahead of terminal output offset ${nextOffset}.`,
      400,
    );
  }
}

async function readUtf8(
  terminal: Terminal,
  offset: number,
  maxBytes: number,
  waitMs: number,
  mayReadAgain: () => boolean,
): Promise<Utf8ReadResult> {
  const deadline = host.nowMs() + waitMs;
  let rawOffset = offset;
  let bytes: Uint8Array = new Uint8Array(0);
  let contentStart = offset;
  let dropped = false;
  let droppedBytes = 0;

  for (;;) {
    const remainingWait = Math.max(0, deadline - host.nowMs());
    const requestedBytes = bytes.byteLength === 0 ? maxBytes : 4;
    const result = await terminal.read({
      offset: rawOffset,
      maxBytes: requestedBytes,
      waitMs: remainingWait,
    });
    const incoming = new Uint8Array(result.data);
    const incomingStart = result.nextOffset - incoming.byteLength;
    if (result.dropped) {
      bytes = new Uint8Array(0);
      dropped = true;
      droppedBytes += result.droppedBytes;
      contentStart = incomingStart;
    } else if (bytes.byteLength === 0) {
      contentStart = incomingStart;
    }
    bytes = concatenate(bytes, incoming);
    rawOffset = result.nextOffset;

    const leading = dropped ? leadingContinuationBytes(bytes) : 0;
    const completeEnd = result.eof
      ? bytes.byteLength
      : utf8PrefixEnd(bytes, Math.min(bytes.byteLength, maxBytes));
    if (
      completeEnd > leading ||
      result.eof ||
      remainingWait === 0 ||
      bytes.byteLength === 0 ||
      !mayReadAgain()
    ) {
      if (dropped && leading > 0) droppedBytes += leading;
      const nextCursor = contentStart + completeEnd;
      return {
        status: result.status,
        exitCode: result.exitCode,
        signal: result.signal,
        timedOut: result.timedOut,
        content: utf8Decode(bytes.subarray(leading, completeEnd)),
        contentStart: contentStart + leading,
        nextCursor,
        availableFrom: result.availableFrom + (dropped ? leading : 0),
        dropped,
        droppedBytes,
        eof: result.eof && nextCursor >= result.nextOffset,
      };
    }
  }
}

function concatenate(left: Uint8Array, right: Uint8Array): Uint8Array {
  if (left.byteLength === 0) return right;
  if (right.byteLength === 0) return left;
  const result = new Uint8Array(left.byteLength + right.byteLength);
  result.set(left);
  result.set(right, left.byteLength);
  return result;
}

function leadingContinuationBytes(bytes: Uint8Array): number {
  let index = 0;
  while (index < bytes.byteLength && (bytes[index]! & 0xc0) === 0x80) index++;
  return index;
}

type Expectation = { mode: 'literal'; pattern: string } | { mode: 'regex'; regex: RegExp };

function compileExpectation(input: TerminalExpectInput): Expectation {
  if (input.mode === 'literal') return { mode: 'literal', pattern: input.pattern };
  try {
    return { mode: 'regex', regex: new RegExp(input.pattern, input.regexFlags) };
  } catch (error) {
    throw protocolError(
      'validation_error',
      `invalid terminal expect regex: ${error instanceof Error ? error.message : String(error)}`,
      400,
    );
  }
}

function findExpectation(
  expectation: Expectation,
  text: string,
): { index: number; text: string } | null {
  if (expectation.mode === 'literal') {
    const index = text.indexOf(expectation.pattern);
    return index < 0 ? null : { index, text: expectation.pattern };
  }
  const match = expectation.regex.exec(text);
  return match === null ? null : { index: match.index, text: match[0] ?? '' };
}

function trimExpectationWindow(text: string, scanBase: number): { text: string; scanBase: number } {
  const bytes = utf8ByteLengthOfString(text);
  if (bytes <= EXPECT_WINDOW_MAX_BYTES) return { text, scanBase };
  const slice = utf8ByteSlice(text, bytes - EXPECT_WINDOW_MAX_BYTES, bytes);
  return { text: slice.content, scanBase: scanBase + slice.start };
}

function processState(status: TerminalStatus): TerminalProcessState {
  return status;
}

function progressSignal(): ProgressSignal {
  let resolve!: () => void;
  const promise = new Promise<void>((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

function protocolError(
  code: string,
  message: string,
  statusCode: number,
): Error & { code: string; statusCode: number } {
  return Object.assign(new Error(message), { code, statusCode });
}
