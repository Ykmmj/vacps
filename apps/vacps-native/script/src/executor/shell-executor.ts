import type { TaskDispatch } from '@vacps/contracts';
import { isTerminalTaskStatus } from '@vacps/contracts';
import * as log from 'vacps:log';
import { Process, type ProcessResult } from 'vacps:process';

import { shellArgvFlags } from '../runtime/process-exec';
import type { TaskStore } from '../storage/task-store';
import { resolveExecutable } from '../util/resolve-executable';

/** Discriminated success/failure for a single try/catch boundary. */
type Attempt<T> = { ok: true; value: T } | { ok: false; error: unknown };

/** Catch only the supplied operation (sync or async). */
async function attempt<T>(operation: () => T | Promise<T>): Promise<Attempt<T>> {
  try {
    return { ok: true, value: await operation() };
  } catch (error) {
    return { ok: false, error };
  }
}

function errorMessage(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

/**
 * Run command/shell tasks via vacps:process Process class
 * (apps/vacps ShellExecutor counterpart). Supports mid-run cancel via
 * Process.terminate while Process.wait is in flight.
 */
export class ShellExecutor {
  /** task_id → Process instance (for cancel while running). */
  private readonly active = new Map<string, Process>();

  constructor(private readonly store: TaskStore) {}

  async cancelRunning(taskId: string): Promise<boolean> {
    const proc = this.active.get(taskId);
    if (!proc) return false;
    try {
      await proc.terminate('SIGKILL');
      return true;
    } catch {
      return false;
    }
  }

  async execute(task: TaskDispatch): Promise<void> {
    const id = task.task_id;
    if (await this.store.isCancelRequested(id)) {
      await this.store.updateTask(id, {
        status: 'cancelled',
        error: { code: 'cancelled', message: 'Cancelled before start.' },
      });
      return;
    }

    if (task.kind === 'agent') {
      await this.store.updateTask(id, {
        status: 'failed',
        error: {
          code: 'capability_unavailable',
          message: 'Pi runtime is not available on this backend.',
        },
      });
      await this.store.appendLog(id, 'system', 'Pi runtime not available on native');
      return;
    }

    const cwd = task.working_directory ?? '/tmp';
    const timeoutMs = Math.max(1, task.timeout_seconds) * 1000;
    // Preserve hard_max_bytes=0 (do not coerce through || fallback).
    const hardMax = Math.max(0, task.output?.hard_max_bytes ?? 10_485_760);
    const captureStdout = task.output?.capture_stdout !== false;
    const captureStderr = task.output?.capture_stderr !== false;

    const argvAttempt = await attempt<[string, ...string[]]>(async () => {
      if (task.kind === 'command') {
        return [await resolveExecutable(task.program), ...(task.arguments ?? [])];
      }
      const shell = task.shell;
      // HTTP task boundary rejects /bin/sh + load_user_environment=true.
      const loadUser = task.load_user_environment;
      const resolved = await resolveExecutable(shell);
      return [resolved, ...shellArgvFlags(shell, loadUser), task.command];
    });
    if (!argvAttempt.ok) {
      await this.recordExecFailure(id, argvAttempt.error);
      return;
    }
    const argv = argvAttempt.value;

    // No Process yet — store failure propagates without leaking a handle.
    await this.store.appendLog(id, 'system', `exec: ${argv.map(shellQuote).join(' ')}`);
    log.info(`task ${id} start kind=${task.kind}`);

    const procAttempt = await attempt(() => {
      const [command, ...args] = argv;
      return new Process(command, args, {
        cwd,
        timeoutMs,
        stdin: 'ignore',
        maxStdoutBytes: captureStdout ? hardMax : 0,
        maxStderrBytes: captureStderr ? hardMax : 0,
      });
    });
    if (!procAttempt.ok) {
      await this.recordExecFailure(id, procAttempt.error);
      return;
    }
    const proc = procAttempt.value;

    const startAttempt = await attempt(() => proc.start());
    if (!startAttempt.ok) {
      try {
        await proc.close();
      } catch {
        /* ignore */
      }
      await this.recordExecFailure(id, startAttempt.error);
      return;
    }

    this.active.set(id, proc);
    try {
      if (await this.store.isCancelRequested(id)) {
        try {
          await proc.terminate('SIGKILL');
        } catch {
          /* ignore */
        }
        let result: ProcessResult | null;
        try {
          result = await proc.wait();
        } catch {
          result = null;
        }
        const facts = streamFacts(result, captureStdout, captureStderr);
        if (captureStdout && facts.stdout) await this.store.appendLog(id, 'stdout', facts.stdout);
        if (captureStderr && facts.stderr) await this.store.appendLog(id, 'stderr', facts.stderr);
        await this.store.updateTask(id, {
          status: 'cancelled',
          error: { code: 'cancelled', message: 'Cancelled during execution.' },
          result: processResult(result?.exitCode ?? null, result?.timedOut ?? false, facts),
        });
        return;
      }

      const waitAttempt = await attempt(() => proc.wait());
      if (!waitAttempt.ok) {
        await this.recordExecFailure(id, waitAttempt.error);
        return;
      }
      const final = waitAttempt.value;

      // Cancel may have raced with wait (cancelRunning → terminate).
      if (await this.store.isCancelRequested(id)) {
        const facts = streamFacts(final, captureStdout, captureStderr);
        if (captureStdout && facts.stdout) await this.store.appendLog(id, 'stdout', facts.stdout);
        if (captureStderr && facts.stderr) await this.store.appendLog(id, 'stderr', facts.stderr);
        await this.store.updateTask(id, {
          status: 'cancelled',
          error: { code: 'cancelled', message: 'Cancelled during execution.' },
          result: processResult(final.exitCode, final.timedOut, facts),
        });
        return;
      }

      const facts = streamFacts(final, captureStdout, captureStderr);

      if (captureStdout && facts.stdout) await this.store.appendLog(id, 'stdout', facts.stdout);
      if (captureStderr && facts.stderr) await this.store.appendLog(id, 'stderr', facts.stderr);

      if (final.timedOut) {
        await this.store.updateTask(id, {
          status: 'timed_out',
          error: { code: 'timed_out', message: `Timeout after ${task.timeout_seconds}s` },
          result: processResult(final.exitCode, true, facts),
        });
        return;
      }

      if (final.exitCode === 0) {
        await this.store.updateTask(id, {
          status: 'succeeded',
          result: processResult(0, false, facts),
        });
      } else {
        await this.store.updateTask(id, {
          status: 'failed',
          error: {
            code: 'exit_nonzero',
            message: `Process exited with code ${final.exitCode}`,
          },
          result: processResult(final.exitCode, false, facts),
        });
      }
    } finally {
      this.active.delete(id);
      try {
        await proc.close();
      } catch {
        /* ignore */
      }
    }
  }

  /**
   * Record an expected process/resolve failure on the task.
   * Store failures here intentionally propagate to pumpOnce.
   */
  private async recordExecFailure(id: string, e: unknown): Promise<void> {
    const msg = errorMessage(e);
    await this.store.appendLog(id, 'system', `error: ${msg}`);
    const cur = await this.store.getTask(id);
    if (cur && !isTerminalTaskStatus(cur.status)) {
      await this.store.updateTask(id, {
        status: 'failed',
        error: { code: 'exec_error', message: msg },
      });
    }
  }
}

/** Canonical stream fields stored on the task result (no second JS truncation). */
type StreamFacts = {
  captureStdout: boolean;
  captureStderr: boolean;
  stdout: string;
  stderr: string;
  stdoutBytes: number;
  stderrBytes: number;
  stdoutTruncated: boolean;
  stderrTruncated: boolean;
};

/**
 * Map native ProcessResult (or null wait failure) into stored stream facts.
 * Strings are kept only when the corresponding capture flag is true.
 * Null/wait-failure uses zero/false byte facts.
 */
function streamFacts(
  result: ProcessResult | null,
  captureStdout: boolean,
  captureStderr: boolean,
): StreamFacts {
  if (result === null) {
    return {
      captureStdout,
      captureStderr,
      stdout: '',
      stderr: '',
      stdoutBytes: 0,
      stderrBytes: 0,
      stdoutTruncated: false,
      stderrTruncated: false,
    };
  }
  return {
    captureStdout,
    captureStderr,
    stdout: captureStdout ? result.stdout : '',
    stderr: captureStderr ? result.stderr : '',
    stdoutBytes: result.stdoutBytes,
    stderrBytes: result.stderrBytes,
    stdoutTruncated: result.stdoutTruncated,
    stderrTruncated: result.stderrTruncated,
  };
}

function processResult(
  exitCode: number | null | undefined,
  timedOut: boolean,
  facts: StreamFacts,
): Record<string, unknown> {
  const out: Record<string, unknown> = {
    exitCode: exitCode ?? null,
    timedOut,
    stdoutBytes: facts.stdoutBytes,
    stderrBytes: facts.stderrBytes,
    stdoutTruncated: facts.stdoutTruncated,
    stderrTruncated: facts.stderrTruncated,
  };
  if (facts.captureStdout) out.stdout = facts.stdout;
  if (facts.captureStderr) out.stderr = facts.stderr;
  return out;
}

function shellQuote(s: string): string {
  if (/^[A-Za-z0-9_./:=+-]+$/.test(s)) return s;
  return `'${s.replaceAll("'", `'\\''`)}'`;
}
