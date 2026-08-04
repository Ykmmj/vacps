import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';
import * as log from 'vacps:log';
import { Process, run, type ProcessResult } from 'vacps:process';

import { requireAbsolutePath } from '../util/absolute-path';
import { resolveExecutable } from '../util/resolve-executable';
import { randomUuidV4 } from '../util/uuid';

export type ProcessStatus = 'running' | 'exited' | 'signaled' | 'timed_out' | 'cancelled';

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
  output_cursor: string | null;
  stdout: { preview: string; total_bytes: number; truncated: boolean };
  stderr: { preview: string; total_bytes: number; truncated: boolean };
  idempotency?: { key: string; replayed: boolean; request_hash: string };
}

export type ExecInput = {
  toolName?: string;
  program?: string;
  arguments?: string[];
  command?: string;
  shell?: string;
  workingDirectory?: string;
  timeoutMs?: number;
  stdoutMaxBytes?: number;
  stderrMaxBytes?: number;
  /** Hard cap on accumulated stdout (process continues after cap). */
  hardMaxStdout?: number;
  hardMaxStderr?: number;
  stdoutHardMaxBytes?: number;
  stderrHardMaxBytes?: number;
  idempotencyKey?: string;
  loadUserEnvironment?: boolean;
  /** When false, leave stdin open for Process.write (default true for exec). */
  closeStdin?: boolean;
  /** Request TTY semantics for status reporting (native may not allocate a real PTY). */
  tty?: boolean;
};

interface IdempotencyRecord {
  snapshot: ProcessSnapshot;
  requestHash: string;
}

interface Tracked {
  /** Client-facing id (UUID); not a native registry id. */
  id: string;
  proc: Process;
  backendId: string;
  startedMs: number;
  stdoutMax: number;
  stderrMax: number;
  hardMaxStdout?: number;
  hardMaxStderr?: number;
  tty?: boolean;
  stdinAvailable: boolean;
  /**
   * Final retained capture copied from Process.wait() (not native progressive
   * streaming — vacps:process has no Process.read).
   */
  stdoutAcc: string;
  stderrAcc: string;
  /** True after terminate() was requested. */
  terminateRequested: boolean;
  requestedSignal: string | null;
  result: ProcessResult | null;
  done: boolean;
  finishedMs: number | null;
  /** Background waiter; resolves when Process.wait settles with final capture. */
  pump: Promise<void>;
  /** Waiters notified when final capture is available or the process finishes. */
  waiters: Array<() => void>;
}

function clamp(n: number, min: number, max: number): number {
  if (!Number.isFinite(n)) return min;
  return Math.min(max, Math.max(min, n));
}

function preview(
  data: string,
  max: number,
): {
  preview: string;
  total_bytes: number;
  truncated: boolean;
} {
  const total = data.length;
  if (data.length <= max) {
    return { preview: data, total_bytes: total, truncated: false };
  }
  return { preview: data.slice(0, max), total_bytes: total, truncated: true };
}

function mapFinishedStatus(t: Tracked): ProcessStatus {
  if (t.result?.timedOut) return 'timed_out';
  if (t.terminateRequested) return 'cancelled';
  return 'exited';
}

export function canonicalRequestHash(
  backendId: string,
  toolName: string,
  input: ExecInput,
): string {
  const material = JSON.stringify({
    backend_id: backendId,
    tool_name: toolName,
    program: input.program ?? null,
    arguments: input.arguments ?? null,
    command: input.command ?? null,
    shell: input.shell ?? null,
    working_directory: input.workingDirectory ?? null,
    timeout_ms: input.timeoutMs ?? null,
    load_user_environment: input.loadUserEnvironment ?? null,
  });
  return crypto.sha256Hex(material);
}

function notifyWaiters(t: Tracked): void {
  const pending = t.waiters.splice(0);
  for (const w of pending) w();
}

function waitForProgress(t: Tracked): Promise<void> {
  if (t.done) return Promise.resolve();
  return new Promise<void>((resolve) => {
    t.waiters.push(resolve);
  });
}

/**
 * Process manager (apps/vacps ProcessManager counterpart).
 * Tracks Process instances under client-facing UUIDs (not native registry ids).
 * Long-lived: start / readWait / write / terminate / close.
 * readWait exposes manager-local slices of the final retained capture after
 * Process.wait completes — not native progressive streaming.
 * Fire-and-wait: exec() → waitUntilDone via Process.wait().
 */
