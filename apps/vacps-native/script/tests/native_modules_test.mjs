/**
 * Integration tests for C++ vacps:* modules (runs inside QuickJS Runtime/host, not Node).
 * Standalone ESM smoke script for the vacps:* module surface.
 *
 * On success: top-level await completes and default export is true.
 * On failure: throws Error so the host run fails.
 */
import * as log from 'vacps:log';
import * as host from 'vacps:host';
import { Store } from 'vacps:store';
import * as fs from 'vacps:fs';
import * as crypto from 'vacps:crypto';
import * as process from 'vacps:process';
import * as http from 'vacps:http';

// ── minimal harness ───────────────────────────────────────────────

const results = [];

function ok(name) {
  results.push({ name, ok: true });
  log.info(`[pass] ${name}`);
}

function fail(name, err) {
  const msg = err && err.message ? err.message : String(err);
  results.push({ name, ok: false, msg });
  log.error(`[fail] ${name}: ${msg}`);
  throw new Error(`${name}: ${msg}`);
}

async function test(name, fn) {
  try {
    await fn();
    ok(name);
  } catch (e) {
    fail(name, e);
  }
}

function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

function assertEq(a, b, msg) {
  if (a !== b) {
    throw new Error(`${msg || 'assertEq'}: got ${JSON.stringify(a)} want ${JSON.stringify(b)}`);
  }
}

function abLen(buf) {
  if (buf instanceof ArrayBuffer) return buf.byteLength;
  if (buf && typeof buf.byteLength === 'number') return buf.byteLength;
  throw new Error('expected ArrayBuffer-like');
}

// ── vacps:host ────────────────────────────────────────────────────

await test('host.version', () => {
  const v = host.version();
  assert(typeof v === 'string' && v.length > 0, 'version non-empty string');
});

await test('host.dataDir', () => {
  const d = host.dataDir();
  assert(typeof d === 'string' && d.length > 0, 'dataDir set');
});

await test('host.nowMs', () => {
  const t = host.nowMs();
  assert(typeof t === 'number' && t > 1_700_000_000_000, 'nowMs looks like unix ms');
});

await test('host.platform', () => {
  const p = host.platform();
  assert(typeof p === 'string' && p.length > 0, 'platform non-empty string');
  assert(
    /^(linux|darwin)-(x86_64|aarch64)-(musl|gnu|unknown)$/.test(p),
    'platform format: ' + p,
  );
});

// ── vacps:log ─────────────────────────────────────────────────────

await test('log levels + async flush', async () => {
  log.trace('trace-js');
  log.debug('debug-js');
  log.info('info-js');
  log.warn('warn-js');
  log.error('error-js');
  await log.flush();
});

// ── global URL / Encoding APIs ───────────────────────────────────

await test('URL + live URLSearchParams', () => {
  const url = new URL('/items?a=1&a=2', 'http://127.0.0.1:8080/base');
  assertEq(url.href, 'http://127.0.0.1:8080/items?a=1&a=2', 'resolved href');
  assertEq(url.searchParams.getAll('a').join(','), '1,2', 'repeated params');
  url.searchParams.append('b', 'hello world');
  assertEq(url.search, '?a=1&a=2&b=hello+world', 'live params update URL');
  url.search = '?z=9';
  assertEq(url.searchParams.get('z'), '9', 'search update live params');
  assert(URL.canParse('/relative', url), 'canParse with URL base');
});

await test('TextEncoder/TextDecoder UTF-8 + encodeInto', () => {
  const encoder = new TextEncoder();
  const decoder = new TextDecoder('utf-8', { fatal: true });
  const encoded = encoder.encode('vacps-中文');
  assertEq(decoder.decode(encoded), 'vacps-中文', 'UTF-8 roundtrip');
  const destination = new Uint8Array(8);
  const progress = encoder.encodeInto('中A', destination);
  assertEq(progress.read, 2, 'encodeInto UTF-16 units read');
  assertEq(progress.written, 4, 'encodeInto bytes written');
  assertEq(decoder.decode(destination.subarray(0, progress.written)), '中A', 'encodeInto bytes');
});

// ── vacps:store ───────────────────────────────────────────────────

