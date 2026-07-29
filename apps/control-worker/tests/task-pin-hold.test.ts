import { describe, expect, it, vi } from 'vitest';

import { AppError } from '../src/lib/http.js';
import { TaskService } from '../src/tasks/task-service.js';
import { FakeD1, asD1, type FakeRow } from './fake-d1.js';

function seed(db: FakeD1, status = 'succeeded'): FakeRow {
  const now = new Date().toISOString();
  const row: FakeRow = {
    id: crypto.randomUUID(),
    backend_id: 'backend-1',
    type: 'shell',
    kind: 'command',
    source: 'mcp',
    profile: 'full',
    name: null,
    summary: status,
    status,
    schedule_id: null,
    idempotency_key: null,
    request_hash: null,
    retry_of_task_id: null,
    created_at: now,
    updated_at: now,
    finished_at: now,
    terminal_at: now,
    expires_at: new Date(Date.now() - 86_400_000).toISOString(),
    labels_json: null,
    environment: 'production',
    retention_class: 'success',
    deleted_at: null,
    deleted_by: null,
    deletion_reason: null,
    cleanup_state: 'eligible',
    legal_hold: 0,
    pinned_at: null,
    pinned_by: null,
    legal_hold_reason: null,
    legal_hold_at: null,
    legal_hold_by: null,
    output_expires_at: null,
  };
  db.tasks.push(row);
  return row;
}

function svc(db: FakeD1) {
  return new TaskService(asD1(db), { get: vi.fn() } as never, {} as never);
}

describe('task pin / legal hold', () => {
  it('pins and unpins; unpin refreshes expires_at for terminal tasks', async () => {
    const db = new FakeD1();
    const row = seed(db);
    const tasks = svc(db);

    const pinned = await tasks.pin(String(row.id), { pinnedBy: 'ops' });
    expect(pinned.pinned).toBe(true);
    expect(pinned.already_pinned).toBe(false);
    expect(pinned.task.pinned).toBe(true);
    expect(pinned.task.pinnedBy).toBe('ops');

    const again = await tasks.pin(String(row.id));
    expect(again.already_pinned).toBe(true);

    const unpinned = await tasks.unpin(String(row.id));
    expect(unpinned.pinned).toBe(false);
    expect(unpinned.task.pinned).toBe(false);
    expect(unpinned.task.expiresAt).toBeTruthy();
  });

  it('legal hold blocks delete; clear allows delete again', async () => {
    const db = new FakeD1();
    const row = seed(db, 'failed');
    const tasks = svc(db);

    const held = await tasks.setLegalHold(String(row.id), {
      reason: 'incident-review',
      heldBy: 'admin',
    });
    expect(held.legal_hold).toBe(true);
    expect(held.task.legalHold).toBe(true);
    expect(held.task.legalHoldReason).toBe('incident-review');

    await expect(tasks.delete(String(row.id), { mode: 'soft' })).rejects.toMatchObject({
      code: 'task_legal_hold',
      status: 403,
    });

    // Not in cleanup candidates while held
    const previewHeld = await tasks.cleanupPreview({ expiredOnly: true }, { limit: 100 });
    expect(previewHeld.sample_task_ids.includes(String(row.id))).toBe(false);

    await tasks.clearLegalHold(String(row.id));
    const del = await tasks.delete(String(row.id), { mode: 'soft' });
    expect(del.deleted).toBe(true);
  });

  it('maps task_legal_hold to permission category', async () => {
    const { categoryFor } = await import('../src/mcp/schema/envelope.js');
    expect(categoryFor('task_legal_hold')).toBe('permission');
    expect(new AppError('task_legal_hold', 'held', 403).status).toBe(403);
  });
});
