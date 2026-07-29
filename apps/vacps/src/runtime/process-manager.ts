import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { userInfo } from 'node:os';

import { describeOutput, type OutputDescriptor } from './output.js';
import { assertSafeAbsolutePath } from './path-guard.js';

export type ProcessStatus = 'running' | 'exited' | 'signaled' | 'timed_out' | 'cancelled';

export interface ProcessChunk {
  sequence: number;
  stream: 'stdout' | 'stderr';
  data: string;
  observed_at: string;
  /** Inclusive byte offset within the original stream buffer (UTF-8). */
  offset_start?: number;
  /** Exclusive byte offset within the original stream buffer (UTF-8). */
  offset_end?: number;
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
  /** Opaque cursor for process.read; null means start from the beginning. */
  output_cursor: string | null;
  stdout: OutputDescriptor;
  stderr: OutputDescriptor;
  idempotency?: { key: string; replayed: boolean; request_hash: string };
}

interface IdempotencyRecord {
  processId: string;
  requestHash: string;
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

export type ExecInput = {
  toolName?: string | undefined;
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
  /**
   * Shell only: load agent login environment (.bashrc / profile).
   * Default true for shell.exec so PATH and user tooling match a real agent login.
   * Set false for a clean non-interactive shell (--noprofile --norc).
   */
  loadUserEnvironment?: boolean | undefined;
};

export class ProcessManager {
  private readonly processes = new Map<string, ManagedProcess>();
  /** key = `${toolName}\0${idempotencyKey}` → process + request hash */
  private readonly idempotency = new Map<string, IdempotencyRecord>();

  constructor(private readonly backendId: string) {}

