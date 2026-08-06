import * as host from 'vacps:host';
import * as log from 'vacps:log';
import {
  Process,
  type ProcessExit,
  type ProcessSnapshot as NativeProcessSnapshot,
  type ProcessStatus,
} from 'vacps:process';

import { requireAbsolutePath } from '../util/absolute-path';
import { resolveExecutable } from '../util/resolve-executable';
import { randomUuidV4 } from '../util/uuid';
import { NATIVE_STREAM_MAX_BYTES, shellArgvFlags, type ShellPath } from './process-exec';

const PROCESS_READ_MAX_BYTES = 1024 * 1024;
const SESSION_CAPACITY = 512;
const TERMINAL_RETENTION_MS = 10 * 60 * 1000;

export { NATIVE_STREAM_MAX_BYTES, PROCESS_READ_MAX_BYTES };

export type StartInput =
  | {
      kind: 'command';
      program: string;
      arguments?: readonly string[];
      workingDirectory?: string;
      timeoutMs: number;
      stdoutHardMaxBytes: number;
      stderrHardMaxBytes: number;
      stdin: 'pipe' | 'ignore';
    }
  | {
      kind: 'shell';
      command: string;
      shell: ShellPath;
      loadUserEnvironment: boolean;
      workingDirectory?: string;
      timeoutMs: number;
      stdoutHardMaxBytes: number;
      stderrHardMaxBytes: number;
      stdin: 'pipe' | 'ignore';
    };

export interface PreviewLimits {
  stdoutMaxBytes: number;
  stderrMaxBytes: number;
}

export interface ProcessSnapshot {
  process_id: string;
  backend_id: string;
  status: ProcessStatus;
  exit_code: number | null;
  signal: string | null;
  timed_out: boolean;
  started_at: string;
  finished_at: string | null;
  duration_ms: number;
  stdin_available: boolean;
  tty: false;
  output_cursor: null;
  stdout: OutputDescriptor;
  stderr: OutputDescriptor;
}

interface OutputDescriptor {
  preview: string;
  total_bytes: number;
  truncated: boolean;
}

class ProcessSession {
  readonly completion: Promise<ProcessExit>;
  finishedAtMs: number | null = null;
  replayRetainUntilMs = 0;

  constructor(
    readonly id: string,
    readonly process: Process,
    readonly startedAtMs: number,
  ) {
    this.completion = process.waitForExit().then((waited) => {
      this.finishedAtMs = host.nowMs();
      return waited;
    });
    void this.completion.catch((error: unknown) => {
      log.warn(
        `process session ${this.id} completion failed: ${error instanceof Error ? error.message : String(error)}`,
      );
    });
  }
}

/**
 * Product-owned protocol sessions over JS-owned native Process resources.
 * Host/Runtime never enumerate these handles.
 */
export class ProcessSessions {
  private readonly sessions = new Map<string, ProcessSession>();
  private closed = false;

  constructor(private readonly backendId: string) {}

  async exec(
    input: StartInput,
    preview: PreviewLimits,
    yieldMs: number | undefined,
  ): Promise<ProcessSnapshot> {
    const session = await this.create(input);
    if (yieldMs === undefined) {
      await session.completion;
    } else {
      const waited = await session.process.waitForExit(Math.min(yieldMs, input.timeoutMs));
      if (waited.completed) {
        session.finishedAtMs ??= host.nowMs();
      }
    }
    return this.snapshot(session, preview);
  }

  async start(input: StartInput, preview: PreviewLimits): Promise<ProcessSnapshot> {
    return this.snapshot(await this.create(input), preview);
  }

  snapshotById(processId: string, preview: PreviewLimits): ProcessSnapshot {
    return this.snapshot(this.require(processId), preview);
  }

  retainForIdempotencyReplay(processId: string): void {
    const session = this.require(processId);
    session.replayRetainUntilMs = Math.max(
      session.replayRetainUntilMs,
      host.nowMs() + TERMINAL_RETENTION_MS,
    );
  }

  async read(
    processId: string,
    cursor: string | undefined,
    maxBytes: number,
    waitMs: number,
  ): Promise<Record<string, unknown>> {
    const session = this.require(processId);
    const decoded = parseCursor(cursor);
    const result = await session.process.read({
      sequence: decoded.sequence,
      byteOffset: decoded.byteOffset,
      maxBytes,
      waitMs,
    });
    return {
      process_id: session.id,
      status: result.status,
      chunks: result.chunks.map((chunk) => ({
        sequence: chunk.sequence,
        stream: chunk.stream,
        data: chunk.data,
        observed_at: new Date(chunk.observedAtMs).toISOString(),
        offset_start: chunk.offsetStart,
        offset_end: chunk.offsetEnd,
      })),
      next_cursor: encodeCursor(result.nextSequence, result.nextByteOffset),
      eof: result.eof,
      exit_code: result.exitCode,
      signal: result.signal,
      returned_bytes: result.returnedBytes,
    };
  }

  async write(processId: string, data: string, closeStdin: boolean): Promise<number> {
    return await this.require(processId).process.write(data, closeStdin);
  }

