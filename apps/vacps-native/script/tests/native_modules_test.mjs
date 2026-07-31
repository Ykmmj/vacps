/**
 * Integration tests for C++ vacps:* modules (runs inside QuickJS ScriptRuntime, not Node).
 * Loaded by tests/test_js_native_api.cpp via ScriptRuntime::eval_module + await_value.
 *
 * On success: top-level await completes and default export is true.
 * On failure: throws Error (Promise rejects → C++ test fails).
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
  assertEq(host.platform(), 'linux-x86_64-musl', 'platform string');
});

// ── vacps:log ─────────────────────────────────────────────────────

await test('log levels + flush', () => {
  log.trace('trace-js');
  log.debug('debug-js');
  log.info('info-js');
  log.warn('warn-js');
  log.error('error-js');
  log.flush();
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

// ── vacps:fs (File.open + namespace ops only) ──────────────────────

async function readTextFile(path) {
  const f = await fs.File.open(path, fs.O_RDONLY);
  try {
    return await f.readText();
  } finally {
    await f.close();
  }
}

async function writeTextFile(path, content) {
  const f = await fs.File.open(path, fs.O_WRONLY | fs.O_CREAT | fs.O_TRUNC);
  try {
    await f.writeText(content);
  } finally {
    await f.close();
  }
}

async function appendTextFile(path, content) {
  const f = await fs.File.open(path, fs.O_WRONLY | fs.O_CREAT | fs.O_APPEND);
  try {
    await f.writeText(content);
  } finally {
    await f.close();
  }
}

async function readBytesFile(path) {
  const f = await fs.File.open(path, fs.O_RDONLY);
  try {
    return await f.read();
  } finally {
    await f.close();
  }
}

async function writeBytesFile(path, data) {
  const f = await fs.File.open(path, fs.O_WRONLY | fs.O_CREAT | fs.O_TRUNC);
  try {
    await f.write(data);
  } finally {
    await f.close();
  }
}

await test('fs File write/read/append/exists/readDirectory/rename/remove', async () => {
  await fs.mkdir('js_api/fs');
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
});

// Path allowlist is JS path-guard.ts at tool boundaries — not C++ vacps:fs.
// Pure I/O: relative paths under dataDir; absolute paths open as given.
await test('fs pure I/O relative under dataDir', async () => {
  await writeTextFile('js_api/fs/pure.txt', 'pure');
  assertEq(await readTextFile('js_api/fs/pure.txt'), 'pure', 'relative pure I/O');
  await fs.remove('js_api/fs/pure.txt');
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

await test('http.Server listen/close', async () => {
  // Use a high ephemeral-ish port from nowMs to avoid clashes in parallel tests.
  const port = 20000 + (host.nowMs() % 20000);
  const server = new http.Server({ host: '127.0.0.1', port });
  assert(server.listening === false, 'not listening yet');
  await server.listen();
  assert(server.listening === true, 'listening');
  await server.close();
  assert(server.listening === false, 'closed');
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

// ── summary ───────────────────────────────────────────────────────

const passed = results.filter((r) => r.ok).length;
log.info(`native_modules_test: ${passed}/${results.length} passed`);
log.flush();

export default {
  passed,
  total: results.length,
  ok: passed === results.length,
};