  async exec(input: ExecInput): Promise<ProcessSnapshot> {
    const toolName = input.toolName ?? (input.command ? 'shell.exec' : 'command.exec');
    const requestHash = canonicalRequestHash(this.backendId, toolName, input);

    if (input.idempotencyKey) {
      const storeKey = idempotencyStoreKey(toolName, input.idempotencyKey);
      const existing = this.idempotency.get(storeKey);
      if (existing) {
        if (existing.requestHash !== requestHash) {
          throw Object.assign(
            new Error('The idempotency key was previously used with different arguments.'),
            { code: 'idempotency_conflict', statusCode: 409 },
          );
        }
        const snap = this.snapshot(existing.processId, {
          stdoutMax: input.stdoutMaxBytes ?? 16_384,
          stderrMax: input.stderrMaxBytes ?? 16_384,
        });
        return {
          ...snap,
          idempotency: {
            key: input.idempotencyKey,
            replayed: true,
            request_hash: requestHash,
          },
        };
      }
    }

    if (input.program && input.command) {
      throw Object.assign(new Error('Provide either program or command, not both.'), {
        code: 'validation_error',
        statusCode: 400,
      });
    }
    if (!input.program && !input.command) {
      throw Object.assign(new Error('program or command is required.'), {
        code: 'validation_error',
        statusCode: 400,
      });
    }

    const timeoutMs = clamp(input.timeoutMs ?? 120_000, 1, 3_600_000);
    const yieldTimeMs = clamp(input.yieldTimeMs ?? 10_000, 1, 120_000);
    const cwd = input.workingDirectory
      ? assertSafeAbsolutePath(input.workingDirectory)
      : process.cwd();
    // shell.exec defaults to full agent login env; command.exec never uses a shell.
    const loadUserEnvironment =
      input.loadUserEnvironment !== undefined ? input.loadUserEnvironment : Boolean(input.command);
    const env = buildExecEnvironment(input.environment, { loadUserEnvironment });
    const hardMaxStdout = clamp(input.hardMaxStdout ?? 100 * 1024 * 1024, 0, 1024 * 1024 * 1024);
    const hardMaxStderr = clamp(input.hardMaxStderr ?? 100 * 1024 * 1024, 0, 1024 * 1024 * 1024);

    let child: ChildProcessWithoutNullStreams;
    if (input.command) {
      const shell = input.shell === '/bin/sh' ? '/bin/sh' : '/bin/bash';
      // /bin/sh has no portable login+rc load; reject claiming full user environment.
      if (shell === '/bin/sh' && loadUserEnvironment) {
        throw Object.assign(
          new Error(
            'load_user_environment=true is not supported with shell=/bin/sh; use /bin/bash or set load_user_environment=false.',
          ),
          { code: 'validation_error', statusCode: 400 },
        );
      }
      // Login shell (-lc) sources profile/bashrc for the real agent user environment.
      // Opt out with load_user_environment=false → --noprofile --norc -c.
      const shellArgs =
        shell === '/bin/sh'
          ? (['-c', input.command] as string[])
          : loadUserEnvironment
            ? (['-lc', input.command] as string[])
            : (['--noprofile', '--norc', '-c', input.command] as string[]);
      child = spawn(shell, shellArgs, {
        cwd,
        env,
        stdio: ['pipe', 'pipe', 'pipe'],
      });
    } else {
      child = spawn(input.program!, input.arguments ?? [], {
        cwd,
        env,
        stdio: ['pipe', 'pipe', 'pipe'],
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
      hardMaxStdout: Math.max(hardMaxStdout, 1024),
      hardMaxStderr: Math.max(hardMaxStderr, 1024),
      child,
      stdinOpen: true,
      tty: Boolean(input.tty),
      sequence: 0,
      chunks: [],
      waiters: [],
    };
    this.processes.set(id, managed);
    if (input.idempotencyKey) {
      this.idempotency.set(idempotencyStoreKey(toolName, input.idempotencyKey), {
        processId: id,
        requestHash,
      });
    }

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
      return {
        ...snap,
        idempotency: {
          key: input.idempotencyKey,
          replayed: false,
          request_hash: requestHash,
        },
      };
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
    returned_bytes: number;
  } {
    const managed = this.require(processId);
    const maxBytes = clamp(input.maxBytes ?? 65_536, 1, 1_048_576);
    const cursor = parseCursor(input.cursor);

    const chunks: ProcessChunk[] = [];
    let bytes = 0;
    let nextSeq = cursor.sequence;
    let nextOffset = cursor.byteOffset;

    for (const chunk of managed.chunks) {
      if (chunk.sequence < cursor.sequence) continue;
      const buffer = Buffer.from(chunk.data, 'utf8');
      let start = 0;
      if (chunk.sequence === cursor.sequence) {
        start = cursor.byteOffset;
        if (start >= buffer.length) continue;
      }
      if (bytes >= maxBytes) break;

      const remaining = maxBytes - bytes;
      const end = Math.min(buffer.length, start + remaining);
      if (end <= start) break;

      const slice = buffer.subarray(start, end);
      chunks.push({
        sequence: chunk.sequence,
        stream: chunk.stream,
        data: slice.toString('utf8'),
        observed_at: chunk.observed_at,
        offset_start: start,
        offset_end: end,
      });
      bytes += end - start;

      if (end < buffer.length) {
        nextSeq = chunk.sequence;
        nextOffset = end;
      } else {
        nextSeq = chunk.sequence + 1;
        nextOffset = 0;
      }

      if (bytes >= maxBytes) break;
    }

    // eof only when process finished and no unread bytes remain after the next cursor.
    let unread = false;
    for (const chunk of managed.chunks) {
      if (chunk.sequence < nextSeq) continue;
      const buffer = Buffer.from(chunk.data, 'utf8');
      const start = chunk.sequence === nextSeq ? nextOffset : 0;
      if (start < buffer.length) {
        unread = true;
        break;
      }
    }

    return {
      process_id: managed.id,
      status: managed.status,
      chunks,
      next_cursor: encodeCursor(nextSeq, nextOffset),
      eof: managed.status !== 'running' && !unread,
      exit_code: managed.exitCode,
      signal: managed.signal,
      returned_bytes: bytes,
    };
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
      const before = this.read(processId, { ...input, waitMs: 0 });
      if (before.chunks.length === 0) await waitFor(managed, waitMs);
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
  ): ProcessSnapshot & {
    termination_requested: boolean;
    requested_signal: string;
  } {
    const managed = this.require(processId);
    if (managed.status !== 'running') {
      return {
        ...this.snapshot(processId),
        termination_requested: false,
        requested_signal: signal,
      };
    }

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
    const snap = this.snapshot(processId);
    return {
      ...snap,
      // Still running immediately after signal is expected for async terminate.
      status: snap.status === 'running' ? 'running' : snap.status,
      termination_requested: true,
      requested_signal: nodeSignal,
    };
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
      // process.start/command.exec share the same snapshot; clients pass cursor opaquely to process.read.
      output_cursor: null,
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

export function canonicalRequestHash(
  backendId: string,
  toolName: string,
  input: ExecInput,
): string {
  // Exclude yield/preview limits from the identity of the work — they affect
  // response packaging, not which process is replayed.
  const payload = {
    backend_id: backendId,
    tool: toolName,
    program: input.program ?? null,
    arguments: input.arguments ?? null,
    command: input.command ?? null,
    shell: input.shell ?? null,
    working_directory: input.workingDirectory ?? null,
    environment: input.environment ?? null,
    timeout_ms: input.timeoutMs ?? null,
    hard_max_stdout: input.hardMaxStdout ?? null,
    hard_max_stderr: input.hardMaxStderr ?? null,
    tty: input.tty ?? null,
    close_stdin: input.closeStdin ?? null,
    load_user_environment: input.loadUserEnvironment ?? false,
  };
  return `sha256:${createHash('sha256').update(stableStringify(payload)).digest('hex')}`;
}

function idempotencyStoreKey(toolName: string, key: string): string {
  return `${toolName}\0${key}`;
}

function stableStringify(value: unknown): string {
  if (value === null || typeof value !== 'object') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map((item) => stableStringify(item)).join(',')}]`;
  const record = value as Record<string, unknown>;
  const keys = Object.keys(record).sort();
  return `{${keys.map((key) => `${JSON.stringify(key)}:${stableStringify(record[key])}`).join(',')}}`;
}

function parseCursor(cursor: string | undefined): { sequence: number; byteOffset: number } {
  if (!cursor) return { sequence: 1, byteOffset: 0 };
  // Formats: "seq" (legacy) or "seq:byteOffset" or base64url JSON
  if (cursor.includes(':')) {
    const [seq, off] = cursor.split(':');
    return {
      sequence: Math.max(1, Number(seq) || 1),
      byteOffset: Math.max(0, Number(off) || 0),
    };
  }
  // Legacy: cursor was "last returned sequence" meaning "start after this sequence"
  const legacy = Number(cursor) || 0;
  return { sequence: legacy + 1, byteOffset: 0 };
}

function encodeCursor(sequence: number, byteOffset: number): string {
  return `${sequence}:${byteOffset}`;
}

function buildExecEnvironment(
  overrides: Record<string, string> | undefined,
  options: { loadUserEnvironment: boolean },
): NodeJS.ProcessEnv {
  let home = process.env.HOME || '/home/agent';
  let username = process.env.USER || 'agent';
  try {
    const user = userInfo();
    if (typeof user.homedir === 'string' && user.homedir) home = user.homedir;
    if (typeof user.username === 'string' && user.username) username = user.username;
  } catch {
    /* keep env defaults */
  }
  const base: NodeJS.ProcessEnv = { ...process.env, ...overrides };
  // Always clear accidental BASH_ENV hijacks; login shells still load ~/.bashrc via -lc.
  delete base.BASH_ENV;
  delete base.ENV;
  base.HOME = overrides?.HOME ?? home;
  base.USER = overrides?.USER ?? username;
  base.LOGNAME = overrides?.LOGNAME ?? process.env.LOGNAME ?? username;
  base.SHELL = overrides?.SHELL ?? process.env.SHELL ?? '/bin/bash';
  // Document intent for operators inspecting process env.
  if (options.loadUserEnvironment) base.VACPS_SHELL_USER_ENV = '1';
  else base.VACPS_SHELL_USER_ENV = '0';
  return base;
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
