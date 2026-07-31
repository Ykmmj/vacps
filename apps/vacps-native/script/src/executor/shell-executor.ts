import type { TaskDispatch } from '@vacps/contracts';
import { isTerminalTaskStatus } from '@vacps/contracts';
import * as log from 'vacps:log';
import { Process } from 'vacps:process';

import type { TaskStore } from '../storage/task-store';
import { resolveExecutable } from '../util/resolve-executable';

/**
 * Run command/shell tasks via vacps:process Process class
 * (apps/vacps ShellExecutor counterpart). Supports mid-run cancel via
 * Process.terminate while Process.wait is in flight.
 */
export class ShellExecutor {
  /** task_id → Process instance (for cancel while running). */
  private readonly active = new Map<string, Process>();

  constructor(private readonly store: TaskStore) {}

  /** True when a process is still tracked for the task (no native registry id). */
  processIdForTask(taskId: string): string | undefined {
    return this.active.has(taskId) ? taskId : undefined;
  }

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
    const hardMax = Math.max(0, Number(task.output?.hard_max_bytes ?? 10_485_760) || 10_485_760);
    const captureStdout = task.output?.capture_stdout !== false;
    const captureStderr = task.output?.capture_stderr !== false;

    let argv: string[];
    if (task.kind === 'command') {
      argv = [await resolveExecutable(task.program), ...(task.arguments ?? [])];
    } else {
      const shell = await resolveExecutable(task.shell ?? '/bin/bash');
      argv = [shell, '-lc', task.command];
    }

    await this.store.appendLog(id, 'system', `exec: ${argv.map(shellQuote).join(' ')}`);
    log.info(`task ${id} start kind=${task.kind}`);

    const command = argv[0]!;
    const args = argv.slice(1);
    const proc = new Process(command, args, {
      cwd,
      timeoutMs,
      stdin: 'ignore',
      maxStdoutBytes: captureStdout ? hardMax : 0,
      maxStderrBytes: captureStderr ? hardMax : 0,
    });

    try {
      await proc.start();
      this.active.set(id, proc);

      if (await this.store.isCancelRequested(id)) {
        try {
          await proc.terminate('SIGKILL');
        } catch {
          /* ignore */
        }
        let result;
        try {
          result = await proc.wait();
        } catch {
          result = null;
        }
        const stdout = captureStdout
          ? truncate(result?.stdout ?? '', hardMax)
          : { text: '', cut: false };
        const stderr = captureStderr
          ? truncate(result?.stderr ?? '', hardMax)
          : { text: '', cut: false };
        if (captureStdout && stdout.text) await this.store.appendLog(id, 'stdout', stdout.text);
        if (captureStderr && stderr.text) await this.store.appendLog(id, 'stderr', stderr.text);
        await this.store.updateTask(id, {
          status: 'cancelled',
          error: { code: 'cancelled', message: 'Cancelled during execution.' },
          result: processResult(
            result?.exitCode ?? null,
            result?.timedOut ?? false,
            stdout.text,
            stderr.text,
            captureStdout,
            captureStderr,
            stdout.cut,
            stderr.cut,
          ),
        });
        return;
      }

      const final = await proc.wait();

      // Cancel may have raced with wait (cancelRunning → terminate).
      if (await this.store.isCancelRequested(id)) {
        const stdout = captureStdout ? truncate(final.stdout, hardMax) : { text: '', cut: false };
        const stderr = captureStderr ? truncate(final.stderr, hardMax) : { text: '', cut: false };
        if (captureStdout && stdout.text) await this.store.appendLog(id, 'stdout', stdout.text);
        if (captureStderr && stderr.text) await this.store.appendLog(id, 'stderr', stderr.text);
        await this.store.updateTask(id, {
          status: 'cancelled',
          error: { code: 'cancelled', message: 'Cancelled during execution.' },
          result: processResult(
            final.exitCode,
            final.timedOut,
            stdout.text,
            stderr.text,
            captureStdout,
            captureStderr,
            stdout.cut,
            stderr.cut,
          ),
        });
        return;
      }

      const stdout = captureStdout ? truncate(final.stdout, hardMax) : { text: '', cut: false };
      const stderr = captureStderr ? truncate(final.stderr, hardMax) : { text: '', cut: false };

      if (captureStdout && stdout.text) await this.store.appendLog(id, 'stdout', stdout.text);
      if (captureStderr && stderr.text) await this.store.appendLog(id, 'stderr', stderr.text);

      if (final.timedOut) {
        await this.store.updateTask(id, {
          status: 'timed_out',
          error: { code: 'timed_out', message: `Timeout after ${task.timeout_seconds}s` },
          result: processResult(
            final.exitCode,
            true,
            stdout.text,
            stderr.text,
            captureStdout,
            captureStderr,
            stdout.cut,
            stderr.cut,
          ),
        });
        return;
      }

      if (final.exitCode === 0) {
        await this.store.updateTask(id, {
          status: 'succeeded',
          result: processResult(
            0,
            false,
            stdout.text,
            stderr.text,
            captureStdout,
            captureStderr,
            stdout.cut,
            stderr.cut,
          ),
        });
      } else {
        await this.store.updateTask(id, {
          status: 'failed',
          error: {
            code: 'exit_nonzero',
            message: `Process exited with code ${final.exitCode}`,
          },
          result: processResult(
            final.exitCode,
            false,
            stdout.text,
            stderr.text,
            captureStdout,
            captureStderr,
            stdout.cut,
            stderr.cut,
          ),
        });
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      await this.store.appendLog(id, 'system', `error: ${msg}`);
      const cur = await this.store.getTask(id);
      if (cur && !isTerminalTaskStatus(cur.status)) {
        await this.store.updateTask(id, {
          status: 'failed',
          error: { code: 'exec_error', message: msg },
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
}

function truncate(s: string, max: number): { text: string; cut: boolean } {
  if (max <= 0) return { text: '', cut: false };
  if (s.length <= max) return { text: s, cut: false };
  return { text: s.slice(0, max), cut: true };
}

function processResult(
  exitCode: number | null | undefined,
  timedOut: boolean,
  stdout: string,
  stderr: string,
  captureStdout: boolean,
  captureStderr: boolean,
  stdoutTruncated: boolean,
  stderrTruncated: boolean,
): Record<string, unknown> {
  const out: Record<string, unknown> = {
    exitCode: exitCode ?? null,
    timedOut,
  };
  if (captureStdout) {
    out.stdout = stdout;
    if (stdoutTruncated) out.stdout_truncated = true;
  }
  if (captureStderr) {
    out.stderr = stderr;
    if (stderrTruncated) out.stderr_truncated = true;
  }
  return out;
}

function shellQuote(s: string): string {
  if (/^[A-Za-z0-9_./:=+-]+$/.test(s)) return s;
  return `'${s.replaceAll("'", `'\\''`)}'`;
}
