import * as host from 'vacps:host';
import {
  Process,
  type ProcessSnapshot as NativeProcessSnapshot,
  type ProcessStatus,
} from 'vacps:process';

import { requireAbsolutePath } from '../util/absolute-path';
import { resolveExecutable } from '../util/resolve-executable';
import { NATIVE_STREAM_MAX_BYTES, shellArgvFlags, type ShellPath } from './process-exec';

export { NATIVE_STREAM_MAX_BYTES };

export type CommandInput =
  | {
      kind: 'command';
      program: string;
      arguments?: readonly string[];
      workingDirectory?: string;
      timeoutMs: number;
      stdoutHardMaxBytes: number;
      stderrHardMaxBytes: number;
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
    };

export interface PreviewLimits {
  stdoutMaxBytes: number;
  stderrMaxBytes: number;
}

export interface CommandResult {
  backend_id: string;
  status: ProcessStatus;
  exit_code: number | null;
  signal: string | null;
  timed_out: boolean;
  started_at: string;
  finished_at: string;
  duration_ms: number;
  stdout: OutputDescriptor;
  stderr: OutputDescriptor;
}

interface OutputDescriptor {
  preview: string;
  total_bytes: number;
  truncated: boolean;
}

/**
 * Owns only in-flight one-shot command resources.
 *
 * `run` does not publish a native Process handle: it waits for reap and pipe
 * drain, snapshots the final result, and closes the Process before returning.
 * Long-lived work belongs to TaskQueue; interactive work belongs to Terminal.
 */
export class CommandRunner {
  private readonly active = new Set<Process>();

  constructor(private readonly backendId: string) {}

  async run(input: CommandInput, preview: PreviewLimits): Promise<CommandResult> {
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
      stdin: 'ignore',
      timeoutMs: input.timeoutMs,
      maxStdoutBytes: input.stdoutHardMaxBytes,
      maxStderrBytes: input.stderrHardMaxBytes,
    });
    const startedAtMs = host.nowMs();
    this.active.add(process);
    try {
      await process.start();
      await process.waitForExit();
      const finishedAtMs = host.nowMs();
      const snapshot = process.snapshot({
        stdoutMaxBytes: preview.stdoutMaxBytes,
        stderrMaxBytes: preview.stderrMaxBytes,
      });
      return this.result(snapshot, startedAtMs, finishedAtMs);
    } finally {
      this.active.delete(process);
      await process.close();
    }
  }

  async close(): Promise<void> {
    const active = [...this.active];
    this.active.clear();
    await Promise.all(active.map(async (process) => await process.close()));
  }

  private result(
    snapshot: NativeProcessSnapshot,
    startedAtMs: number,
    finishedAtMs: number,
  ): CommandResult {
    return {
      backend_id: this.backendId,
      status: snapshot.status,
      exit_code: snapshot.exitCode,
      signal: snapshot.signal,
      timed_out: snapshot.timedOut,
      started_at: new Date(startedAtMs).toISOString(),
      finished_at: new Date(finishedAtMs).toISOString(),
      duration_ms: finishedAtMs - startedAtMs,
      stdout: describeOutput(snapshot, 'stdout'),
      stderr: describeOutput(snapshot, 'stderr'),
    };
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
