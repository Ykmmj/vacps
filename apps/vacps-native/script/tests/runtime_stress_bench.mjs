/**
 * QuickJS/native runtime stress and throughput benchmark.
 *
 * This is an integration workload, not a microbenchmarking authority. Results
 * are intended for regressions on the same host/build. Every timed workload
 * also validates its result so high throughput cannot hide broken settlement.
 */
import * as crypto from 'vacps:crypto';
import * as fs from 'vacps:fs';
import * as host from 'vacps:host';
import * as http from 'vacps:http';
import * as log from 'vacps:log';
import * as process from 'vacps:process';
import { Store } from 'vacps:store';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

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

function formatRate(operations, elapsedMs) {
  return Math.round((operations * 1_000) / Math.max(1, elapsedMs));
}

async function benchmark(name, operations, body) {
  const started = host.nowMs();
  await body();
  const elapsedMs = host.nowMs() - started;
  const rate = formatRate(operations, elapsedMs);
  log.info(`[bench] ${name}: ops=${operations} elapsed_ms=${elapsedMs} ops_per_sec=${rate}`);
  return { elapsedMs, rate };
}

async function runPool(total, concurrency, task) {
  let next = 0;
  const workerCount = Math.min(total, concurrency);
  await Promise.all(
    Array.from({ length: workerCount }, async () => {
      for (;;) {
        const index = next;
        next += 1;
        if (index >= total) return;
        await task(index);
      }
    }),
  );
}

function percentile(sorted, fraction) {
  if (sorted.length === 0) return 0;
  return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * fraction))];
}

async function writeFixture(path, bytes) {
  const file = await fs.File.open(path, { mode: 'write' });
  try {
    assertEq(await file.write(bytes), bytes.byteLength, 'fixture bytes written');
    await file.flush();
  } finally {
    await file.close();
  }
}

async function synchronousBenchmarks() {
  await benchmark('crypto.sha256Hex native calls', 100_000, () => {
    let digest = '';
    for (let index = 0; index < 100_000; index += 1) {
      digest = crypto.sha256Hex(`vacps-stress-${index & 255}`);
    }
    assertEq(digest.length, 64, 'last SHA-256 digest length');
  });

  await benchmark('URL parse + URLSearchParams', 50_000, () => {
    let last = '';
    for (let index = 0; index < 50_000; index += 1) {
      const url = new URL(`/path/${index}?a=${index}&b=two`, 'http://127.0.0.1:8080');
      last = url.searchParams.get('b');
    }
    assertEq(last, 'two', 'last URL parameter');
  });

  await benchmark('TextEncoder/TextDecoder roundtrip', 100_000, () => {
    let last = '';
    for (let index = 0; index < 100_000; index += 1) {
      last = decoder.decode(encoder.encode(`vacps-${index & 255}-中文`));
    }
    assert(last.endsWith('-中文'), 'last encoding roundtrip');
  });
}

async function promiseAndFileBenchmarks() {
  await benchmark('QuickJS Promise microtask awaits', 100_000, async () => {
    let completed = 0;
    for (let index = 0; index < 100_000; index += 1) {
      await Promise.resolve();
      completed += 1;
    }
    assertEq(completed, 100_000, 'microtasks completed');
  });

  await fs.mkdir('stress', { recursive: true });
  const fixture = new Uint8Array(1024 * 1024);
  for (let index = 0; index < fixture.length; index += 1) {
    fixture[index] = index & 255;
  }
  await writeFixture('stress/fixture.bin', fixture);

  const file = await fs.File.open('stress/fixture.bin', { mode: 'read' });
  try {
    await benchmark('FS queued readAt on one File (concurrency 64)', 5_000, async () => {
      await runPool(5_000, 64, async (index) => {
        const offset = (index * 4096) % (fixture.length - 4096);
        const bytes = new Uint8Array(await file.readAt(offset, 4096));
        assertEq(bytes.byteLength, 4096, 'readAt byte length');
        assertEq(bytes[0], offset & 255, 'readAt first byte');
      });
    });
  } finally {
    await file.close();
  }

  await benchmark('FS namespace exists worker jobs (concurrency 64)', 5_000, async () => {
    await runPool(5_000, 64, async () => {
      assert(await fs.exists('stress/fixture.bin'), 'fixture exists');
    });
  });

  await benchmark('native Promise rejection settlement (concurrency 64)', 2_000, async () => {
    let rejected = 0;
    await runPool(2_000, 64, async (index) => {
      try {
        await fs.stat(`stress/missing-${index}`);
      } catch {
        rejected += 1;
      }
    });
    assertEq(rejected, 2_000, 'all missing stats rejected');
    assert(await fs.exists('stress/fixture.bin'), 'runtime remains usable after rejection storm');
  });
}