export class ProcessManager {
  private readonly idempotency = new Map<string, IdempotencyRecord>();
  private readonly tracked = new Map<string, Tracked>();

  constructor(private readonly backendId: string) {}

  /**
   * Fire-and-wait exec. Uses vacps:process `run()` (start+wait+close in one native
   * coroutine) so the JS event loop cannot park forever between Process.start and
   * Process.wait — that split path was hanging JsTasksTest.ExecAndFsRoutes.
   * Long-lived processes still use start() / Process handles.
   */
  async exec(input: ExecInput): Promise<ProcessSnapshot> {
    const toolName = input.toolName ?? (input.command ? 'shell.exec' : 'command.exec');
    const requestHash = canonicalRequestHash(this.backendId, toolName, input);

    if (input.idempotencyKey) {
      const key = `${toolName}\0${input.idempotencyKey}`;
      const existing = this.idempotency.get(key);
      if (existing) {
        if (existing.requestHash !== requestHash) {
          throw Object.assign(
            new Error('The idempotency key was previously used with different arguments.'),
            { code: 'idempotency_conflict', statusCode: 409 },
          );
        }
        return {
          ...existing.snapshot,
          idempotency: {
            key: input.idempotencyKey,
            replayed: true,
            request_hash: requestHash,
          },
        };
      }
    }

    const { command, args, cwd, timeoutMs, stdoutMax, stderrMax, hardMaxStdout, hardMaxStderr } =
      await this.buildExecArgs(input);

    log.info(`ProcessManager.exec run ${command} timeoutMs=${timeoutMs}`);
    const startedMs = host.nowMs();
    const result = await run(command, args, {
      cwd,
      timeoutMs,
      maxStdoutBytes: hardMaxStdout,
      maxStderrBytes: hardMaxStderr,
    });
    log.info(`ProcessManager.exec done exit=${result.exitCode} timedOut=${result.timedOut}`);
    const finishedMs = host.nowMs();
    const id = randomUuidV4();
    const timedOut = result.timedOut === true;
    const snap: ProcessSnapshot = {
      process_id: id,
      backend_id: this.backendId,
      status: timedOut ? 'timed_out' : 'exited',
      exit_code: timedOut ? null : result.exitCode,
      signal: timedOut ? 'SIGKILL' : null,
      timed_out: timedOut,
      started_at: new Date(startedMs).toISOString(),
      finished_at: new Date(finishedMs).toISOString(),
      duration_ms: finishedMs - startedMs,
      stdin_available: false,
      tty: input.tty === true,
      output_cursor: null,
      stdout: preview(result.stdout ?? '', stdoutMax),
      stderr: preview(result.stderr ?? '', stderrMax),
    };

    if (input.idempotencyKey) {
      this.idempotency.set(`${toolName}\0${input.idempotencyKey}`, {
        snapshot: snap,
        requestHash,
      });
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

  private async buildExecArgs(input: ExecInput): Promise<{
    command: string;
    args: string[];
    cwd: string;
    timeoutMs: number;
    stdoutMax: number;
    stderrMax: number;
    hardMaxStdout: number;
    hardMaxStderr: number;
  }> {
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

    const timeoutMs = clamp(input.timeoutMs ?? 3_600_000, 1, 3_600_000);
    const cwd = input.workingDirectory ? requireAbsolutePath(input.workingDirectory) : '/tmp';
    const stdoutMax = clamp(input.stdoutMaxBytes ?? 16_384, 0, 1_048_576);
    const stderrMax = clamp(input.stderrMaxBytes ?? 16_384, 0, 1_048_576);

    let argv: string[];
    if (input.command) {
      const shell = input.shell === '/bin/sh' ? '/bin/sh' : '/bin/bash';
      const loadUserEnvironment =
        input.loadUserEnvironment !== undefined ? input.loadUserEnvironment : true;
      if (shell === '/bin/sh' && loadUserEnvironment) {
        throw Object.assign(
          new Error(
            'load_user_environment=true is not supported with shell=/bin/sh; use /bin/bash or set load_user_environment=false.',
          ),
          { code: 'validation_error', statusCode: 400 },
        );
      }
      const shellArgs =
        shell === '/bin/sh'
          ? ['-c', input.command]
          : loadUserEnvironment
            ? ['-lc', input.command]
            : ['--noprofile', '--norc', '-c', input.command];
      argv = [await resolveExecutable(shell), ...shellArgs];
    } else {
      argv = [await resolveExecutable(input.program!), ...(input.arguments ?? [])];
    }

    const hardMaxStdout = clamp(
      input.hardMaxStdout ?? input.stdoutHardMaxBytes ?? 16 * 1024 * 1024,
      0,
      64 * 1024 * 1024,
    );
    const hardMaxStderr = clamp(
      input.hardMaxStderr ?? input.stderrHardMaxBytes ?? 16 * 1024 * 1024,
      0,
      64 * 1024 * 1024,
    );

    return {
      command: argv[0]!,
      args: argv.slice(1),
      cwd,
      timeoutMs,
      stdoutMax,
      stderrMax,
      hardMaxStdout,
      hardMaxStderr,
    };
  }

  async start(input: ExecInput): Promise<ProcessSnapshot> {
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

    const timeoutMs = clamp(input.timeoutMs ?? 3_600_000, 1, 3_600_000);
    const cwd = input.workingDirectory ? requireAbsolutePath(input.workingDirectory) : '/tmp';
    const stdoutMax = clamp(input.stdoutMaxBytes ?? 16_384, 0, 1_048_576);
    const stderrMax = clamp(input.stderrMaxBytes ?? 16_384, 0, 1_048_576);
    const closeStdin = input.closeStdin !== false;

    let argv: string[];
    const wantTty = input.tty === true;
    if (input.command) {
      const shell = input.shell === '/bin/sh' ? '/bin/sh' : '/bin/bash';
      const loadUserEnvironment =
        input.loadUserEnvironment !== undefined ? input.loadUserEnvironment : true;
      if (shell === '/bin/sh' && loadUserEnvironment) {
        throw Object.assign(
          new Error(
            'load_user_environment=true is not supported with shell=/bin/sh; use /bin/bash or set load_user_environment=false.',
          ),
          { code: 'validation_error', statusCode: 400 },
        );
      }
      const shellArgs =
        shell === '/bin/sh'
          ? ['-c', input.command]
          : loadUserEnvironment
            ? ['-lc', input.command]
            : ['--noprofile', '--norc', '-c', input.command];
      argv = [await resolveExecutable(shell), ...shellArgs];
    } else {
      argv = [await resolveExecutable(input.program!), ...(input.arguments ?? [])];
    }

    const hardMaxStdout = clamp(
      input.hardMaxStdout ?? input.stdoutHardMaxBytes ?? 16 * 1024 * 1024,
      0,
      64 * 1024 * 1024,
    );
    const hardMaxStderr = clamp(
      input.hardMaxStderr ?? input.stderrHardMaxBytes ?? 16 * 1024 * 1024,
      0,
      64 * 1024 * 1024,
    );

    const command = argv[0]!;
    const args = argv.slice(1);
    const proc = new Process(command, args, {
      cwd,
      timeoutMs,
      stdin: closeStdin ? 'ignore' : 'pipe',
      maxStdoutBytes: hardMaxStdout,
      maxStderrBytes: hardMaxStderr,
    });

    const startedMs = host.nowMs();
    await proc.start();

    const id = randomUuidV4();
    const tracked: Tracked = {
      id,
      proc,
      backendId: this.backendId,
      startedMs,
      stdoutMax,
      stderrMax,
      hardMaxStdout,
      hardMaxStderr,
      tty: wantTty,
      stdinAvailable: !closeStdin,
      stdoutAcc: '',
      stderrAcc: '',
      terminateRequested: false,
      requestedSignal: null,
      result: null,
      done: false,
      finishedMs: null,
      pump: Promise.resolve(),
      waiters: [],
    };
    tracked.pump = this.pumpOutputs(tracked);
    this.tracked.set(id, tracked);

    return {
      process_id: id,
      backend_id: this.backendId,
      status: 'running',
      exit_code: null,
      signal: null,
      timed_out: false,
      started_at: new Date(startedMs).toISOString(),
      finished_at: null,
      duration_ms: null,
      stdin_available: !closeStdin,
      tty: wantTty,
      output_cursor: null,
      stdout: preview('', stdoutMax),
      stderr: preview('', stderrMax),
    };
  }

  /**
   * Output for long-lived processes from the final retained capture after
   * Process.wait completes. Cursor is manager-local (delivered offsets only).
   * Not native progressive streaming (no Process.read).
   */
  async readWait(
    processId: string,
    input: {
      cursor?: string;
      maxBytes?: number;
      waitMs?: number;
    } = {},
  ) {
    const t = this.require(processId);
    const maxBytes = clamp(input.maxBytes ?? 65_536, 1, 1_048_576);
    const waitMs = clamp(input.waitMs ?? 0, 0, 60_000);
    let { stdoutOffset, stderrOffset } = parseCursor(input.cursor);

    // Clamp cursor to the final capture we hold (may still be empty until wait settles).
    stdoutOffset = Math.min(stdoutOffset, t.stdoutAcc.length);
    stderrOffset = Math.min(stderrOffset, t.stderrAcc.length);

    const hasNew = () =>
      stdoutOffset < t.stdoutAcc.length || stderrOffset < t.stderrAcc.length || t.done;

    if (!hasNew() && waitMs > 0) {
      // Long-poll until wait settles / capture is available.
      while (!hasNew()) {
        await waitForProgress(t);
      }
    }

    const chunks: Array<{
      sequence: number;
      stream: 'stdout' | 'stderr';
      data: string;
      observed_at: string;
    }> = [];
    let seq = 1;
    let budget = maxBytes;
    const observedAt = new Date(host.nowMs()).toISOString();

    if (stdoutOffset < t.stdoutAcc.length && budget > 0) {
      const data = t.stdoutAcc.slice(stdoutOffset, stdoutOffset + budget);
      budget -= data.length;
      stdoutOffset += data.length;
      if (data) {
        chunks.push({ sequence: seq++, stream: 'stdout', data, observed_at: observedAt });
      }
    }
    if (stderrOffset < t.stderrAcc.length && budget > 0) {
      const data = t.stderrAcc.slice(stderrOffset, stderrOffset + budget);
      stderrOffset += data.length;
      if (data) {
        chunks.push({ sequence: seq++, stream: 'stderr', data, observed_at: observedAt });
      }
    }

    const eof =
      t.done && stdoutOffset >= t.stdoutAcc.length && stderrOffset >= t.stderrAcc.length;
    const status: ProcessStatus = t.done ? mapFinishedStatus(t) : 'running';
    const exitCode = t.done && t.result && !t.result.timedOut ? t.result.exitCode : null;

    return {
      process_id: processId,
      status,
      chunks,
      next_cursor: encodeCursor(stdoutOffset, stderrOffset),
      eof,
      exit_code: status === 'running' ? null : exitCode,
      signal: t.result?.timedOut
        ? 'SIGKILL'
        : t.terminateRequested
          ? (t.requestedSignal ?? 'SIGTERM')
          : null,
      returned_bytes: chunks.reduce((n, c) => n + c.data.length, 0),
    };
  }

  async write(
    processId: string,
    data: string,
    closeStdin = false,
  ): Promise<{ written_bytes: number }> {
    const t = this.require(processId);
    const written = await t.proc.write(data);
    if (closeStdin) {
      // Process.write has no close-stdin option; mark locally for status reporting.
      t.stdinAvailable = false;
    }
    return { written_bytes: written };
  }

  /** Free Process (buffers). Safe to call after waitUntilDone. */
  async close(processId: string): Promise<{ closed: boolean }> {
    const t = this.tracked.get(processId);
    if (!t) return { closed: false };
    this.tracked.delete(processId);
    t.waiters.splice(0);
    try {
      await t.proc.close();
    } catch {
      /* idempotent / already closed */
    }
    return { closed: true };
  }

  /**
   * Application shutdown: terminate then close every tracked Process.
   * Host does not enumerate processes — JS owns this set.
   */
  async closeAll(): Promise<void> {
    const ids = [...this.tracked.keys()];
    for (const id of ids) {
      try {
        await this.terminate(id, 'sigterm');
      } catch {
        /* best-effort */
      }
      try {
        await this.close(id);
      } catch {
        /* best-effort */
      }
    }
  }

  async terminate(
    processId: string,
    signal: 'sigterm' | 'sigint' | 'sigkill' = 'sigterm',
    _gracePeriodMs = 3_000,
  ): Promise<
    ProcessSnapshot & {
      termination_requested: boolean;
      requested_signal: string;
    }
  > {
    const t = this.require(processId);
    const nodeSignal =
      signal === 'sigkill' ? 'SIGKILL' : signal === 'sigint' ? 'SIGINT' : 'SIGTERM';
    t.terminateRequested = true;
    t.requestedSignal = nodeSignal;
    let requested = true;
    try {
      await t.proc.terminate(nodeSignal);
    } catch {
      requested = false;
    }
    // Let the wait-side capture settle when possible.
    try {
      await t.pump;
    } catch {
      /* ignore */
    }
    const snap = await this.snapshot(processId);
    return {
      ...snap,
      termination_requested: requested,
      requested_signal: nodeSignal,
    };
  }

  async snapshot(
    processId: string,
    limits: { stdoutMax?: number; stderrMax?: number } = {},
  ): Promise<ProcessSnapshot> {
    const t = this.require(processId);
    const stdoutMax = limits.stdoutMax ?? t.stdoutMax ?? 16_384;
    const stderrMax = limits.stderrMax ?? t.stderrMax ?? 16_384;
    const startedMs = t.startedMs;
    const finished = t.done;
    const finishedMs = finished ? (t.finishedMs ?? host.nowMs()) : null;
    const timedOut = t.result?.timedOut === true;
    const status: ProcessStatus = finished ? mapFinishedStatus(t) : 'running';

    // Final retained capture from Process.wait (copied into stdoutAcc/stderrAcc).
    const stdout = t.stdoutAcc || t.result?.stdout || '';
    const stderr = t.stderrAcc || t.result?.stderr || '';

    return {
      process_id: processId,
      backend_id: t.backendId,
      status,
      exit_code: finished && t.result && !timedOut ? t.result.exitCode : null,
      signal: timedOut
        ? 'SIGKILL'
        : t.terminateRequested
          ? (t.requestedSignal ?? 'SIGTERM')
          : null,
      timed_out: timedOut,
      started_at: new Date(startedMs).toISOString(),
      finished_at: finishedMs ? new Date(finishedMs).toISOString() : null,
      duration_ms: finishedMs ? finishedMs - startedMs : host.nowMs() - startedMs,
      stdin_available: finished ? false : t.stdinAvailable,
      tty: t.tty === true,
      output_cursor: null,
      stdout: preview(stdout, stdoutMax),
      stderr: preview(stderr, stderrMax),
    };
  }

  /** Block until process finishes; returns final snapshot. */
  async waitUntilDone(
    processId: string,
    limits: { stdoutMax?: number; stderrMax?: number } = {},
  ): Promise<ProcessSnapshot> {
    const t = this.require(processId);
    await t.pump;
    return this.snapshot(processId, limits);
  }

  private require(processId: string): Tracked {
    const t = this.tracked.get(processId);
    if (!t) {
      throw Object.assign(new Error(`Unknown process_id: ${processId}`), {
        code: 'not_found',
        statusCode: 404,
      });
    }
    return t;
  }

  /**
   * Await Process.wait() for the final retained stdout/stderr capture.
   * readWait serves manager-local slices of that capture after wait settles;
   * it is not native progressive streaming (vacps:process has no Process.read).
   */
  private async pumpOutputs(t: Tracked): Promise<void> {
    const p = t.proc;
    try {
      t.result = await p.wait();
      if (t.result.stdout) t.stdoutAcc = t.result.stdout;
      if (t.result.stderr) t.stderrAcc = t.result.stderr;
    } catch {
      /* closed mid-wait — capture unavailable */
    } finally {
      t.done = true;
      t.finishedMs = host.nowMs();
      t.stdinAvailable = false;
      notifyWaiters(t);
    }
  }
}

function parseCursor(cursor: string | undefined): {
  stdoutOffset: number;
  stderrOffset: number;
} {
  if (!cursor) return { stdoutOffset: 0, stderrOffset: 0 };
  if (cursor.includes(':')) {
    const [a, b] = cursor.split(':');
    return {
      stdoutOffset: Math.max(0, Number(a) || 0),
      stderrOffset: Math.max(0, Number(b) || 0),
    };
  }
  return { stdoutOffset: Math.max(0, Number(cursor) || 0), stderrOffset: 0 };
}

function encodeCursor(stdoutOffset: number, stderrOffset: number): string {
  return `${stdoutOffset}:${stderrOffset}`;
}
