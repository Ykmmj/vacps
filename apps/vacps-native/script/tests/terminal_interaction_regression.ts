/** Real-program PTY regression suite. Bundled for the native QuickJS runtime. */
import { File, exists, remove } from 'vacps:fs';
import * as log from 'vacps:log';
import { sleep } from 'vacps:timer';

import {
  TerminalSessions,
  type OpenTerminalInput,
  type TerminalView,
} from '../src/runtime/terminal-sessions';
import type { TerminalKeyEvent } from '../src/runtime/terminal-key-encoder';

const sessions = new TerminalSessions('terminal-regression');
const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });
let passed = 0;

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

function field<T>(object: Record<string, unknown>, name: string): T {
  return object[name] as T;
}

async function test(name: string, body: () => Promise<void>): Promise<void> {
  try {
    await body();
    passed += 1;
    log.info(`[pass] terminal ${name}`);
  } catch (error) {
    log.error(`[fail] terminal ${name}: ${error instanceof Error ? error.message : String(error)}`);
    throw error;
  }
}

function commandInput(
  program: string,
  args: readonly string[],
  overrides: Partial<Extract<OpenTerminalInput, { kind: 'command' }>> = {},
): Extract<OpenTerminalInput, { kind: 'command' }> {
  return {
    kind: 'command',
    program,
    arguments: args,
    workingDirectory: '/tmp',
    environment: { TERM: 'xterm-256color', LANG: 'C.UTF-8' },
    columns: 80,
    rows: 24,
    timeoutMs: 20_000,
    idleTimeoutMs: 30_000,
    maxBufferBytes: 4 * 1024 * 1024,
    ...overrides,
  };
}

async function openCommand(
  program: string,
  args: readonly string[],
  overrides: Partial<Extract<OpenTerminalInput, { kind: 'command' }>> = {},
): Promise<TerminalView> {
  return await sessions.open(commandInput(program, args, overrides));
}

async function expectText(
  terminalId: string,
  pattern: string,
  cursor: string | undefined = undefined,
  timeoutMs = 5_000,
): Promise<Record<string, unknown>> {
  const result = await sessions.expect(terminalId, {
    ...(cursor === undefined ? {} : { cursor }),
    pattern,
    mode: 'literal',
    regexFlags: '',
    timeoutMs,
  });
  assert(field<boolean>(result, 'matched'), `expect did not match ${JSON.stringify(pattern)}`);
  return result;
}

async function waitForProcess(
  terminalId: string,
  predicate: (view: TerminalView) => boolean,
  label: string,
  timeoutMs = 5_000,
): Promise<TerminalView> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const view = sessions.get(terminalId);
    if (predicate(view)) return view;
    if (Date.now() >= deadline) throw new Error(`timed out waiting for ${label}`);
    await sleep(20);
  }
}

async function waitForScreen(
  terminalId: string,
  predicate: (lines: readonly string[], generation: number) => boolean,
  label: string,
  timeoutMs = 5_000,
): Promise<Record<string, unknown>> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const screen = await sessions.screen(terminalId);
    const lines = field<readonly string[]>(screen, 'lines');
    const generation = field<number>(screen, 'generation');
    if (predicate(lines, generation)) return screen;
    if (Date.now() >= deadline) {
      throw new Error(`timed out waiting for ${label}; screen=${JSON.stringify(lines)}`);
    }
    await sleep(20);
  }
}

async function waitForEof(terminalId: string, timeoutMs = 5_000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let cursor = sessions.get(terminalId).next_cursor;
  for (;;) {
    const result = await sessions.read(terminalId, cursor, 4, 100);
    cursor = field<string>(result, 'next_cursor');
    if (field<boolean>(result, 'eof')) return;
    if (Date.now() >= deadline) throw new Error('timed out waiting for PTY EOF');
  }
}

function textKeys(text: string): TerminalKeyEvent[] {
  return [...text].map((key) => ({ key }));
}

async function sendText(terminalId: string, text: string): Promise<void> {
  await sessions.sendKeys(terminalId, textKeys(text));
}

async function closeIfPresent(terminalId: string | undefined): Promise<void> {
  if (terminalId === undefined) return;
  try {
    await sessions.closeTerminal(terminalId, 100);
  } catch {
    // The test body may already have closed and removed it.
  }
}

