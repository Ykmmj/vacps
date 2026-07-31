import * as crypto from 'vacps:crypto';
import * as host from 'vacps:host';
import * as process from 'vacps:process';

import { resolveExecutable } from '../util/resolve-executable';
import { assertSafeAbsolutePath } from './path-guard';

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
  /** When false, leave stdin open for process.write (default true for exec). */
  closeStdin?: boolean;
  /** Request TTY semantics for status reporting (native may not allocate a real PTY). */
  tty?: boolean;
};

interface IdempotencyRecord {
  snapshot: ProcessSnapshot;
  requestHash: string;
}

interface Tracked {
  id: string;
  backendId: string;
  startedMs: number;
  stdoutMax: number;
  stderrMax: number;
  hardMaxStdout?: number;
  hardMaxStderr?: number;
  tty?: boolean;
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

function mapStatus(status: string, timedOut: boolean): ProcessStatus {
  if (timedOut || status === 'timed_out') return 'timed_out';
  if (status === 'cancelled') return 'cancelled';
  if (status === 'running') return 'running';
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

/**
 * Process manager (apps/vacps ProcessManager counterpart).
 * Long-lived: start/read/write/terminate via vacps:process.
 * Fire-and-wait: exec() waits until eof.
 */
export class ProcessManager {
  private readonly idempotency = new Map<string, IdempotencyRecord>();
  private readonly tracked = new Map<string, Tracked>();

  constructor(private readonly backendId: string) {}

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

    const started = await this.start(input);
    const snap = await this.waitUntilDone(started.process_id, {
      stdoutMax: input.stdoutMaxBytes ?? 16_384,
      stderrMax: input.stderrMaxBytes ?? 16_384,
    });

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
    const cwd = input.workingDirectory ? assertSafeAbsolutePath(input.workingDirectory) : '/tmp';
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

    const startedMs = host.nowMs();
    const started = await process.start(argv, {
      cwd,
      timeoutMs,
      closeStdin,
      hardMaxStdout,
      hardMaxStderr,
    });
    this.tracked.set(started.id, {
      id: started.id,
      backendId: this.backendId,
      startedMs,
      stdoutMax,
      stderrMax,
      hardMaxStdout,
      hardMaxStderr,
      tty: wantTty,
    });

    return {
      process_id: started.id,
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

  async readWait(
    processId: string,
    input: {
      cursor?: string;
      maxBytes?: number;
      waitMs?: number;
    } = {},
  ) {
    const { stdoutOffset, stderrOffset } = parseCursor(input.cursor);
    const r = await process.read(processId, {
      waitMs: clamp(input.waitMs ?? 0, 0, 60_000),
      maxBytes: clamp(input.maxBytes ?? 65_536, 1, 1_048_576),
      stdoutOffset,
      stderrOffset,
    });
    const chunks: Array<{
      sequence: number;
      stream: 'stdout' | 'stderr';
      data: string;
      observed_at: string;
    }> = [];
    let seq = 1;
    if (r.stdout) {
      chunks.push({
        sequence: seq++,
        stream: 'stdout',
        data: r.stdout,
        observed_at: new Date(host.nowMs()).toISOString(),
      });
    }
    if (r.stderr) {
      chunks.push({
        sequence: seq++,
        stream: 'stderr',
        data: r.stderr,
        observed_at: new Date(host.nowMs()).toISOString(),
      });
    }
    return {
      process_id: processId,
      status: mapStatus(r.status, r.timedOut),
      chunks,
      next_cursor: encodeCursor(r.nextStdoutOffset, r.nextStderrOffset),
      eof: r.eof,
      exit_code: r.status === 'running' ? null : r.exitCode,
      signal: r.timedOut ? 'SIGKILL' : null,
      returned_bytes: r.stdout.length + r.stderr.length,
    };
  }

  async write(
    processId: string,
    data: string,
    closeStdin = false,
  ): Promise<{ written_bytes: number }> {
    const r = await process.write(processId, data, { close: closeStdin });
    return { written_bytes: r.writtenBytes };
  }

  /** Free native registry entry (buffers). Safe to call after waitUntilDone. */
  async close(processId: string): Promise<{ closed: boolean }> {
    this.tracked.delete(processId);
    const r = await process.close(processId);
    return { closed: r.closed };
  }

  async terminate(
    processId: string,
    signal: 'sigterm' | 'sigint' | 'sigkill' = 'sigterm',
    gracePeriodMs = 3_000,
  ): Promise<
    ProcessSnapshot & {
      termination_requested: boolean;
      requested_signal: string;
    }
  > {
    const nodeSignal =
      signal === 'sigkill' ? 'SIGKILL' : signal === 'sigint' ? 'SIGINT' : 'SIGTERM';
    const r = await process.terminate(processId, {
      signal: nodeSignal,
      graceMs: clamp(gracePeriodMs, 0, 60_000),
    });
    const snap = await this.snapshot(processId);
    return {
      ...snap,
      termination_requested: r.requested,
      requested_signal: nodeSignal,
    };
  }

  async snapshot(
    processId: string,
    limits: { stdoutMax?: number; stderrMax?: number } = {},
  ): Promise<ProcessSnapshot> {
    const tracked = this.tracked.get(processId);
    const r = await process.read(processId, {
      waitMs: 0,
      maxBytes: 1_048_576,
      stdoutOffset: 0,
      stderrOffset: 0,
    });
    // Re-read full buffers: process.read with offset 0 returns slices up to maxBytes.
    // For snapshot use totals + first max of available slice (accumulated in registry).
    const stdoutMax = limits.stdoutMax ?? tracked?.stdoutMax ?? 16_384;
    const stderrMax = limits.stderrMax ?? tracked?.stderrMax ?? 16_384;
    const startedMs = tracked?.startedMs ?? host.nowMs();
    const finished = r.status !== 'running';
    const finishedMs = finished ? host.nowMs() : null;
    return {
      process_id: processId,
      backend_id: tracked?.backendId ?? this.backendId,
      status: mapStatus(r.status, r.timedOut),
      exit_code: finished && !r.timedOut ? r.exitCode : r.timedOut ? null : null,
      signal: r.timedOut ? 'SIGKILL' : r.status === 'cancelled' ? 'SIGTERM' : null,
      timed_out: r.timedOut,
      started_at: new Date(startedMs).toISOString(),
      finished_at: finishedMs ? new Date(finishedMs).toISOString() : null,
      duration_ms: finishedMs ? finishedMs - startedMs : host.nowMs() - startedMs,
      stdin_available: r.stdinOpen,
      tty: tracked?.tty === true,
      output_cursor: null,
      stdout: preview(r.stdout, stdoutMax),
      stderr: preview(r.stderr, stderrMax),
    };
  }

  /** Block until process finishes; returns final snapshot. */
  async waitUntilDone(
    processId: string,
    limits: { stdoutMax?: number; stderrMax?: number } = {},
  ): Promise<ProcessSnapshot> {
    let stdoutOff = 0;
    let stderrOff = 0;
    let stdout = '';
    let stderr = '';
    for (;;) {
      const r = await process.read(processId, {
        waitMs: 500,
        maxBytes: 256_000,
        stdoutOffset: stdoutOff,
        stderrOffset: stderrOff,
      });
      if (r.stdout) stdout += r.stdout;
      if (r.stderr) stderr += r.stderr;
      stdoutOff = r.nextStdoutOffset;
      stderrOff = r.nextStderrOffset;
      // Complete only when eof (exit + both pipe EOFs + buffer fully read).
      if (r.eof) {
        const tracked = this.tracked.get(processId);
        const startedMs = tracked?.startedMs ?? host.nowMs();
        const finishedMs = host.nowMs();
        const stdoutMax = limits.stdoutMax ?? tracked?.stdoutMax ?? 16_384;
        const stderrMax = limits.stderrMax ?? tracked?.stderrMax ?? 16_384;
        return {
          process_id: processId,
          backend_id: tracked?.backendId ?? this.backendId,
          status: mapStatus(r.status, r.timedOut),
          exit_code: r.timedOut ? null : r.exitCode,
          signal: r.timedOut ? 'SIGKILL' : r.status === 'cancelled' ? 'SIGTERM' : null,
          timed_out: r.timedOut,
          started_at: new Date(startedMs).toISOString(),
          finished_at: new Date(finishedMs).toISOString(),
          duration_ms: finishedMs - startedMs,
          stdin_available: false,
          tty: tracked?.tty === true,
          output_cursor: null,
          stdout: preview(stdout, stdoutMax),
          stderr: preview(stderr, stderrMax),
        };
      }
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