await test('store open/exec/run/query/close', async () => {
  const path = host.dataDir() + '/js_api_store.db';
  const db = await Store.open(path);
  assertEq(db.path, path, 'path');
  assert(db.closed === false, 'closed=false when open');
  await db.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, n REAL);');
  const r = await db.run('INSERT INTO t(name, n) VALUES(?, ?);', ['alice', 1.5]);
  assert(r.changes === 1, 'changes=1');
  assert(typeof r.lastInsertRowid === 'number', 'lastInsertRowid');
  const rows = await db.query('SELECT id, name, n FROM t WHERE name = ?;', ['alice']);
  assert(Array.isArray(rows) && rows.length === 1, 'one row');
  assertEq(rows[0].name, 'alice', 'name');
  assert(rows[0].n === 1.5 || rows[0].n === 1, 'n numeric');
  await db.close();
  assert(db.closed === true, 'closed=true after close');
});

await test('store transaction rollback on expectedChanges miss', async () => {
  const db = await Store.open(host.dataDir() + '/js_api_tx.db');
  await db.exec('CREATE TABLE u (id INTEGER PRIMARY KEY, v TEXT);');
  await db.run('INSERT INTO u(v) VALUES(?);', ['keep']);
  // Second step expectedChanges miss → whole txn rolls back (no begin/commit/rollback API).
  let rejected = false;
  try {
    await db.transaction([
      { sql: 'INSERT INTO u(v) VALUES(?);', params: ['drop'] },
      {
        sql: 'INSERT INTO u(v) VALUES(?);',
        params: ['drop2'],
        expectedChanges: { exactly: 99 },
      },
    ]);
  } catch {
    rejected = true;
  }
  assert(rejected, 'transaction must reject on expectedChanges miss');
  const rows = await db.query('SELECT v FROM u;');
  assertEq(rows.length, 1, 'only keep');
  assertEq(rows[0].v, 'keep', 'value');
  await db.close();
});

await test('store concurrent submissions are FIFO through close', async () => {
  const db = await Store.open(host.dataDir() + '/js_api_store_fifo.db');
  await db.exec(
    'DROP TABLE IF EXISTS fifo; CREATE TABLE fifo (id INTEGER PRIMARY KEY, v INTEGER);',
  );

  const writes = [];
  for (let i = 0; i < 256; ++i) {
    writes.push(db.run('INSERT INTO fifo(v) VALUES(?);', [i]));
  }
  const rowsPromise = db.query('SELECT v FROM fifo ORDER BY id;');
  const closePromise = db.close();

  await Promise.all(writes);
  const rows = await rowsPromise;
  await closePromise;

  assertEq(rows.length, 256, 'FIFO query observes every earlier write');
  for (let i = 0; i < rows.length; ++i) {
    assertEq(rows[i].v, i, `FIFO row ${i}`);
  }
  assert(db.closed === true, 'FIFO close runs after prior operations');
});

// ── vacps:fs (File.open + namespace ops only; bytes-first) ─────────

const fsTextEncoder = new TextEncoder();
const fsTextDecoder = new TextDecoder();

async function readTextFile(path) {
  const f = await fs.File.open(path, { mode: 'read' });
  try {
    return fsTextDecoder.decode(await f.read());
  } finally {
    await f.close();
  }
}

async function writeTextFile(path, content) {
  const f = await fs.File.open(path, { mode: 'write' });
  try {
    await f.write(fsTextEncoder.encode(content));
  } finally {
    await f.close();
  }
}

async function appendTextFile(path, content) {
  const f = await fs.File.open(path, { mode: 'append' });
  try {
    await f.write(fsTextEncoder.encode(content));
  } finally {
    await f.close();
  }
}

async function readBytesFile(path) {
  const f = await fs.File.open(path, { mode: 'read' });
  try {
    return await f.read();
  } finally {
    await f.close();
  }
}

async function writeBytesFile(path, data) {
  const f = await fs.File.open(path, { mode: 'write' });
  try {
    await f.write(data);
  } finally {
    await f.close();
  }
}