async function writeFile(path: string, content: string): Promise<void> {
  const file = await File.open(path, { mode: 'write' });
  try {
    await file.write(encoder.encode(content));
  } finally {
    await file.close();
  }
}

async function readFile(path: string): Promise<string> {
  const file = await File.open(path, { mode: 'read' });
  try {
    return decoder.decode(await file.read(1024 * 1024));
  } finally {
    await file.close();
  }
}

await test('vim screen and editing', async () => {
  const path = '/tmp/vacps-terminal-vim-regression.txt';
  if (await exists(path)) await remove(path);
  let terminalId: string | undefined;
  try {
    const opened = await openCommand('/usr/bin/vim', [
      '-Nu',
      'NONE',
      '-n',
      '-i',
      'NONE',
      '-N',
      path,
    ]);
    terminalId = opened.terminal_id;
    await waitForScreen(
      terminalId,
      (lines) => lines.some((line) => line.includes('[New]')),
      'Vim initial screen',
    );

    await sessions.sendKeys(terminalId, [{ key: 'i' }]);
    await sendText(terminalId, 'alpha');
    await sessions.sendKeys(terminalId, [{ key: 'ENTER' }]);
    await sendText(terminalId, 'beta');
    await sessions.sendKeys(terminalId, [{ key: 'ESC' }, { key: 'UP' }, { key: 'END' }]);
    await sessions.sendKeys(terminalId, [{ key: 'A' }]);
    await sendText(terminalId, '!');
    await sessions.sendKeys(terminalId, [{ key: 'ESC' }]);

    await waitForScreen(
      terminalId,
      (lines) =>
        lines.some((line) => line.includes('alpha!')) &&
        lines.some((line) => line.includes('beta')),
      'Vim edited screen',
    );
    await sendText(terminalId, ':wq');
    await sessions.sendKeys(terminalId, [{ key: 'ENTER' }]);
    const exited = await waitForProcess(
      terminalId,
      (view) => view.process_state !== 'running',
      'Vim exit',
    );
    assert(exited.process_state === 'exited' && exited.exit_code === 0, 'Vim did not exit 0');
    assert((await readFile(path)) === 'alpha!\nbeta\n', 'Vim saved unexpected file content');
  } finally {
    await closeIfPresent(terminalId);
    if (await exists(path)) await remove(path);
  }
});

await test('less paging and search', async () => {
  const path = '/tmp/vacps-terminal-less-regression.txt';
  const content = Array.from({ length: 220 }, (_, index) => {
    const number = String(index + 1).padStart(4, '0');
    return `line-${number}${index === 172 ? '-TARGET-173' : ''}`;
  }).join('\n');
  await writeFile(path, `${content}\n`);
  let terminalId: string | undefined;
  try {
    const opened = await openCommand('/usr/bin/less', ['-N', path]);
    terminalId = opened.terminal_id;
    const initial = await waitForScreen(
      terminalId,
      (lines) => lines.some((line) => line.includes('line-0001')),
      'less first page',
    );
    const initialGeneration = field<number>(initial, 'generation');

    await sessions.sendKeys(terminalId, [{ key: 'PAGE_DOWN' }]);
    await waitForScreen(
      terminalId,
      (lines, generation) =>
        generation > initialGeneration && lines.some((line) => /line-00[2-9][0-9]/.test(line)),
      'less next page',
    );
    await sessions.sendKeys(terminalId, [{ key: 'PAGE_UP' }]);
    await waitForScreen(
      terminalId,
      (lines) => lines.some((line) => line.includes('line-0001')),
      'less previous page',
    );

    await sendText(terminalId, '/TARGET-173');
    await sessions.sendKeys(terminalId, [{ key: 'ENTER' }]);
    await waitForScreen(
      terminalId,
      (lines) => lines.some((line) => line.includes('TARGET-173')),
      'less search result',
    );
    await sessions.sendKeys(terminalId, [{ key: 'q' }]);
    const exited = await waitForProcess(
      terminalId,
      (view) => view.process_state !== 'running',
      'less exit',
    );
    assert(exited.process_state === 'exited' && exited.exit_code === 0, 'less did not exit 0');
  } finally {
    await closeIfPresent(terminalId);
    if (await exists(path)) await remove(path);
  }
});

