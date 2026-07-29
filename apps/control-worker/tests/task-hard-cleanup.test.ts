/**
 * Bulk hard-delete acceptance (control-plane path).
 * Covers the r6 observation: cleanup.run mode=hard must be safe before production use.
 */
import { describe, expect, it, vi } from 'vitest';

import type { CreateTaskInput } from '@vacps/contracts';

import { AppError } from '../src/lib/http.js';
import { TaskService } from '../src/tasks/task-service.js';
import { FakeD1, asD1, type FakeRow } from './fake-d1.js';

const BACKEND = 'vacps-df15d16eb52d';
const RUN = 'hard-accept-20260729';

function labels(extra: Record<string, string> = {}) {
  return {
    environment: 'test',
    suite: 'hard-cleanup-accept',
    run_id: RUN,
    purpose: 'acceptance-test',
    ...extra,
  };
}

function seedTerminal(
  db: FakeD1,
  partial: {
    id?: string;
    kind?: string;
    status: string;
    idempotency_key?: string | null;
    request_hash?: string | null;
    name?: string;
    summary?: string;
  },
): FakeRow {
  const now = new Date().toISOString();
  const id = partial.id ?? crypto.randomUUID();
  const lab = labels({ kind: partial.kind ?? 'command' });
  const row: FakeRow = {
    id,
    backend_id: BACKEND,
    type: partial.kind === 'agent' ? 'agent' : 'shell',
    kind: partial.kind ?? 'command',
    source: 'mcp',
    profile: 'full',
    name: partial.name ?? null,
    summary: partial.summary ?? `${partial.kind ?? 'command'} ${partial.status}`,
    status: partial.status,
    schedule_id: null,
    idempotency_key: partial.idempotency_key ?? null,
    request_hash: partial.request_hash ?? null,
    retry_of_task_id: null,
    created_at: now,
    updated_at: now,
    finished_at: now,
    terminal_at: now,
    expires_at: new Date(Date.now() + 3 * 86_400_000).toISOString(),
    labels_json: JSON.stringify(lab),
    environment: 'test',
    retention_class: 'test',
    deleted_at: null,
    deleted_by: null,
    deletion_reason: null,
    cleanup_state: 'eligible',
  };
  db.tasks.push(row);
  return row;
}

function service(db: FakeD1, createTask = vi.fn(async () => ({}))) {
  const backends = {
    get: vi.fn(async () => ({
      id: BACKEND,
      enabled: true,
      baseUrl: 'https://agent.example',
      name: 'test',
    })),
  };
  const client = {
    createTask,
    getTask: vi.fn(async () => ({ status: 'succeeded' })),
    cancelTask: vi.fn(async () => ({ cancelled: true })),
    getLogs: vi.fn(async () => ({})),
    getCapabilities: vi.fn(async () => ({})),
  };
  return new TaskService(asD1(db), backends as never, client as never);
}

const filters = {
  backendId: BACKEND,
  testOnly: true,
  labels: { suite: 'hard-cleanup-accept', run_id: RUN },
};

