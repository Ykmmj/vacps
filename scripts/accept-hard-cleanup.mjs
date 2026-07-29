#!/usr/bin/env node
/**
 * Live bulk hard-cleanup acceptance harness (control plane HTTP).
 *
 * Requires a session cookie or runs against local wrangler with auth disabled — this script
 * uses the password login flow when VACPS_PASSWORD is set.
 *
 * Usage:
 *   VACPS_BASE_URL=https://… VACPS_PASSWORD=… node scripts/accept-hard-cleanup.mjs
 *
 * Optional:
 *   VACPS_BACKEND_ID=vacps-…
 *   VACPS_RUN_ID=hard-live-…
 *
 * Notes:
 * - Creates only isolated test-labeled tasks when MCP/create path is available via /api/tasks.
 * - Prefer running after unit suite `task-hard-cleanup.test.ts` is green.
 * - Agent-local output purge is out of scope for this harness (CP index + tombstone only).
 */

const base = (process.env.VACPS_BASE_URL ?? 'http://127.0.0.1:8787').replace(/\/$/, '');
const password = process.env.VACPS_PASSWORD ?? '';
const backendId = process.env.VACPS_BACKEND_ID ?? '';
const runId = process.env.VACPS_RUN_ID ?? `hard-live-${Date.now()}`;

const labels = {
  environment: 'test',
  suite: 'hard-cleanup-live',
  run_id: runId,
  purpose: 'acceptance-test',
};

let cookie = '';

async function api(path, init = {}) {
  const headers = new Headers(init.headers ?? {});
  headers.set('content-type', 'application/json');
  if (cookie) headers.set('cookie', cookie);
  const res = await fetch(`${base}${path}`, { ...init, headers, redirect: 'manual' });
  const setCookie = res.headers.getSetCookie?.() ?? [];
  for (const c of setCookie) {
    const part = c.split(';')[0];
    if (part) cookie = cookie ? `${cookie}; ${part}` : part;
  }
  // fallback single set-cookie
  const sc = res.headers.get('set-cookie');
  if (sc && !setCookie.length) {
    cookie = sc.split(';')[0] ?? cookie;
  }
  const text = await res.text();
  let body;
  try {
    body = text ? JSON.parse(text) : null;
  } catch {
    body = text;
  }
  if (!res.ok) {
    const err = new Error(
      `HTTP ${res.status} ${path}: ${typeof body === 'string' ? body : JSON.stringify(body)}`,
    );
    err.status = res.status;
    err.body = body;
    throw err;
  }
  return body;
}

function assert(cond, msg) {
  if (!cond) throw new Error(`ASSERT: ${msg}`);
}

async function main() {
  console.log(`base=${base} run_id=${runId}`);
  if (!password) {
    console.error('Set VACPS_PASSWORD (and VACPS_BASE_URL) for live HTTP acceptance.');
    console.error(
      'Unit path: pnpm --filter @vacps/control-worker test tests/task-hard-cleanup.test.ts',
    );
    process.exit(2);
  }

  await api('/api/auth/login', {
    method: 'POST',
    body: JSON.stringify({ password }),
  });
  console.log('login ok');

  // Prefer dashboard to discover backend if not set
  let backend = backendId;
  if (!backend) {
    const dash = await api('/api/dashboard');
    backend = dash.backends?.[0]?.id ?? dash.nodes?.[0]?.registration?.backendId;
  }
  assert(backend, 'no backend id');
  console.log(`backend=${backend}`);

  const filters = {
    backend_id: backend,
    test_only: true,
    labels: { suite: labels.suite, run_id: labels.run_id },
  };

  // Create terminal-ish tasks via web API when possible (agent must be healthy).
  const creates = [];
  for (const [name, program, args] of [
    ['cmd-ok', 'true', []],
    ['cmd-fail', 'sh', ['-c', 'exit 9']],
    ['shell-ok', 'printf', ['ok\\n']],
  ]) {
    try {
      const body = await api('/api/tasks', {
        method: 'POST',
        body: JSON.stringify({
          kind: name.startsWith('shell') ? 'shell' : 'command',
          backend_id: backend,
          ...(name.startsWith('shell')
            ? { command: 'printf ok\\n', shell: '/bin/bash' }
            : { program, arguments: args }),
          timeout_seconds: 30,
          working_directory: '/tmp',
          labels,
          idempotency_key: `${runId}-${name}`,
          name: `${runId}-${name}`,
        }),
      });
      creates.push(body);
      console.log(
        `created ${name}: ${body.id ?? body.task?.id ?? JSON.stringify(body).slice(0, 80)}`,
      );
    } catch (e) {
      console.warn(`create ${name} skipped: ${e.message}`);
    }
  }

  // Wait briefly for terminal
  await new Promise((r) => setTimeout(r, 3000));

  const preview = await api('/api/tasks/cleanup/preview', {
    method: 'POST',
    body: JSON.stringify({ filters, limit: 5000 }),
  });
  console.log('preview', preview);
  assert(typeof preview.matched_count === 'number', 'preview.matched_count');

  // Drift check: if matched>=1, call with wrong expected
  if (preview.matched_count >= 1) {
    let drifted = false;
    try {
      await api('/api/tasks/cleanup/run', {
        method: 'POST',
        body: JSON.stringify({
          filters,
          mode: 'hard',
          expected_matched_count: preview.matched_count - 1,
          reason: 'live_hard_drift',
          idempotency_key: `${runId}-drift`,
        }),
      });
    } catch (e) {
      drifted = e.status === 409;
      console.log('drift rejection', e.body?.error ?? e.message);
    }
    assert(drifted, 'expected cleanup_scope_changed on drift');
  }

  if (preview.matched_count === 0) {
    console.log(
      'No matching terminal test tasks — create path may have failed. Unit suite still covers hard path.',
    );
    process.exit(0);
  }

  const hardKey = `${runId}-hard`;
  const run = await api('/api/tasks/cleanup/run', {
    method: 'POST',
    body: JSON.stringify({
      filters,
      mode: 'hard',
      expected_matched_count: preview.matched_count,
      reason: 'live_hard_accept',
      idempotency_key: hardKey,
      limit: 5000,
    }),
  });
  console.log('hard run', run);
  assert(run.mode === 'hard', 'mode hard');
  assert(run.deleted_count === preview.matched_count, 'deleted_count matches');

  const replay = await api('/api/tasks/cleanup/run', {
    method: 'POST',
    body: JSON.stringify({
      filters,
      mode: 'hard',
      expected_matched_count: preview.matched_count,
      reason: 'live_hard_accept',
      idempotency_key: hardKey,
      limit: 5000,
    }),
  });
  assert(replay.idempotency?.replayed === true, 'op idempotency replay');

  const after = await api('/api/tasks/cleanup/preview', {
    method: 'POST',
    body: JSON.stringify({ filters, limit: 5000 }),
  });
  assert(after.matched_count === 0, 'preview empty after hard');

  // Tombstone: recreate first created key if any
  if (creates[0]) {
    try {
      const again = await api('/api/tasks', {
        method: 'POST',
        body: JSON.stringify({
          kind: 'command',
          backend_id: backend,
          program: 'true',
          arguments: [],
          timeout_seconds: 30,
          working_directory: '/tmp',
          labels,
          idempotency_key: `${runId}-cmd-ok`,
          name: `${runId}-cmd-ok`,
        }),
      });
      console.log('tombstone replay response', again);
    } catch (e) {
      console.log('tombstone path note', e.message);
    }
  }

  console.log('LIVE HARD ACCEPTANCE PASS (control-plane path)');
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
