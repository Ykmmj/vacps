/** Native vacps:process per-stream capture maximum (64 MiB). */
export const NATIVE_STREAM_MAX_BYTES = 64 * 1024 * 1024;

/** Shell binary path accepted by Narrow shell exec helpers. */
export type ShellPath = '/bin/bash' | '/bin/sh';

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