await test('fs File write/read/append/exists/readDirectory/rename/remove', async () => {
  await fs.mkdir('js_api/fs', { recursive: true });
  await writeTextFile('js_api/fs/a.txt', 'hello');
  assertEq(await readTextFile('js_api/fs/a.txt'), 'hello', 'readText');
  await appendTextFile('js_api/fs/a.txt', '-world');
  assertEq(await readTextFile('js_api/fs/a.txt'), 'hello-world', 'append');
  assert(await fs.exists('js_api/fs/a.txt'), 'exists');

  const bytes = await readBytesFile('js_api/fs/a.txt');
  assert(abLen(bytes) === 'hello-world'.length, 'readBytes length');

  await writeBytesFile('js_api/fs/b.bin', new Uint8Array([1, 2, 3, 4]));
  const b2 = await readBytesFile('js_api/fs/b.bin');
  assert(abLen(b2) === 4, 'writeBytes/readBytes');

  const entries = await fs.readDirectory('js_api/fs');
  assert(Array.isArray(entries) && entries.length >= 2, 'readDirectory');
  const names = entries.map((e) => e.name).sort();
  assert(names.includes('a.txt') && names.includes('b.bin'), 'readDirectory names');

  await fs.rename('js_api/fs/a.txt', 'js_api/fs/c.txt');
  assert(!(await fs.exists('js_api/fs/a.txt')), 'renamed away');
  assert(await fs.exists('js_api/fs/c.txt'), 'renamed to');

  await fs.remove('js_api/fs/c.txt');
  assert(!(await fs.exists('js_api/fs/c.txt')), 'removed');

  // remove default is non-recursive; recursive clears a tree.
  await fs.mkdir('js_api/fs/tree');
  await writeTextFile('js_api/fs/tree/x.txt', 't');
  let removeTreeFailed = false;
  try {
    await fs.remove('js_api/fs/tree');
  } catch {
    removeTreeFailed = true;
  }
  assert(removeTreeFailed, 'remove non-empty dir without recursive fails');
  await fs.remove('js_api/fs/tree', { recursive: true });
  assert(!(await fs.exists('js_api/fs/tree')), 'recursive remove');

  // rename replace: default fails when target exists; replace:true overwrites.
  await writeTextFile('js_api/fs/r1.txt', 'one');
  await writeTextFile('js_api/fs/r2.txt', 'two');
  let renameFail = false;
  try {
    await fs.rename('js_api/fs/r1.txt', 'js_api/fs/r2.txt');
  } catch {
    renameFail = true;
  }
  assert(renameFail, 'rename without replace fails when target exists');
  await fs.rename('js_api/fs/r1.txt', 'js_api/fs/r2.txt', { replace: true });
  assertEq(await readTextFile('js_api/fs/r2.txt'), 'one', 'rename replace');
  await fs.remove('js_api/fs/r2.txt');
});

// Pure I/O: relative paths under dataDir; absolute paths open as given.
// No path allowlist in C++ vacps:fs or the JS module surface.
await test('fs pure I/O relative under dataDir', async () => {
  await writeTextFile('js_api/fs/pure.txt', 'pure');
  assertEq(await readTextFile('js_api/fs/pure.txt'), 'pure', 'relative pure I/O');
  await fs.remove('js_api/fs/pure.txt');
});

await test('fs FileOperationQueue serializes concurrent handle operations', async () => {
  await writeTextFile('js_api/fs/queued.txt', 'alpha-beta-gamma');
  const file = await fs.File.open('js_api/fs/queued.txt', { mode: 'read' });
  try {
    const [alpha, beta, stat] = await Promise.all([
      file.readAt(0, 5),
      file.readAt(6, 4),
      file.stat(),
    ]);
    assertEq(fsTextDecoder.decode(alpha), 'alpha', 'first queued read');
    assertEq(fsTextDecoder.decode(beta), 'beta', 'second queued read');
    assertEq(stat.size, 'alpha-beta-gamma'.length, 'queued stat');
  } finally {
    await file.close();
    await fs.remove('js_api/fs/queued.txt');
  }
});

// ── vacps:crypto ──────────────────────────────────────────────────

await test('crypto.sha256Hex abc', () => {
  assertEq(
    crypto.sha256Hex('abc'),
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    'sha256Hex(abc)',
  );
});

await test('crypto.randomBytes + hex roundtrip', () => {
  const raw = crypto.randomBytes(16);
  assert(abLen(raw) === 16, 'randomBytes 16');
  const hex = crypto.toHex(raw);
  assert(typeof hex === 'string' && hex.length === 32, 'hex length');
  const back = crypto.fromHex(hex);
  assert(abLen(back) === 16, 'fromHex length');
  assertEq(crypto.toHex(back), hex, 'hex roundtrip');
});

