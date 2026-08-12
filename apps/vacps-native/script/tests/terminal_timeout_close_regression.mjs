/**
 * Native QuickJS regression for overlapping terminal hard-timeout and close.
 *
 * The child ignores close's initial SIGHUP. While close is waiting through its
 * grace period, the independent hard-timeout owns the SIGKILL. Exit fields
 * describe how the process ended; close fields describe only actions performed
 * by this close operation.
 */
import * as log from 'vacps:log';
import { Terminal } from 'vacps:terminal';

function assertEqual(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(
      `${label}: got ${JSON.stringify(actual)}, expected ${JSON.stringify(expected)}`,
    );
  }
}

const terminal = new Terminal(
  '/bin/sh',
  ['-c', `trap '' HUP; printf 'terminal-timeout-ready\\n'; while :; do sleep 1; done`],
  { timeoutMs: 1_500 },
);

let closed = false;
try {
  await terminal.start();
  const ready = await terminal.read({ offset: 0, maxBytes: 4096, waitMs: 500 });
  const output = new TextDecoder().decode(ready.data);
  if (!output.includes('terminal-timeout-ready')) {
    throw new Error(`child did not become ready before timeout: ${JSON.stringify(output)}`);
  }
  assertEqual(terminal.snapshot().status, 'running', 'process state before close');

  // close starts first, but its grace deadline is later than the hard timeout.
  const result = await terminal.close(5_000);
  closed = true;
  assertEqual(result.status, 'timed_out', 'process status');
  assertEqual(result.signal, 'SIGKILL', 'process termination signal');
  assertEqual(result.timedOut, true, 'hard timeout ownership');
  assertEqual(result.escalated, false, 'close escalation ownership');
  assertEqual(result.finalSignal, null, 'close final signal');
  log.info(
    `[pass] terminal timeout wins while close awaits grace ${JSON.stringify({
      status: result.status,
      signal: result.signal,
      timedOut: result.timedOut,
      escalated: result.escalated,
      finalSignal: result.finalSignal,
    })}`,
  );
} finally {
  if (!closed) await terminal.close(0);
}

export default true;
export async function initialize() {}
export async function shutdown() {}