await test('Python REPL expect and Ctrl-D', async () => {
  let terminalId: string | undefined;
  try {
    const opened = await openCommand('/usr/bin/python3', ['-q']);
    terminalId = opened.terminal_id;
    const prompt = await expectText(terminalId, '>>> ');
    await sendText(terminalId, 'sum(range(10))');
    await sessions.sendKeys(terminalId, [{ key: 'ENTER' }]);
    const result = await expectText(terminalId, '45', field<string>(prompt, 'end_cursor'));
    await expectText(terminalId, '>>> ', field<string>(result, 'end_cursor'));
    await sessions.sendKeys(terminalId, [{ key: 'd', ctrl: true }]);
    const exited = await waitForProcess(
      terminalId,
      (view) => view.process_state !== 'running',
      'Python Ctrl-D exit',
    );
    assert(exited.process_state === 'exited' && exited.exit_code === 0, 'Python did not exit 0');
  } finally {
    await closeIfPresent(terminalId);
  }
});

await test('Bash Ctrl-Z and fg job control', async () => {
  let terminalId: string | undefined;
  try {
    const opened = await sessions.open({
      kind: 'shell',
      shell: '/bin/bash',
      login: false,
      workingDirectory: '/tmp',
      environment: {
        TERM: 'xterm-256color',
        LANG: 'C.UTF-8',
        PS1: 'VACPS> ',
        PROMPT_COMMAND: '',
      },
      columns: 80,
      rows: 24,
      timeoutMs: 20_000,
      idleTimeoutMs: 30_000,
      maxBufferBytes: 4 * 1024 * 1024,
    });
    terminalId = opened.terminal_id;
    let cursor = field<string>(await expectText(terminalId, 'VACPS> '), 'end_cursor');
    await sendText(terminalId, 'sleep 30');
    await sessions.sendKeys(terminalId, [{ key: 'ENTER' }]);
    await sleep(100);
    await sessions.sendKeys(terminalId, [{ key: 'z', ctrl: true }]);
    const stopped = await expectText(terminalId, 'Stopped', cursor);
    const stoppedPrompt = await expectText(
      terminalId,
      'VACPS> ',
      field<string>(stopped, 'end_cursor'),
    );
    cursor = field<string>(stoppedPrompt, 'end_cursor');
    assert(sessions.get(terminalId).process_state === 'running', 'shell exited after Ctrl-Z');

    await sendText(terminalId, 'jobs');
    await sessions.sendKeys(terminalId, [{ key: 'ENTER' }]);
    const jobs = await expectText(terminalId, 'Stopped', cursor);
    const jobsPrompt = await expectText(terminalId, 'VACPS> ', field<string>(jobs, 'end_cursor'));
    cursor = field<string>(jobsPrompt, 'end_cursor');
    await sendText(terminalId, 'fg');
    await sessions.sendKeys(terminalId, [{ key: 'ENTER' }]);
    const foreground = await expectText(terminalId, 'sleep 30', cursor);
    cursor = field<string>(foreground, 'end_cursor');
    await sleep(100);
    await sessions.sendKeys(terminalId, [{ key: 'c', ctrl: true }]);
    const resumedPrompt = await expectText(terminalId, 'VACPS> ', cursor);
    cursor = field<string>(resumedPrompt, 'end_cursor');
    await sendText(terminalId, 'exit 0');
    await sessions.sendKeys(terminalId, [{ key: 'ENTER' }]);
    const exited = await waitForProcess(
      terminalId,
      (view) => view.process_state !== 'running',
      'interactive Bash exit',
    );
    assert(
      exited.process_state === 'exited' && exited.exit_code === 0,
      `Bash did not exit 0: ${JSON.stringify(exited)}`,
    );
  } finally {
    await closeIfPresent(terminalId);
  }
});