await test('crypto.base64 + base64url roundtrip', () => {
  const enc = crypto.base64Encode('vacps-base64');
  assertEq(enc, 'dmFjcHMtYmFzZTY0', 'standard base64');
  const back = crypto.base64Decode(enc);
  assertEq(crypto.base64Encode(back), enc, 'b64 re-encode');
  const url = crypto.base64UrlEncode(new Uint8Array([0xfb, 0xff]));
  assert(typeof url === 'string' && url.length > 0, 'url form non-empty');
  assert(!url.includes('+') && !url.includes('/'), 'url alphabet');
  const urlBack = crypto.base64UrlDecode(url);
  assertEq(crypto.base64UrlEncode(urlBack), url, 'url roundtrip');
});

await test('crypto.ed25519 sign/verify', () => {
  const seed = crypto.randomBytes(32);
  const pub = crypto.ed25519PublicFromPrivate(seed);
  assert(abLen(pub) === 32, 'public 32');
  const msg = 'vacps-js-ed25519';
  const sig = crypto.ed25519Sign(seed, msg);
  assert(abLen(sig) === 64, 'sig 64');
  assert(crypto.ed25519Verify(pub, msg, sig) === true, 'verify ok');
  assert(crypto.ed25519Verify(pub, 'tampered', sig) === false, 'verify fail');
});

await test('crypto.ed25519SeedFromPrivateKey raw', () => {
  const seed = crypto.randomBytes(32);
  const b64 = crypto.base64UrlEncode(seed);
  const back = crypto.ed25519SeedFromPrivateKey(b64);
  assertEq(crypto.toHex(back), crypto.toHex(seed), 'seed roundtrip');
});

// ── vacps:process ─────────────────────────────────────────────────

await test('process.run /bin/true', async () => {
  const r = await process.run('/bin/true');
  assertEq(r.exitCode, 0, 'exit 0');
  assert(r.timedOut === false, 'not timed out');
});

await test('process.run /bin/false', async () => {
  const r = await process.run('/bin/false');
  assert(r.exitCode !== 0, 'non-zero');
});

await test('process.run capture stdout', async () => {
  const r = await process.run('/bin/echo', ['vacps-js-hi']);
  assertEq(r.exitCode, 0, 'echo exit');
  assert(String(r.stdout).includes('vacps-js-hi'), 'stdout');
});

await test('process.run timeout', async () => {
  const r = await process.run('/bin/sleep', ['5'], { timeoutMs: 200 });
  assert(r.timedOut === true, 'timedOut');
});

await test('Process class start/wait/close', async () => {
  const p = new process.Process('/bin/echo', ['vacps-proc-class']);
  await p.start();
  const r = await p.wait();
  assertEq(r.exitCode, 0, 'Process.wait exit');
  assert(String(r.stdout).includes('vacps-proc-class'), 'Process stdout');
  await p.close();
});

// ── vacps:http ────────────────────────────────────────────────────

await test('http.Server async handler + loopback request + close', async () => {
  // This deliberately crosses both directions:
  // native accept -> JS handler -> native fs Promise -> JS response -> native write.
  let handled = 0;
  const server = new http.Server({ host: '127.0.0.1', port: 0 }, async (request) => {
    assertEq(request.method, 'POST', 'handler method');
    assertEq(request.url, '/async?value=42', 'handler request-target');
    assertEq(fsTextDecoder.decode(request.body), 'ping', 'handler body');
    assert(await fs.exists('js_api/fs'), 'native Promise awaited inside JS handler');
    handled += 1;
    return {
      status: 201,
      headers: { 'x-vacps-async': 'handled' },
      body: fsTextEncoder.encode('pong'),
    };
  });
  assert(server.listening === false, 'not listening yet');
  assert(server.address === undefined, 'address undefined before listen');
  try {
    const addr = await server.listen();
    assert(server.listening === true, 'listening');
    assert(typeof addr.host === 'string' && addr.host.length > 0, 'listen host');
    assert(typeof addr.port === 'number' && addr.port > 0, 'ephemeral port > 0');
    assert(server.address !== undefined, 'address set while listening');
    assertEq(server.address.host, addr.host, 'address.host');
    assertEq(server.address.port, addr.port, 'address.port');

    const response = await http.request({
      url: `http://${addr.host}:${addr.port}/async?value=42`,
      method: 'POST',
      headers: { 'x-vacps-test': 'loopback' },
      body: fsTextEncoder.encode('ping'),
      timeoutMs: 5_000,
    });
    assertEq(response.status, 201, 'loopback status');
    assertEq(response.headers['x-vacps-async'], 'handled', 'loopback response header');
    assertEq(fsTextDecoder.decode(response.body), 'pong', 'loopback response body');
    assertEq(handled, 1, 'handler invocation count');
  } finally {
    await server.close();
  }
  assert(server.listening === false, 'closed');
  // close is idempotent
  await server.close();
  assert(server.listening === false, 'closed twice');
});

