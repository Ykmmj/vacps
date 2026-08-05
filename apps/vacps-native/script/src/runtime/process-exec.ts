import * as host from 'vacps:host';
import * as log from 'vacps:log';
import { run } from 'vacps:process';

import { requireAbsolutePath } from '../util/absolute-path';
import { resolveExecutable } from '../util/resolve-executable';
import { utf8ByteSlice } from '../util/utf8';
import { randomUuidV4 } from '../util/uuid';

/** Native vacps:process per-stream capture maximum (64 MiB). */
export const NATIVE_STREAM_MAX_BYTES = 64 * 1024 * 1024;

/**
 * Fixed internal one-shot capture per stream (16 MiB).
 * Below NATIVE_STREAM_MAX_BYTES; not part of the shared /exec protocol.
 */
const ONE_SHOT_CAPTURE_MAX_BYTES = 16 * 1024 * 1024;

export type ProcessStatus = 'exited' | 'timed_out';

/**
 * Completed one-shot exec snapshot.
 * process_id is protocol-required and synthetic only — not queryable;
 * interactive_process is unsupported (capabilities.features.interactive_process = false).
 */
export interface ProcessSnapshot {
  process_id: string;
  backend_id: string;
  status: ProcessStatus;
  exit_code: number | null;
  signal: string | null;
  timed_out: boolean;
  started_at: string;
  finished_at: string;
  duration_ms: number;
  stdin_available: false;
  tty: false;
  output_cursor: null;
  stdout: { preview: string; total_bytes: number; truncated: boolean };
  stderr: { preview: string; total_bytes: number; truncated: boolean };
}

/** Shell binary path accepted by Narrow shell exec helpers. */
export type ShellPath = '/bin/bash' | '/bin/sh';

/** Shared already-normalized options for one-shot exec (HTTP Wide boundary). */
export type ExecSharedOptions = {
  workingDirectory?: string;
  /** Already normalized: integer 1..3600000. */
  timeoutMs: number;
  /** Response preview budget (not the native capture hard cap). */
  stdoutMaxBytes: number;
  stderrMaxBytes: number;
};

/**
 * Exactly one exec mode, encoded in the discriminant.
 * command: required program, optional readonly arguments
 * shell: required command, shell /bin/bash|/bin/sh, required loadUserEnvironment
 */
export type ExecInput =
  | ({
      kind: 'command';
      program: string;
      arguments?: readonly string[];
    } & ExecSharedOptions)
  | ({
      kind: 'shell';
      command: string;
      shell: ShellPath;
      loadUserEnvironment: boolean;
    } & ExecSharedOptions);

/** Nonempty argv tuple after resolveExecutable (program is always present). */
type ResolvedArgv = [string, ...string[]];

/**
 * Build a protocol preview from retained capture text + exact native byte facts.
 * total_bytes is the native drained count; truncated is native OR preview clipping.
 */
function previewStream(
  data: string,
  maxPreviewBytes: number,
  totalBytes: number,
  nativeTruncated: boolean,
): {
  preview: string;
  total_bytes: number;
  truncated: boolean;
} {
  if (maxPreviewBytes <= 0) {
    return {
      preview: '',
      total_bytes: totalBytes,
      truncated: nativeTruncated || totalBytes > 0,
    };
  }
  const slice = utf8ByteSlice(data, 0, maxPreviewBytes);
  const previewClipped = slice.end < slice.totalBytes;
  return {
    preview: slice.content,
    total_bytes: totalBytes,
    truncated: nativeTruncated || previewClipped,
  };
}

/**
 * Shell argv flags (without the command string).
 * - /bin/bash + loadUserEnvironment true → -lc
 * - /bin/bash + false → --noprofile --norc -c
 * - /bin/sh + false → -c
 *
 * Contract: Narrow
 * Preconditions: shell is '/bin/bash' | '/bin/sh'; shell === '/bin/sh' implies
 *   loadUserEnvironment === false (caller established at HTTP trust boundary).
 * Errors: none (pure argv construction)
 * Threading: any
 * Lifetime: returned array is owned by caller
 */
export function shellArgvFlags(shell: ShellPath, loadUserEnvironment: boolean): string[] {
  if (shell === '/bin/sh') {
    return ['-c'];
  }
  return loadUserEnvironment ? ['-lc'] : ['--noprofile', '--norc', '-c'];
}

/**
 * Stateless one-shot process execution via vacps:process run().
 * Does not retain Process handles or maps. Owns start/wait/close inside the native coroutine.
 *
 * Contract: Narrow
 * Preconditions: input came from the HTTP Wide boundary; numeric options are
 *   already normalized/within declared limits; shell /bin/sh implies
 *   loadUserEnvironment=false; working directory was boundary-shaped and
 *   requireAbsolutePath may still report its operational/validation result.
 * Errors: expected resolve/run failures reject.
 * Threading/Lifetime: owner JS turn and vacps:process run owns process lifetime.
 */
export async function execOneShot(backendId: string, input: ExecInput): Promise<ProcessSnapshot> {
  const cwd = input.workingDirectory ? requireAbsolutePath(input.workingDirectory) : '/tmp';

  let argv: ResolvedArgv;
  if (input.kind === 'shell') {
    const resolved = await resolveExecutable(input.shell);
    argv = [resolved, ...shellArgvFlags(input.shell, input.loadUserEnvironment), input.command];
  } else {
    argv = [await resolveExecutable(input.program), ...(input.arguments ?? [])];
  }

  const [command, ...args] = argv;
  log.info(`process-exec run ${command} timeoutMs=${input.timeoutMs}`);
  const startedMs = host.nowMs();
  const result = await run(command, args, {
    cwd,
    timeoutMs: input.timeoutMs,
    maxStdoutBytes: ONE_SHOT_CAPTURE_MAX_BYTES,
    maxStderrBytes: ONE_SHOT_CAPTURE_MAX_BYTES,
  });
  const finishedMs = host.nowMs();
  log.info(`process-exec done exit=${result.exitCode} timedOut=${result.timedOut}`);

  const timedOut = result.timedOut === true;
  return {
    process_id: randomUuidV4(),
    backend_id: backendId,
    status: timedOut ? 'timed_out' : 'exited',
    exit_code: timedOut ? null : result.exitCode,
    signal: timedOut ? 'SIGKILL' : null,
    timed_out: timedOut,
    started_at: new Date(startedMs).toISOString(),
    finished_at: new Date(finishedMs).toISOString(),
    duration_ms: finishedMs - startedMs,
    stdin_available: false,
    tty: false,
    output_cursor: null,
    stdout: previewStream(
      result.stdout,
      input.stdoutMaxBytes,
      result.stdoutBytes,
      result.stdoutTruncated,
    ),
    stderr: previewStream(
      result.stderr,
      input.stderrMaxBytes,
      result.stderrBytes,
      result.stderrTruncated,
    ),
  };
}