await test('resize delivers SIGWINCH', async () => {
  let terminalId: string | undefined;
  try {
    const command =
      'trap \'printf "WINCH "; stty size\' WINCH; printf "READY "; stty size; while :; do sleep 1; done';
    const opened = await openCommand('/bin/bash', ['--noprofile', '--norc', '-c', command]);
    terminalId = opened.terminal_id;
    let cursor = field<string>(await expectText(terminalId, 'READY 24 80'), 'end_cursor');

    const resized = await sessions.resize(terminalId, 100, 40);
    assert(resized.columns === 100 && resized.rows === 40, 'resize metadata mismatch');
    const firstWinch = await expectText(terminalId, 'WINCH 40 100', cursor);
    cursor = field<string>(firstWinch, 'end_cursor');
    const firstScreen = await sessions.screen(terminalId);
    assert(
      field<number>(firstScreen, 'columns') === 100 && field<number>(firstScreen, 'rows') === 40,
      'screen dimensions did not follow first resize',
    );

    await sessions.resize(terminalId, 70, 20);
    await expectText(terminalId, 'WINCH 20 70', cursor);
    const secondScreen = await sessions.screen(terminalId);
    assert(
      field<number>(secondScreen, 'columns') === 70 && field<number>(secondScreen, 'rows') === 20,
      'screen dimensions did not follow second resize',
    );
  } finally {
    await closeIfPresent(terminalId);
  }
});

await test('rolling buffer overflow and UTF-8 cursors', async () => {
  let terminalId: string | undefined;
  try {
    const script = 'import sys; sys.stdout.write("行🙂0123456789\\n" * 30000); sys.stdout.flush()';
    const opened = await openCommand('/usr/bin/python3', ['-c', script], {
      maxBufferBytes: 65_536,
    });
    terminalId = opened.terminal_id;
    const reaped = await waitForProcess(
      terminalId,
      (view) => view.process_state !== 'running',
      'overflow producer exit',
      10_000,
    );
    assert(reaped.process_state === 'exited' && reaped.exit_code === 0, 'producer did not exit 0');
    // Child reap and PTY EOF are deliberately separate. Freeze the rolling
    // buffer only after the native reader has drained the slave completely.
    await waitForEof(terminalId, 10_000);
    const exited = sessions.get(terminalId);
    assert(exited.process_state === 'exited' && exited.exit_code === 0, 'producer did not exit 0');
    assert(exited.buffered_bytes <= 65_536, 'rolling buffer exceeded configured capacity');
    assert(Number(exited.available_from) > 0, 'rolling buffer did not evict old output');

    const dropped = await sessions.read(terminalId, '0', 65_536, 0);
    const availableFrom = Number(field<string>(dropped, 'available_from'));
    const droppedBytes = field<number>(dropped, 'dropped_bytes');
    const nextCursor = Number(field<string>(dropped, 'next_cursor'));
    assert(field<boolean>(dropped, 'dropped'), 'old cursor was not reported as dropped');
    assert(droppedBytes === availableFrom, 'dropped_bytes does not equal skipped byte range');
    assert(nextCursor >= availableFrom, 'next cursor moved behind available_from');
    assert(
      !field<string>(dropped, 'content').includes('\uFFFD'),
      'dropped read returned invalid UTF-8 replacement text',
    );

    const retainedStart = await sessions.read(terminalId, String(availableFrom), 64, 0);
    assert(!field<boolean>(retainedStart, 'dropped'), 'adjusted available cursor dropped again');
    assert(
      !field<string>(retainedStart, 'content').includes('\uFFFD'),
      'retained cursor did not begin at a UTF-8 boundary',
    );

    let cursor = nextCursor;
    let eof = field<boolean>(dropped, 'eof');
    for (let reads = 0; !eof && reads < 4; reads += 1) {
      const tail = await sessions.read(terminalId, String(cursor), 4, 0);
      const advanced = Number(field<string>(tail, 'next_cursor'));
      assert(!field<boolean>(tail, 'dropped'), 'continuation cursor dropped retained output');
      assert(
        advanced > cursor || field<boolean>(tail, 'eof'),
        'continuation cursor did not advance',
      );
      assert(
        !field<string>(tail, 'content').includes('\uFFFD'),
        'continuation returned invalid UTF-8 replacement text',
      );
      cursor = advanced;
      eof = field<boolean>(tail, 'eof');
    }
    assert(eof, 'retained tail did not eventually reach EOF');
    assert(cursor === Number(exited.next_cursor), 'final cursor does not equal produced bytes');
  } finally {
    await closeIfPresent(terminalId);
  }
});

await sessions.close();
log.info(`terminal_interaction_regression: ${passed}/6 passed`);

export default { passed, total: 6, ok: passed === 6 };
export async function initialize(): Promise<void> {}
export async function shutdown(): Promise<void> {}