await test('http.request rejects bad url', async () => {
  let threw = false;
  try {
    await http.request({ url: 'not-a-url' });
  } catch {
    threw = true;
  }
  assert(threw, 'expected reject for bad url');
});

await test('http.request uses one absolute timeout and recovers', async () => {
  const server = new http.Server(
    { host: '127.0.0.1', port: 0 },
    async (request) => {
      if (request.url === '/slow') {
        await process.run('/bin/sleep', ['0.1']);
      }
      return { status: 200, body: request.url };
    },
  );

  try {
    const address = await server.listen();
    const origin = `http://${address.host}:${address.port}`;
    let timeoutMessage = '';
    try {
      await http.request({ url: `${origin}/slow`, timeoutMs: 20 });
    } catch (error) {
      timeoutMessage = error && error.message ? error.message : String(error);
    }
    assert(timeoutMessage.includes('timed out'), `timeout message: ${timeoutMessage}`);

    const recovered = await http.request({ url: `${origin}/fast`, timeoutMs: 1_000 });
    assertEq(recovered.status, 200, 'request after timeout status');
    assertEq(fsTextDecoder.decode(recovered.body), '/fast', 'request after timeout body');
  } finally {
    await server.close();
  }
});

await test('http.request reuses connections, bounds concurrency, and recovers', async () => {
  const peers = new Set();
  let handled = 0;
  const server = new http.Server(
    { host: '127.0.0.1', port: 0, maxResponseBytes: 1024 * 1024 },
    async (request) => {
      peers.add(request.remoteAddress);
      handled += 1;
      await Promise.resolve();
      if (request.url === '/head') {
        return { status: 200, body: 'head-payload-is-not-sent' };
      }
      if (request.url === '/large') {
        return { status: 200, body: new Uint8Array(4096) };
      }
      return { status: 200, body: request.url };
    },
  );

  try {
    const address = await server.listen();
    const origin = `http://${address.host}:${address.port}`;

    for (let index = 0; index < 32; index += 1) {
      const response = await http.request({ url: `${origin}/sequential/${index}` });
      assertEq(response.status, 200, `sequential ${index} status`);
    }
    assertEq(peers.size, 1, 'sequential requests reuse one TCP connection');

    const responses = await Promise.all(
      Array.from({ length: 64 }, (_, index) =>
        http.request({ url: `${origin}/concurrent/${index}`, timeoutMs: 5_000 }),
      ),
    );
    assertEq(responses.length, 64, 'all concurrent requests completed');
    assert(peers.size <= 16, `per-origin connection cap exceeded: ${peers.size}`);

    const head = await http.request({ url: `${origin}/head`, method: 'HEAD' });
    assertEq(head.status, 200, 'HEAD status');
    assertEq(head.body.byteLength, 0, 'HEAD body is empty despite Content-Length');

    let bodyLimitRejected = false;
    try {
      await http.request({ url: `${origin}/large`, maxResponseBytes: 16 });
    } catch {
      bodyLimitRejected = true;
    }
    assert(bodyLimitRejected, 'body limit rejects while reading');

    const recovered = await http.request({ url: `${origin}/recovered` });
    assertEq(recovered.status, 200, 'request after discarded connection succeeds');
    assertEq(fsTextDecoder.decode(recovered.body), '/recovered', 'recovery body');
    assertEq(handled, 99, 'all server handlers ran');
  } finally {
    await server.close();
  }
});

// ── summary ───────────────────────────────────────────────────────

const passed = results.filter((r) => r.ok).length;
log.info(`native_modules_test: ${passed}/${results.length} passed`);
log.flush();

export default {
  passed,
  total: results.length,
  ok: passed === results.length,
};

export async function initialize() {}
export async function shutdown() {}
