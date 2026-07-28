import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { randomBytes } from 'node:crypto';

import { describeOutput, type OutputDescriptor } from './output.js';
import { assertSafeAbsolutePath } from './path-guard.js';

export type ProcessStatus = 'running' | 'exited' | 'signaled' | 'timed_out' | 'cancelled';

export interface ProcessChunk {
  sequence: number;
  stream: 'stdout' | 'stderr';
  data: string;
  observed_at: string;
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
  duration_ms: number | null;
  stdin_available: boolean;
  tty: boolean;
  stdout: OutputDescriptor;
  stderr: OutputDescriptor;
  idempotency?: { key: string; replayed: boolean };
}

interface ManagedProcess {
  id: string;
  backendId: string;
  status: ProcessStatus;
  exitCode: number | null;
  signal: string | null;
  timedOut: boolean;
  startedAt: number;
  finishedAt: number | null;
  stdout: string;
  stderr: string;
  hardMaxStdout: number;
  hardMaxStderr: number;
  child: ChildProcessWithoutNullStreams;
  stdinOpen: boolean;
  tty: boolean;
  sequence: number;
  chunks: ProcessChunk[];
  killTimer?: ReturnType<typeof setTimeout>;
  waiters: Array<() => void>;
}

export class ProcessManager {
  private readonly processes = new Map<string, ManagedProcess>();
  private readonly idempotency = new Map<string, string>();

  constructor(private readonly backendId: string) {}