async function storeBenchmarks() {
  const database = await Store.open(host.dataDir() + '/stress.db');
  try {
    await database.exec(
      'PRAGMA journal_mode=WAL; CREATE TABLE bench (id INTEGER PRIMARY KEY, value TEXT NOT NULL);',
    );

    const totalRows = 5_000;
    const chunkSize = 100;
    await benchmark('SQLite transaction inserts', totalRows, async () => {
      for (let base = 0; base < totalRows; base += chunkSize) {
        const steps = Array.from({ length: chunkSize }, (_, offset) => ({
          sql: 'INSERT INTO bench(value) VALUES(?);',
          params: [`value-${base + offset}`],
          expectedChanges: { exactly: 1 },
        }));
        const results = await database.transaction(steps);
        assertEq(results.length, chunkSize, 'transaction result count');
      }
    });

    await benchmark('SQLite indexed point queries (concurrency 32)', 2_000, async () => {
      await runPool(2_000, 32, async (index) => {
        const id = (index % totalRows) + 1;
        const rows = await database.query('SELECT value FROM bench WHERE id = ?;', [id]);
        assertEq(rows.length, 1, 'point query row count');
        assertEq(rows[0].value, `value-${id - 1}`, 'point query value');
      });
    });
  } finally {
    await database.close();
  }
}

async function httpBenchmarks() {
  let handled = 0;
  const peers = new Set();
  const server = new http.Server(
    { host: '127.0.0.1', port: 0, handlerTimeoutMs: 10_000 },
    async (request) => {
      peers.add(request.remoteAddress);
      if (request.url.startsWith('/native/')) {
        assert(await fs.exists('stress/fixture.bin'), 'handler native FS await');
      } else {
        await Promise.resolve();
      }
      handled += 1;
      return {
        status: 200,
        headers: { 'x-vacps-stress': 'ok' },
        body: 'ok',
      };
    },
  );

  try {
    const address = await server.listen();
    const origin = `http://${address.host}:${address.port}`;
    const latency = [];

    await benchmark('HTTP loopback sequential pooled connection', 1_000, async () => {
      for (let index = 0; index < 1_000; index += 1) {
        const response = await http.request({
          url: `${origin}/sequential/${index}`,
          timeoutMs: 10_000,
        });
        assertEq(response.status, 200, 'sequential HTTP status');
      }
    });
    assertEq(peers.size, 1, 'sequential benchmark reused one connection');

    await benchmark('HTTP loopback async JS handler (concurrency 64)', 2_000, async () => {
      await runPool(2_000, 64, async (index) => {
        const started = host.nowMs();
        const response = await http.request({
          url: `${origin}/promise/${index}`,
          timeoutMs: 10_000,
        });
        latency.push(host.nowMs() - started);
        assertEq(response.status, 200, 'HTTP status');
        assertEq(response.headers['x-vacps-stress'], 'ok', 'HTTP response header');
        assertEq(decoder.decode(response.body), 'ok', 'HTTP response body');
      });
    });

    latency.sort((left, right) => left - right);
    assert(peers.size <= 16, `per-origin connection cap exceeded: ${peers.size}`);
    log.info(
      `[latency] HTTP loopback ms: p50=${percentile(latency, 0.5)} ` +
        `p95=${percentile(latency, 0.95)} p99=${percentile(latency, 0.99)}`,
    );

    await benchmark('HTTP handler awaiting native FS (concurrency 32)', 1_000, async () => {
      await runPool(1_000, 32, async (index) => {
        const response = await http.request({
          url: `${origin}/native/${index}`,
          timeoutMs: 10_000,
        });
        assertEq(response.status, 200, 'native-await HTTP status');
      });
    });

    assertEq(handled, 4_000, 'all HTTP handlers completed');
  } finally {
    await server.close();
  }
}

async function processBenchmark() {
  await benchmark('Process spawn/wait/close (concurrency 8)', 100, async () => {
    await runPool(100, 8, async () => {
      const result = await process.run('/bin/true');
      assertEq(result.exitCode, 0, 'child exit code');
      assert(result.timedOut === false, 'child did not time out');
    });
  });
}

export async function initialize() {
  const started = host.nowMs();
  await synchronousBenchmarks();
  await promiseAndFileBenchmarks();
  await storeBenchmarks();
  await httpBenchmarks();
  await processBenchmark();
  const elapsedMs = host.nowMs() - started;
  log.info(`[stress_bench] COMPLETE elapsed_ms=${elapsedMs}`);
  await log.flush();
}

export async function shutdown() {
  await log.flush();
}
