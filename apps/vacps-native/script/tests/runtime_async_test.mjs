/**
 * Runtime::Async integration test executed by the native QuickJS host.
 *
 * Exercises:
 * - async lifecycle awaiting;
 * - concurrent kernel/worker/native-process promises;
 * - rejection settlement followed by successful work;
 * - concurrent native HTTP events awaiting async JS handlers.
 */
import * as fs from 'vacps:fs';
import * as host from 'vacps:host';
import * as http from 'vacps:http';
import * as log from 'vacps:log';
import * as process from 'vacps:process';
import { Store } from 'vacps:store';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
let passed = 0;

function assert(condition, message) {
  if (!condition) throw new Error(message || 'assertion failed');
}

function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(
      `${message || 'assertEq'}: got ${JSON.stringify(actual)} want ${JSON.stringify(expected)}`,
    );
  }
}

async function test(name, body) {
  await body();
  passed += 1;
  log.info(`[pass] ${name}`);
}

async function fileRoundtrip(path, text) {
  const output = await fs.File.open(path, { mode: 'write' });
  try {
    await output.write(encoder.encode(text));
  } finally {
    await output.close();
  }

  const input = await fs.File.open(path, { mode: 'read' });
  try {
    return decoder.decode(await input.read());
  } finally {
    await input.close();
  }
}

export async function initialize() {
  await fs.mkdir('runtime_async', { recursive: true });

  await test('async lifecycle waits for native Promise jobs', async () => {
    const order = [];
    const pending = fs.exists('runtime_async').then((exists) => {
      assert(exists, 'runtime_async directory exists');
      order.push('resolved');
    });
    order.push('scheduled');
    await pending;
    assertEq(order.join(','), 'scheduled,resolved', 'Promise job order');
  });

  await test('concurrent FS + Store worker + Process operations', async () => {
    const database = await Store.open(host.dataDir() + '/runtime_async.db');
    try {
      await database.exec('CREATE TABLE values_test (id INTEGER PRIMARY KEY, value TEXT);');
      const [fileText, insert, child] = await Promise.all([
        fileRoundtrip('runtime_async/concurrent.txt', 'concurrent-native-io'),
        database.run('INSERT INTO values_test(value) VALUES(?);', ['worker-store']),
        process.run('/bin/echo', ['native-process']),
      ]);
      assertEq(fileText, 'concurrent-native-io', 'concurrent file result');
      assertEq(insert.changes, 1, 'concurrent store result');
      assertEq(child.exitCode, 0, 'concurrent process exit');
      assert(String(child.stdout).includes('native-process'), 'concurrent process output');
    } finally {
      await database.close();
    }
  });

  await test('rejected native Promise does not poison later jobs', async () => {
    let rejected = false;
    try {
      await fs.stat('runtime_async/does-not-exist');
    } catch {
      rejected = true;
    }
    assert(rejected, 'missing stat rejects');
    assert(await fs.exists('runtime_async'), 'later native Promise resolves');
    const child = await process.run('/bin/true');
    assertEq(child.exitCode, 0, 'later process Promise resolves');
  });

  await test('concurrent HTTP requests await async JS handlers', async () => {
    let handled = 0;
    const server = new http.Server(
      { host: '127.0.0.1', port: 0, handlerTimeoutMs: 5_000 },
      async (request) => {
        await Promise.resolve();
        assert(await fs.exists('runtime_async'), 'handler awaits native FS Promise');
        handled += 1;
        return {
          status: 200,
          headers: { 'x-runtime-async': 'ok' },
          body: request.url,
        };
      },
    );

    try {
      const address = await server.listen();
      const count = 8;
      const responses = await Promise.all(
        Array.from({ length: count }, (_, index) =>
          http.request({
            url: `http://${address.host}:${address.port}/request/${index}`,
            timeoutMs: 5_000,
          }),
        ),
      );
      assertEq(handled, count, 'all handlers completed');
      for (let index = 0; index < responses.length; index += 1) {
        assertEq(responses[index].status, 200, `response ${index} status`);
        assertEq(
          responses[index].headers['x-runtime-async'],
          'ok',
          `response ${index} header`,
        );
        assertEq(decoder.decode(responses[index].body), `/request/${index}`, `response ${index} body`);
      }
    } finally {
      await server.close();
    }
  });

  log.info(`runtime_async_test: ${passed}/4 passed`);
  await log.flush();
}

export async function shutdown() {
  await log.flush();
}