  async terminate(
    processId: string,
    signal: 'sigterm' | 'sigint' | 'sigkill',
    gracePeriodMs: number,
    preview: PreviewLimits,
  ): Promise<Record<string, unknown>> {
    const session = this.require(processId);
    const before = session.process.snapshot({ stdoutMaxBytes: 0, stderrMaxBytes: 0 });
    const requestedSignal =
      signal === 'sigkill' ? 'SIGKILL' : signal === 'sigint' ? 'SIGINT' : 'SIGTERM';
    const terminationRequested = before.status === 'running';
    await session.process.terminate(requestedSignal, gracePeriodMs);
    return {
      ...this.snapshot(session, preview),
      termination_requested: terminationRequested,
      requested_signal: requestedSignal,
    };
  }

  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    const sessions = [...this.sessions.values()];
    this.sessions.clear();
    let firstError: unknown;
    for (const session of sessions) {
      try {
        await session.process.close();
      } catch (error) {
        if (firstError === undefined) firstError = error;
      }
    }
    if (firstError !== undefined) throw firstError;
  }

  private async create(input: StartInput): Promise<ProcessSession> {
    await this.pruneForAdmission();
    if (this.closed) {
      throw protocolError('process_sessions_closed', 'Process sessions are closed.', 503);
    }

    const cwd = input.workingDirectory ? requireAbsolutePath(input.workingDirectory) : '/tmp';
    let argv: [string, ...string[]];
    if (input.kind === 'command') {
      argv = [await resolveExecutable(input.program), ...(input.arguments ?? [])];
    } else {
      const shell = await resolveExecutable(input.shell);
      argv = [shell, ...shellArgvFlags(input.shell, input.loadUserEnvironment), input.command];
    }

    const [command, ...args] = argv;
    const process = new Process(command, args, {
      cwd,
      stdin: input.stdin,
      timeoutMs: input.timeoutMs,
      maxStdoutBytes: input.stdoutHardMaxBytes,
      maxStderrBytes: input.stderrHardMaxBytes,
    });
    const startedAtMs = host.nowMs();
    try {
      await process.start();
    } catch (error) {
      await process.close();
      throw error;
    }

    const session = new ProcessSession(
      `proc_${randomUuidV4().replaceAll('-', '')}`,
      process,
      startedAtMs,
    );
    this.sessions.set(session.id, session);
    return session;
  }

  private snapshot(session: ProcessSession, preview: PreviewLimits): ProcessSnapshot {
    const native = session.process.snapshot({
      stdoutMaxBytes: preview.stdoutMaxBytes,
      stderrMaxBytes: preview.stderrMaxBytes,
    });
    this.observeTerminal(session, native);
    const finishedAt = session.finishedAtMs;
    const now = host.nowMs();
    return {
      process_id: session.id,
      backend_id: this.backendId,
      status: native.status,
      exit_code: native.exitCode,
      signal: native.signal,
      timed_out: native.timedOut,
      started_at: new Date(session.startedAtMs).toISOString(),
      finished_at: finishedAt === null ? null : new Date(finishedAt).toISOString(),
      duration_ms: (finishedAt ?? now) - session.startedAtMs,
      stdin_available: native.stdinAvailable,
      tty: false,
      output_cursor: null,
      stdout: describeOutput(native, 'stdout'),
      stderr: describeOutput(native, 'stderr'),
    };
  }

  private observeTerminal(session: ProcessSession, snapshot: NativeProcessSnapshot): void {
    if (snapshot.status === 'running' || session.finishedAtMs !== null) return;
    session.finishedAtMs = host.nowMs();
  }

  private require(processId: string): ProcessSession {
    const session = this.sessions.get(processId);
    if (session === undefined) {
      throw protocolError('process_not_found', `Process '${processId}' was not found.`, 404);
    }
    return session;
  }

  private async pruneForAdmission(): Promise<void> {
    const now = host.nowMs();
    for (const session of [...this.sessions.values()]) {
      if (
        session.finishedAtMs !== null &&
        now - session.finishedAtMs >= TERMINAL_RETENTION_MS &&
        now >= session.replayRetainUntilMs
      ) {
        await this.remove(session);
      }
    }
    while (this.sessions.size >= SESSION_CAPACITY) {
      const terminal = [...this.sessions.values()].find(
        (session) => session.finishedAtMs !== null && now >= session.replayRetainUntilMs,
      );
      if (terminal === undefined) {
        throw protocolError(
          'process_capacity',
          'Process session capacity is occupied by running or idempotency-retained processes.',
          503,
        );
      }
      await this.remove(terminal);
    }
  }

  private async remove(session: ProcessSession): Promise<void> {
    if (this.sessions.get(session.id) !== session) return;
    this.sessions.delete(session.id);
    await session.process.close();
  }
}

function describeOutput(
  snapshot: NativeProcessSnapshot,
  stream: 'stdout' | 'stderr',
): OutputDescriptor {
  if (stream === 'stdout') {
    return {
      preview: snapshot.stdout,
      total_bytes: snapshot.stdoutBytes,
      truncated: snapshot.stdoutTruncated,
    };
  }
  return {
    preview: snapshot.stderr,
    total_bytes: snapshot.stderrBytes,
    truncated: snapshot.stderrTruncated,
  };
}

function parseCursor(cursor: string | undefined): { sequence: number; byteOffset: number } {
  if (cursor === undefined) return { sequence: 1, byteOffset: 0 };
  const match = /^([1-9][0-9]*):([0-9]+)$/.exec(cursor);
  if (match === null) {
    throw protocolError('validation_error', 'cursor is not a valid process cursor.', 400);
  }
  const sequence = Number(match[1]);
  const byteOffset = Number(match[2]);
  if (
    !Number.isSafeInteger(sequence) ||
    !Number.isSafeInteger(byteOffset) ||
    byteOffset > PROCESS_READ_MAX_BYTES
  ) {
    throw protocolError('validation_error', 'cursor is outside the supported range.', 400);
  }
  return { sequence, byteOffset };
}

function encodeCursor(sequence: number, byteOffset: number): string {
  return `${sequence}:${byteOffset}`;
}

function protocolError(
  code: string,
  message: string,
  statusCode: number,
): Error & { code: string; statusCode: number } {
  return Object.assign(new Error(message), { code, statusCode });
}