describe('bulk hard cleanup acceptance', () => {
  it('1–4,8: preview exact count, hard-delete mix of kinds/statuses, list empty', async () => {
    const db = new FakeD1();
    // Command success/fail + shell cancel (and one active that must stay out of preview)
    seedTerminal(db, {
      kind: 'command',
      status: 'succeeded',
      name: 'cmd-ok',
      idempotency_key: 'hard-ok-1',
      request_hash: 'sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    });
    seedTerminal(db, {
      kind: 'command',
      status: 'failed',
      name: 'cmd-fail',
      idempotency_key: 'hard-fail-1',
      request_hash: 'sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    });
    seedTerminal(db, {
      kind: 'shell',
      status: 'cancelled',
      name: 'shell-cancel',
      idempotency_key: 'hard-cancel-1',
      request_hash: 'sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',
    });
    // Active — must not appear in cleanup candidates
    db.tasks.push({
      id: crypto.randomUUID(),
      backend_id: BACKEND,
      type: 'shell',
      kind: 'shell',
      source: 'mcp',
      profile: 'full',
      name: 'still-running',
      summary: 'active',
      status: 'queued',
      schedule_id: null,
      idempotency_key: null,
      request_hash: null,
      retry_of_task_id: null,
      created_at: new Date().toISOString(),
      updated_at: new Date().toISOString(),
      finished_at: null,
      terminal_at: null,
      expires_at: null,
      labels_json: JSON.stringify(labels()),
      environment: 'test',
      retention_class: 'test',
      deleted_at: null,
      deleted_by: null,
      deletion_reason: null,
      cleanup_state: 'none',
    });

    const tasks = service(db);
    const preview = await tasks.cleanupPreview(filters, { limit: 5000 });
    expect(preview.matched_count).toBe(3);
    expect(preview.deletable_count).toBe(3);
    expect(preview.status_breakdown).toEqual({
      succeeded: 1,
      failed: 1,
      cancelled: 1,
    });

    const run = await tasks.cleanupRun(filters, {
      mode: 'hard',
      reason: 'hard_accept',
      expectedMatchedCount: 3,
      limit: 5000,
      idempotencyKey: 'hard-run-1',
    });
    expect(run.deleted_count).toBe(3);
    expect(run.mode).toBe('hard');
    expect(run.idempotency?.replayed).toBe(false);

    // 4. list include_deleted + preview → 0 for terminal test suite
    const remaining = await tasks.list({
      backendId: BACKEND,
      includeDeleted: true,
      labels: { suite: 'hard-cleanup-accept', run_id: RUN },
      limit: 50,
    });
    // only the active queued task remains
    expect(remaining.every((t) => t.status === 'queued')).toBe(true);
    expect(remaining).toHaveLength(1);

    const after = await tasks.cleanupPreview(filters, { limit: 5000 });
    expect(after.matched_count).toBe(0);
    expect(after.deletable_count).toBe(0);

    // 10. active/queued still present (not wiped)
    expect(db.tasks.some((t) => t.status === 'queued')).toBe(true);
  });

  it('3: scope drift returns cleanup_scope_changed with zero deletes', async () => {
    const db = new FakeD1();
    seedTerminal(db, { status: 'succeeded', kind: 'command' });
    seedTerminal(db, { status: 'failed', kind: 'command' });
    const tasks = service(db);

    const preview = await tasks.cleanupPreview(filters);
    expect(preview.matched_count).toBe(2);

    // third terminal arrives after preview
    seedTerminal(db, { status: 'cancelled', kind: 'shell' });

    await expect(
      tasks.cleanupRun(filters, {
        mode: 'hard',
        expectedMatchedCount: 2,
        reason: 'drift-test',
        idempotencyKey: 'hard-drift-key',
      }),
    ).rejects.toMatchObject({
      code: 'cleanup_scope_changed',
      status: 409,
      details: { expected_matched_count: 2, actual_matched_count: 3 },
    });

    // zero hard deletes
    expect(db.tasks.filter((t) => !t.deleted_at).length).toBe(3);
    // failed cleanup must not pollute op-idempotency success
    await expect(
      tasks.cleanupRun(filters, {
        mode: 'hard',
        expectedMatchedCount: 3,
        reason: 'drift-test',
        idempotencyKey: 'hard-drift-key',
      }),
    ).resolves.toMatchObject({ deleted_count: 3, idempotency: { replayed: false } });
  });

  it('5–6: hard delete leaves create-idempotency tombstone; conflict on different body', async () => {
    const db = new FakeD1();
    const key = 'hard-tombstone-key';
    const hash = 'sha256:1111111111111111111111111111111111111111111111111111111111111111';
    const row = seedTerminal(db, {
      status: 'failed',
      kind: 'command',
      idempotency_key: key,
      request_hash: hash,
    });
    // Also seed create-idempotency as live create would
    db.createIdem.push({
      backend_id: BACKEND,
      idempotency_key: key,
      request_hash: hash,
      task_id: row.id,
      task_deleted: 0,
      original_status: 'failed',
      original_created_at: row.created_at,
      created_at: row.created_at,
      expires_at: new Date(Date.now() + 7 * 86_400_000).toISOString(),
    });

    const tasks = service(db);
    await tasks.cleanupRun(filters, {
      mode: 'hard',
      expectedMatchedCount: 1,
      reason: 'tombstone',
    });

    expect(db.tasks.find((t) => t.id === row.id)).toBeUndefined();
    const tomb = db.createIdem.find((t) => t.idempotency_key === key);
    expect(tomb?.task_deleted).toBe(1);
    expect(tomb?.task_id).toBe(row.id);

    const input: CreateTaskInput = {
      kind: 'command',
      backend_id: BACKEND,
      program: 'true',
      arguments: [],
      timeout_seconds: 30,
      working_directory: '/tmp',
      profile: 'full',
      labels: labels(),
      idempotency_key: key,
      output: {
        capture_stdout: true,
        capture_stderr: true,
        preview_max_bytes: 8192,
        retention_seconds: 86_400,
        hard_max_bytes: 10_485_760,
      },
    };

    // Same key + same request hash → resource_deleted replay (no new task)
    // Note: hashTaskRequest recomputes hash; seed hash must match real hash for replay.
    // So recompute via create: first ensure tombstone request_hash matches real hash.
    const realHash = await (await import('../src/tasks/task-service.js')).hashTaskRequest(input);
    tomb!.request_hash = realHash;

    const replayed = await tasks.create(input, 'mcp');
    expect(replayed.reusedExistingTask).toBe(true);
    expect(replayed.resourceDeleted).toBe(true);
    expect(replayed.id).toBe(row.id);
    expect(db.tasks.find((t) => t.idempotency_key === key)).toBeUndefined();

    // Different body → conflict
    await expect(
      tasks.create(
        {
          ...input,
          program: 'false',
        },
        'mcp',
      ),
    ).rejects.toMatchObject({ code: 'idempotency_conflict', status: 409 });
  });

  it('7: repeating hard cleanup idempotency_key returns replayed:true', async () => {
    const db = new FakeD1();
    seedTerminal(db, { status: 'succeeded', kind: 'command' });
    seedTerminal(db, { status: 'failed', kind: 'shell' });
    const tasks = service(db);
    const key = 'hard-cleanup-op-key';

    const first = await tasks.cleanupRun(filters, {
      mode: 'hard',
      expectedMatchedCount: 2,
      reason: 'hard_accept',
      idempotencyKey: key,
    });
    expect(first.idempotency?.replayed).toBe(false);
    expect(first.deleted_count).toBe(2);

    const second = await tasks.cleanupRun(filters, {
      mode: 'hard',
      expectedMatchedCount: 2,
      reason: 'hard_accept',
      idempotencyKey: key,
    });
    expect(second.idempotency?.replayed).toBe(true);
    expect(second.deleted_count).toBe(2);
    // no extra mutations — already empty
    expect(db.tasks.filter((t) => t.cleanup_state === 'eligible')).toHaveLength(0);
  });

  it('9: control-plane has no orphan task rows; only intentional tombstones remain', async () => {
    const db = new FakeD1();
    const a = seedTerminal(db, {
      status: 'succeeded',
      kind: 'command',
      idempotency_key: 'orphan-a',
      request_hash: 'sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',
    });
    const b = seedTerminal(db, {
      status: 'failed',
      kind: 'shell',
      idempotency_key: 'orphan-b',
      request_hash: 'sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',
    });
    const tasks = service(db);
    await tasks.cleanupRun(filters, { mode: 'hard', expectedMatchedCount: 2 });

    expect(db.tasks.find((t) => t.id === a.id || t.id === b.id)).toBeUndefined();
    // intentional create-idempotency tombstones only
    expect(db.createIdem).toHaveLength(2);
    expect(db.createIdem.every((t) => t.task_deleted === 1)).toBe(true);
  });

  it('rejects active tasks via single delete (task_not_terminal / conflict)', async () => {
    const db = new FakeD1();
    const id = crypto.randomUUID();
    db.tasks.push({
      id,
      backend_id: BACKEND,
      type: 'shell',
      kind: 'command',
      source: 'mcp',
      profile: 'full',
      name: null,
      summary: 'active',
      status: 'running',
      schedule_id: null,
      idempotency_key: null,
      request_hash: null,
      retry_of_task_id: null,
      created_at: new Date().toISOString(),
      updated_at: new Date().toISOString(),
      finished_at: null,
      terminal_at: null,
      expires_at: null,
      labels_json: null,
      environment: null,
      retention_class: null,
      deleted_at: null,
      deleted_by: null,
      deletion_reason: null,
      cleanup_state: 'none',
    });
    const tasks = service(db);
    await expect(tasks.delete(id, { mode: 'hard' })).rejects.toBeInstanceOf(AppError);
    await expect(tasks.delete(id, { mode: 'hard' })).rejects.toMatchObject({
      code: 'task_not_terminal',
      status: 409,
    });
  });
});
