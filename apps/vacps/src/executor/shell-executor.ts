import { createWriteStream } from 'node:fs';
import { mkdir } from 'node:fs/promises';
import { dirname } from 'node:path';
import { spawn } from 'node:child_process';

export type ShellExecutionInput = {
  cwd: string;
  environment?: NodeJS.ProcessEnv;
  timeoutSeconds: number;
  stdoutPath: string;
  stderrPath: string;
  signal?: AbortSignal;
  /** In-memory capture cap; full output still streams to log files. */
  hardMaxBytes?: number;
} & (
  | {
      /** Schema v3 kind=command: argv spawn without shell. */
      program: string;
      arguments?: string[];
      command?: never;
      shell?: never;
      loadUserEnvironment?: never;
    }
  | {
      /** Schema v3 kind=shell (or agent tool steps): shell -lc/-c. */
      command: string;
      shell?: string;
      loadUserEnvironment?: boolean;
      program?: never;
      arguments?: never;
    }
);

export interface ShellExecutionResult {
  status: 'succeeded' | 'failed' | 'cancelled' | 'timed_out';
  exitCode: number | null;
  stdout: string;
  stderr: string;
}

const DEFAULT_MAX_CAPTURE_BYTES = 1024 * 1024;

export class ShellExecutor {
  async execute(input: ShellExecutionInput): Promise<ShellExecutionResult> {
    await Promise.all([
      mkdir(dirname(input.stdoutPath), { recursive: true }),
      mkdir(dirname(input.stderrPath), { recursive: true }),
    ]);
    const stdoutFile = createWriteStream(input.stdoutPath, { flags: 'a' });
    const stderrFile = createWriteStream(input.stderrPath, { flags: 'a' });

    const child =
      typeof input.program === 'string'
        ? spawn(input.program, input.arguments ?? [], {
            cwd: input.cwd,
            env: { ...process.env, ...input.environment },
            detached: true,
            stdio: ['ignore', 'pipe', 'pipe'],
          })
        : spawnShell(input);

    let stdout = '';
    let stderr = '';
    let timedOut = false;
    let cancelled = false;
    const maxCapture = input.hardMaxBytes ?? DEFAULT_MAX_CAPTURE_BYTES;
    const append = (current: string, chunk: Buffer): string => {
      const remaining = maxCapture - Buffer.byteLength(current);
      return remaining > 0 ? current + chunk.subarray(0, remaining).toString('utf8') : current;
    };
    const terminate = (reason: 'cancelled' | 'timed_out') => {
      cancelled ||= reason === 'cancelled';
      timedOut ||= reason === 'timed_out';
      if (child.pid) {
        try {
          process.kill(-child.pid, 'SIGTERM');
        } catch (error: unknown) {
          if ((error as NodeJS.ErrnoException).code !== 'ESRCH') throw error;
        }
      }
    };
    const timer = setTimeout(() => terminate('timed_out'), input.timeoutSeconds * 1000);
    input.signal?.addEventListener('abort', () => terminate('cancelled'), { once: true });
    child.stdout?.on('data', (chunk: Buffer) => {
      stdoutFile.write(chunk);
      stdout = append(stdout, chunk);
    });
    child.stderr?.on('data', (chunk: Buffer) => {
      stderrFile.write(chunk);
      stderr = append(stderr, chunk);
    });

    const exitCode = await new Promise<number | null>((resolve, reject) => {
      child.once('error', reject);
      child.once('close', (code) => resolve(code));
    }).finally(() => clearTimeout(timer));
    await Promise.all([
      new Promise<void>((resolve) => stdoutFile.end(resolve)),
      new Promise<void>((resolve) => stderrFile.end(resolve)),
    ]);

    return {
      status: cancelled
        ? 'cancelled'
        : timedOut
          ? 'timed_out'
          : exitCode === 0
            ? 'succeeded'
            : 'failed',
      exitCode,
      stdout,
      stderr,
    };
  }
}

function spawnShell(
  input: Extract<ShellExecutionInput, { command: string }>,
): ReturnType<typeof spawn> {
  const shell = input.shell === '/bin/sh' ? '/bin/sh' : '/bin/bash';
  const loadUser = shell === '/bin/sh' ? false : input.loadUserEnvironment !== false;
  // bash -lc loads login + rc; bash -c / sh -c do not.
  const shellArgs = shell === '/bin/bash' ? (loadUser ? ['-lc'] : ['-c']) : ['-c'];
  return spawn(shell, [...shellArgs, input.command], {
    cwd: input.cwd,
    env: { ...process.env, ...input.environment },
    detached: true,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
}
