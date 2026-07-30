import type { TaskDispatch } from '@vacps/contracts';
import { isTerminalTaskStatus } from '@vacps/contracts';
import * as log from 'vacps:log';
import * as process from 'vacps:process';

import type { TaskStore } from '../storage/task-store';

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

    let argv: string[];
    if (task.kind === 'command') {
      argv = [task.program, ...(task.arguments ?? [])];
    } else {
      const shell = task.shell ?? '/bin/bash';
      argv = [shell, '-lc', task.command];
    }

    this.store.appendLog(id, 'system', `exec: ${argv.map(shellQuote).join(' ')}`);
    log.info(`task ${id} start kind=${task.kind}`);

    try {
      const started = await process.start(argv, {
        cwd,
        timeoutMs,
        closeStdin: true,
      });
      this.active.set(id, started.id);

      let stdoutOff = 0;
      let stderrOff = 0;
      let stdout = '';
      let stderr = '';
      let final = await process.read(started.id, {
        waitMs: 0,
        maxBytes: 256_000,
        stdoutOffset: 0,
        stderrOffset: 0,
      });

      for (;;) {
        if (this.store.isCancelRequested(id)) {
          await process.terminate(started.id, { signal: 'SIGKILL', graceMs: 0 });
          // drain final status
          final = await process.read(started.id, {
            waitMs: 2_000,
            maxBytes: 256_000,
            stdoutOffset: stdoutOff,
            stderrOffset: stderrOff,
          });
          if (final.stdout) stdout += final.stdout;
          if (final.stderr) stderr += final.stderr;
          if (stdout) this.store.appendLog(id, 'stdout', truncate(stdout, 256_000));
          if (stderr) this.store.appendLog(id, 'stderr', truncate(stderr, 256_000));
          this.store.updateTask(id, {
            status: 'cancelled',
            error: { code: 'cancelled', message: 'Cancelled during execution.' },
            result: {
              exitCode: final.exitCode,
              timedOut: final.timedOut,
              stdout: truncate(stdout, 65_536),
              stderr: truncate(stderr, 65_536),
            },
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
          stdout += final.stdout;
          stdoutOff = final.nextStdoutOffset;
        }
        if (final.stderr) {
          stderr += final.stderr;
          stderrOff = final.nextStderrOffset;
        }

        if (final.eof || final.status !== 'running') break;
      }

      if (stdout) this.store.appendLog(id, 'stdout', truncate(stdout, 256_000));
      if (stderr) this.store.appendLog(id, 'stderr', truncate(stderr, 256_000));

      if (final.timedOut || final.status === 'timed_out') {
        this.store.updateTask(id, {
          status: 'timed_out',
          error: { code: 'timed_out', message: `Timeout after ${task.timeout_seconds}s` },
          result: { exitCode: final.exitCode, timedOut: true },
        });
        return;
      }

      if (final.status === 'cancelled') {
        this.store.updateTask(id, {
          status: 'cancelled',
          error: { code: 'cancelled', message: 'Process terminated.' },
          result: {
            exitCode: final.exitCode,
            timedOut: false,
            stdout: truncate(stdout, 65_536),
            stderr: truncate(stderr, 65_536),
          },
        });
        return;
      }

      if (final.exitCode === 0) {
        this.store.updateTask(id, {
          status: 'succeeded',
          result: {
            exitCode: 0,
            stdout: truncate(stdout, 65_536),
            stderr: truncate(stderr, 65_536),
          },
        });
      } else {
        this.store.updateTask(id, {
          status: 'failed',
          error: {
            code: 'exit_nonzero',
            message: `Process exited with code ${final.exitCode}`,
          },
          result: {
            exitCode: final.exitCode,
            stdout: truncate(stdout, 65_536),
            stderr: truncate(stderr, 65_536),
          },
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

function truncate(s: string, max: number): string {
  if (s.length <= max) return s;
  return s.slice(0, max) + `\n…[truncated ${s.length - max} bytes]`;
}

function shellQuote(s: string): string {
  if (/^[A-Za-z0-9_./:=+-]+$/.test(s)) return s;
  return `'${s.replaceAll("'", `'\\''`)}'`;
}