  async exec(input: {
    program?: string | undefined;
    arguments?: string[] | undefined;
    command?: string | undefined;
    shell?: string | undefined;
    workingDirectory?: string | undefined;
    environment?: Record<string, string> | undefined;
    timeoutMs?: number | undefined;
    yieldTimeMs?: number | undefined;
    stdoutMaxBytes?: number | undefined;
    stderrMaxBytes?: number | undefined;
    hardMaxStdout?: number | undefined;
    hardMaxStderr?: number | undefined;
    tty?: boolean | undefined;
    idempotencyKey?: string | undefined;
    closeStdin?: boolean | undefined;
  }): Promise<ProcessSnapshot> {
    if (input.idempotencyKey) {
      const existingId = this.idempotency.get(input.idempotencyKey);
      if (existingId) {
        const snap = this.snapshot(existingId, {
          stdoutMax: input.stdoutMaxBytes ?? 16_384,
          stderrMax: input.stderrMaxBytes ?? 16_384,
        });
        return { ...snap, idempotency: { key: input.idempotencyKey, replayed: true } };
      }
    }

    const timeoutMs = clamp(input.timeoutMs ?? 120_000, 1, 3_600_000);
    const yieldTimeMs = clamp(input.yieldTimeMs ?? 10_000, 1, 120_000);
    const cwd = input.workingDirectory
      ? assertSafeAbsolutePath(input.workingDirectory)
      : process.cwd();
    const env = { ...process.env, ...input.environment };
    const hardMaxStdout = clamp(input.hardMaxStdout ?? 8 * 1024 * 1024, 1024, 64 * 1024 * 1024);
    const hardMaxStderr = clamp(input.hardMaxStderr ?? 8 * 1024 * 1024, 1024, 64 * 1024 * 1024);

    let child: ChildProcessWithoutNullStreams;
    if (input.command) {
      const shell = input.shell === '/bin/sh' ? '/bin/sh' : '/bin/bash';
      child = spawn(shell, ['-lc', input.command], {
        cwd,
        env,
        stdio: ['pipe', 'pipe', 'pipe'],
      });
    } else if (input.program) {
      child = spawn(input.program, input.arguments ?? [], {
        cwd,
        env,
        stdio: ['pipe', 'pipe', 'pipe'],
      });
    } else {
      throw Object.assign(new Error('program or command is required.'), {
        code: 'validation_error',
        statusCode: 400,
      });
    }

    const id = `proc_${randomBytes(8).toString('hex')}`;
    const managed: ManagedProcess = {
      id,
      backendId: this.backendId,
      status: 'running',
      exitCode: null,
      signal: null,
      timedOut: false,
      startedAt: Date.now(),
      finishedAt: null,
      stdout: '',
      stderr: '',
      hardMaxStdout,
      hardMaxStderr,
      child,
      stdinOpen: true,
      tty: Boolean(input.tty),
      sequence: 0,
      chunks: [],
      waiters: [],
    };
    this.processes.set(id, managed);
    if (input.idempotencyKey) this.idempotency.set(input.idempotencyKey, id);

    if (input.closeStdin !== false && !input.tty) {
      child.stdin.end();
      managed.stdinOpen = false;
    }

    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk: string) => this.append(managed, 'stdout', chunk));
    child.stderr.on('data', (chunk: string) => this.append(managed, 'stderr', chunk));
    child.on('error', (error) => {
      managed.stderr += `${error.message}\n`;
      this.finish(managed, 'exited', 127, null, false);
    });
    child.on('close', (code, signal) => {
      if (managed.status !== 'running') return;
      if (signal) this.finish(managed, 'signaled', code, signal, false);
      else this.finish(managed, 'exited', code, null, false);
    });

    managed.killTimer = setTimeout(() => {
      if (managed.status !== 'running') return;
      managed.timedOut = true;
      managed.child.kill('SIGKILL');
      this.finish(managed, 'timed_out', null, 'SIGKILL', true);
    }, timeoutMs);

    const waitMs = Math.min(yieldTimeMs, timeoutMs);
    await waitFor(managed, waitMs);

    const snap = this.snapshot(id, {
      stdoutMax: input.stdoutMaxBytes ?? 16_384,
      stderrMax: input.stderrMaxBytes ?? 16_384,
    });
    if (input.idempotencyKey) {
      return { ...snap, idempotency: { key: input.idempotencyKey, replayed: false } };
    }
    return snap;
  }

  read(
    processId: string,
    input: {
      cursor?: string | undefined;
      maxBytes?: number | undefined;
      waitMs?: number | undefined;
    } = {},
  ): {
    process_id: string;
    status: ProcessStatus;
    chunks: ProcessChunk[];
    next_cursor: string | null;
    eof: boolean;
    exit_code: number | null;
    signal: string | null;
  } {
    const managed = this.require(processId);
    const maxBytes = clamp(input.maxBytes ?? 65_536, 1, 1_048_576);
    const startSequence = input.cursor ? Number(input.cursor) || 0 : 0;

    const collect = () => {
      const chunks: ProcessChunk[] = [];
      let bytes = 0;
      for (const chunk of managed.chunks) {
        if (chunk.sequence <= startSequence) continue;
        const size = Buffer.byteLength(chunk.data, 'utf8');
        if (bytes + size > maxBytes && chunks.length > 0) break;
        chunks.push(chunk);
        bytes += size;
        if (bytes >= maxBytes) break;
      }
      const last = chunks[chunks.length - 1]?.sequence ?? startSequence;
      return {
        process_id: managed.id,
        status: managed.status,
        chunks,
        next_cursor: chunks.length > 0 ? String(last) : String(startSequence),
        eof: managed.status !== 'running' && last >= (managed.chunks.at(-1)?.sequence ?? 0),
        exit_code: managed.exitCode,
        signal: managed.signal,
      };
    };

    // Synchronous path when data already available or process finished.
    const immediate = collect();
    if (immediate.chunks.length > 0 || managed.status !== 'running' || !input.waitMs) {
      return immediate;
    }
    // waitMs is handled by the HTTP layer via async wait helper.
    return immediate;
  }

  async readWait(
    processId: string,
    input: {
      cursor?: string | undefined;
      maxBytes?: number | undefined;
      waitMs?: number | undefined;
    } = {},
  ) {
    const managed = this.require(processId);
    const waitMs = clamp(input.waitMs ?? 0, 0, 60_000);
    if (waitMs > 0 && managed.status === 'running') {
      const startSequence = input.cursor ? Number(input.cursor) || 0 : 0;
      const hasNew = managed.chunks.some((chunk) => chunk.sequence > startSequence);
      if (!hasNew) await waitFor(managed, waitMs);
    }
    return this.read(processId, input);
  }

  write(processId: string, data: string, closeStdin = false): { written_bytes: number } {
    const managed = this.require(processId);
    if (managed.status !== 'running' || !managed.stdinOpen) {
      throw Object.assign(new Error('Process stdin is not available.'), {
        code: 'stdin_closed',
        statusCode: 409,
      });
    }
    managed.child.stdin.write(data);
    if (closeStdin) {
      managed.child.stdin.end();
      managed.stdinOpen = false;
    }
    return { written_bytes: Buffer.byteLength(data, 'utf8') };
  }

  terminate(
    processId: string,
    signal: 'sigterm' | 'sigint' | 'sigkill' = 'sigterm',
    gracePeriodMs = 3_000,
  ): ProcessSnapshot {
    const managed = this.require(processId);
    if (managed.status !== 'running') return this.snapshot(processId);

    const nodeSignal =
      signal === 'sigkill' ? 'SIGKILL' : signal === 'sigint' ? 'SIGINT' : 'SIGTERM';
    managed.child.kill(nodeSignal);
    if (signal !== 'sigkill') {
      setTimeout(
        () => {
          if (managed.status === 'running') {
            managed.child.kill('SIGKILL');
            this.finish(managed, 'cancelled', null, 'SIGKILL', false);
          }
        },
        clamp(gracePeriodMs, 0, 60_000),
      );
    } else {
      this.finish(managed, 'cancelled', null, 'SIGKILL', false);
    }
    // Give a brief moment for close event when kill is immediate.
    return this.snapshot(processId);
  }

  snapshot(
    processId: string,
    limits: { stdoutMax?: number; stderrMax?: number } = {},
  ): ProcessSnapshot {
    const managed = this.require(processId);
    const finished = managed.finishedAt;
    return {
      process_id: managed.id,
      backend_id: managed.backendId,
      status: managed.status,
      exit_code: managed.exitCode,
      signal: managed.signal,
      timed_out: managed.timedOut,
      started_at: new Date(managed.startedAt).toISOString(),
      finished_at: finished ? new Date(finished).toISOString() : null,
      duration_ms: finished ? finished - managed.startedAt : Date.now() - managed.startedAt,
      stdin_available: managed.status === 'running' && managed.stdinOpen,
      tty: managed.tty,
      stdout: describeOutput(managed.stdout, limits.stdoutMax ?? 16_384, {
        complete: managed.status !== 'running',
        processId: managed.id,
        stream: 'stdout',
      }),
      stderr: describeOutput(managed.stderr, limits.stderrMax ?? 16_384, {
        complete: managed.status !== 'running',
        processId: managed.id,
        stream: 'stderr',
      }),
    };
  }

  private append(managed: ManagedProcess, stream: 'stdout' | 'stderr', chunk: string) {
    if (stream === 'stdout') {
      managed.stdout = trimTo(managed.stdout + chunk, managed.hardMaxStdout);
    } else {
      managed.stderr = trimTo(managed.stderr + chunk, managed.hardMaxStderr);
    }
    managed.sequence += 1;
    managed.chunks.push({
      sequence: managed.sequence,
      stream,
      data: chunk,
      observed_at: new Date().toISOString(),
    });
    // Cap chunk history to last ~2000 entries.
    if (managed.chunks.length > 2000) managed.chunks.splice(0, managed.chunks.length - 2000);
    for (const wake of managed.waiters.splice(0)) wake();
  }

  private finish(
    managed: ManagedProcess,
    status: ProcessStatus,
    code: number | null,
    signal: NodeJS.Signals | string | null,
    timedOut: boolean,
  ) {
    if (managed.status !== 'running') return;
    managed.status = status;
    managed.exitCode = code;
    managed.signal = signal;
    managed.timedOut = timedOut;
    managed.finishedAt = Date.now();
    managed.stdinOpen = false;
    if (managed.killTimer) clearTimeout(managed.killTimer);
    for (const wake of managed.waiters.splice(0)) wake();
  }

  private require(processId: string): ManagedProcess {
    const managed = this.processes.get(processId);
    if (!managed) {
      throw Object.assign(new Error(`Process '${processId}' was not found.`), {
        code: 'process_not_found',
        statusCode: 404,
      });
    }
    return managed;
  }
}

function waitFor(managed: ManagedProcess, ms: number): Promise<void> {
  if (managed.status !== 'running' || ms <= 0) return Promise.resolve();
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      cleanup();
      resolve();
    }, ms);
    const wake = () => {
      if (managed.status !== 'running') {
        cleanup();
        resolve();
      }
    };
    const cleanup = () => {
      clearTimeout(timer);
      managed.waiters = managed.waiters.filter((item) => item !== wake);
    };
    managed.waiters.push(wake);
  });
}

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, Math.trunc(value)));
}

function trimTo(text: string, maxBytes: number): string {
  if (Buffer.byteLength(text, 'utf8') <= maxBytes) return text;
  const buffer = Buffer.from(text, 'utf8');
  return buffer.subarray(buffer.length - maxBytes).toString('utf8');
}
