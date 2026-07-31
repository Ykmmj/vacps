import type { TaskDispatch } from '@vacps/contracts';
import { isTerminalTaskStatus } from '@vacps/contracts';
import * as log from 'vacps:log';
import * as process from 'vacps:process';

import type { TaskStore } from '../storage/task-store';
import { resolveExecutable } from '../util/resolve-executable';

/**
 * Run command/shell tasks via vacps:process start/read/terminate
 * (apps/vacps ShellExecutor counterpart). Supports mid-run cancel.
 */
export class ShellExecutor {
  /** task_id → process id (for cancel while running). */
  private readonly active = new Map<string, string>();

  constructor(private readonly store: TaskStore) {}

  processIdForTask(taskId: string): string | undefined {
    return this.active.get(taskId);
  }

  async cancelRunning(taskId: string): Promise<boolean> {
    const procId = this.active.get(taskId);
    if (!procId) return false;
    try {
      await process.terminate(procId, { signal: 'SIGKILL', graceMs: 0 });
      return true;
    } catch {
      return false;
    }
  }

  async execute(task: TaskDispatch): Promise<void> {
    const id = task.task_id;
    if (this.store.isCancelRequested(id)) {
      this.store.updateTask(id, {
        status: 'cancelled',
        error: { code: 'cancelled', message: 'Cancelled before start.' },
      });
      return;
    }

    if (task.kind === 'agent') {
      this.store.updateTask(id, {
        status: 'failed',
        error: {
          code: 'capability_unavailable',
          message: 'Pi runtime is not available on this backend.',
        },
      });
      this.store.appendLog(id, 'system', 'Pi runtime not available on native');
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

    this.store.appendLog(id, 'system', `exec: ${argv.map(shellQuote).join(' ')}`);
    log.info(`task ${id} start kind=${task.kind}`);

    try {
      const started = await process.start(argv, {
        cwd,
        timeoutMs,
        closeStdin: true,
        hardMaxStdout: captureStdout ? hardMax : 0,
        hardMaxStderr: captureStderr ? hardMax : 0,
      });
      this.active.set(id, started.id);

      let stdoutOff = 0;
      let stderrOff = 0;
      let stdout = '';
      let stderr = '';
      let stdoutTruncated = false;
      let stderrTruncated = false;
      let final = await process.read(started.id, {
        waitMs: 0,
        maxBytes: 256_000,
        stdoutOffset: 0,
        stderrOffset: 0,
      });

      const appendCap = (
        acc: string,
        chunk: string,
        max: number,
      ): { text: string; cut: boolean } => {
        if (!captureOrStore(max)) return { text: acc, cut: false };
        if (acc.length >= max) return { text: acc, cut: true };
        const room = max - acc.length;
        if (chunk.length <= room) return { text: acc + chunk, cut: false };
        return { text: acc + chunk.slice(0, room), cut: true };
      };
      const captureOrStore = (max: number) => max > 0;

      for (;;) {
        if (this.store.isCancelRequested(id)) {
          await process.terminate(started.id, { signal: 'SIGKILL', graceMs: 0 });
          final = await process.read(started.id, {
            waitMs: 2_000,
            maxBytes: 256_000,
            stdoutOffset: stdoutOff,
            stderrOffset: stderrOff,
          });
          if (captureStdout && final.stdout) {
            const r = appendCap(stdout, final.stdout, hardMax);
            stdout = r.text;
            stdoutTruncated = stdoutTruncated || r.cut;
          }
          if (captureStderr && final.stderr) {
            const r = appendCap(stderr, final.stderr, hardMax);
            stderr = r.text;
            stderrTruncated = stderrTruncated || r.cut;
          }
          if (captureStdout && stdout) this.store.appendLog(id, 'stdout', stdout);
          if (captureStderr && stderr) this.store.appendLog(id, 'stderr', stderr);
          this.store.updateTask(id, {
            status: 'cancelled',
            error: { code: 'cancelled', message: 'Cancelled during execution.' },
            result: processResult(
              final.exitCode,
              final.timedOut,
              stdout,
              stderr,
              captureStdout,
              captureStderr,
              stdoutTruncated,
              stderrTruncated,
            ),
          });
          return;
        }

        final = await process.read(started.id, {
          waitMs: 200,
          maxBytes: 256_000,
          stdoutOffset: stdoutOff,
          stderrOffset: stderrOff,
        });
        if (final.stdout) {
          stdoutOff = final.nextStdoutOffset;
          if (captureStdout) {
            const r = appendCap(stdout, final.stdout, hardMax);
            stdout = r.text;
            stdoutTruncated = stdoutTruncated || r.cut;
          }
        }
        if (final.stderr) {
          stderrOff = final.nextStderrOffset;
          if (captureStderr) {
            const r = appendCap(stderr, final.stderr, hardMax);
            stderr = r.text;
            stderrTruncated = stderrTruncated || r.cut;
          }
        }

        // Wait for full completion: process exit + both stream EOFs (eof flag).
        // Do not stop solely on status — tail bytes may still be draining.
        if (final.eof) break;
      }

      if (captureStdout && stdout) this.store.appendLog(id, 'stdout', stdout);
      if (captureStderr && stderr) this.store.appendLog(id, 'stderr', stderr);

      if (final.timedOut || final.status === 'timed_out') {
        this.store.updateTask(id, {
          status: 'timed_out',
          error: { code: 'timed_out', message: `Timeout after ${task.timeout_seconds}s` },
          result: processResult(
            final.exitCode,
            true,
            stdout,
            stderr,
            captureStdout,
            captureStderr,
            stdoutTruncated,
            stderrTruncated,
          ),
        });
        return;
      }

      if (final.status === 'cancelled') {
        this.store.updateTask(id, {
          status: 'cancelled',
          error: { code: 'cancelled', message: 'Process terminated.' },
          result: processResult(
            final.exitCode,
            false,
            stdout,
            stderr,
            captureStdout,
            captureStderr,
            stdoutTruncated,
            stderrTruncated,
          ),
        });
        return;
      }

      if (final.exitCode === 0) {
        this.store.updateTask(id, {
          status: 'succeeded',
          result: processResult(
            0,
            false,
            stdout,
            stderr,
            captureStdout,
            captureStderr,
            stdoutTruncated,
            stderrTruncated,
          ),
        });
      } else {
        this.store.updateTask(id, {
          status: 'failed',
          error: {
            code: 'exit_nonzero',
            message: `Process exited with code ${final.exitCode}`,
          },
          result: processResult(
            final.exitCode,
            false,
            stdout,
            stderr,
            captureStdout,
            captureStderr,
            stdoutTruncated,
            stderrTruncated,
          ),
        });
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      this.store.appendLog(id, 'system', `error: ${msg}`);
      const cur = this.store.getTask(id);
      if (cur && !isTerminalTaskStatus(cur.status)) {
        this.store.updateTask(id, {
          status: 'failed',
          error: { code: 'exec_error', message: msg },
        });
      }
    } finally {
      this.active.delete(id);
    }
  }
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
