import { describe, expect, it, vi } from 'vitest';

import { ScheduleService } from '../src/schedules/schedule-service.js';
import { asD1, FakeScheduleD1, seedSchedule } from './fake-schedule-d1.js';

const SCHEDULE_ID = '11111111-1111-4111-8111-111111111111';
const BACKEND_ID = 'backend-1';

function makeService(db: FakeScheduleD1) {
  const upsertScheduler = vi.fn().mockResolvedValue(undefined);
  const backends = {
    get: vi.fn().mockResolvedValue({
      id: BACKEND_ID,
      name: 'b1',
      baseUrl: 'https://agent.example',
      enabled: true,
    }),
  };
  const client = { upsertScheduler };
  const tasks = {} as never;
  const service = new ScheduleService(asD1(db), backends as never, client as never, tasks);
  return { service, upsertScheduler, backends };
}

describe('ScheduleService.ackOccurrence', () => {
  it('CAS-applies authoritative next and re-syncs backend', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: SCHEDULE_ID,
      backend_id: BACKEND_ID,
      cron: '0 * * * *',
      timezone: 'UTC',
      revision: 3,
      next_run_at: '2024-06-01T09:00:00.000Z',
    });
    const { service, upsertScheduler } = makeService(db);

    const result = await service.ackOccurrence({
      backend_id: BACKEND_ID,
      schedule_id: SCHEDULE_ID,
      revision: 3,
      scheduled_for: '2024-06-01T09:00:00.000Z',
      enqueued_count: 1,
      occurrence_id: `${SCHEDULE_ID}:3:${Date.parse('2024-06-01T09:00:00.000Z')}`,
      locally_advanced_to: '2024-06-01T10:00:00.000Z',
    });

    expect(result.accepted).toBe(true);
    expect(result.status).toBe('cas_applied');
    expect(result.revision).toBe(3);
    // From 09:00 with run_once and now ≈ real wall clock (years later) → jumps far
    expect(result.next_run_at).toBeTruthy();
    expect(Date.parse(result.next_run_at!)).toBeGreaterThan(
      Date.parse('2024-06-01T09:00:00.000Z'),
    );
    expect(db.schedules[0]!.next_run_at).toBe(result.next_run_at);
    expect(db.schedules[0]!.last_run_at).toBeTruthy();
    expect(upsertScheduler).toHaveBeenCalledTimes(1);
    // upsertScheduler(backend, scheduleId, body)
    const wire = upsertScheduler.mock.calls[0]![2] as {
      revision: number;
      next_run_at: string;
    };
    expect(wire).toMatchObject({
      revision: 3,
      next_run_at: result.next_run_at,
    });
  });

  it('treats re-ack after advance as already_advanced (idempotent)', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: SCHEDULE_ID,
      backend_id: BACKEND_ID,
      revision: 1,
      next_run_at: '2024-06-01T10:00:00.000Z', // already past 09:00
    });
    const { service, upsertScheduler } = makeService(db);

    const result = await service.ackOccurrence({
      backend_id: BACKEND_ID,
      schedule_id: SCHEDULE_ID,
      revision: 1,
      scheduled_for: '2024-06-01T09:00:00.000Z',
      enqueued_count: 1,
    });

    expect(result).toMatchObject({
      accepted: true,
      status: 'already_advanced',
      next_run_at: '2024-06-01T10:00:00.000Z',
    });
    expect(upsertScheduler).not.toHaveBeenCalled();
  });

  it('rejects revision mismatch without moving cursor', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: SCHEDULE_ID,
      backend_id: BACKEND_ID,
      revision: 5,
      next_run_at: '2024-06-01T09:00:00.000Z',
    });
    const { service } = makeService(db);

    const result = await service.ackOccurrence({
      backend_id: BACKEND_ID,
      schedule_id: SCHEDULE_ID,
      revision: 4,
      scheduled_for: '2024-06-01T09:00:00.000Z',
      enqueued_count: 1,
    });

    expect(result).toMatchObject({
      accepted: false,
      status: 'revision_mismatch',
      revision: 5,
      next_run_at: '2024-06-01T09:00:00.000Z',
    });
    expect(db.schedules[0]!.next_run_at).toBe('2024-06-01T09:00:00.000Z');
  });

  it('rejects disabled schedules', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: SCHEDULE_ID,
      backend_id: BACKEND_ID,
      enabled: 0,
      revision: 1,
      next_run_at: '2024-06-01T09:00:00.000Z',
    });
    const { service } = makeService(db);

    const result = await service.ackOccurrence({
      backend_id: BACKEND_ID,
      schedule_id: SCHEDULE_ID,
      revision: 1,
      scheduled_for: '2024-06-01T09:00:00.000Z',
    });

    expect(result.status).toBe('schedule_disabled');
    expect(result.accepted).toBe(false);
  });

  it('rejects cursor_mismatch when next is unrelated', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: SCHEDULE_ID,
      backend_id: BACKEND_ID,
      revision: 1,
      // Different slot, not later than scheduled_for (same time different day earlier? use earlier)
      next_run_at: '2024-05-01T00:00:00.000Z',
    });
    const { service } = makeService(db);

    const result = await service.ackOccurrence({
      backend_id: BACKEND_ID,
      schedule_id: SCHEDULE_ID,
      revision: 1,
      scheduled_for: '2024-06-01T09:00:00.000Z',
      enqueued_count: 1,
    });

    expect(result.status).toBe('cursor_mismatch');
    expect(result.accepted).toBe(false);
  });

  it('rejects backend_id that does not own the schedule', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: SCHEDULE_ID,
      backend_id: BACKEND_ID,
      revision: 1,
      next_run_at: '2024-06-01T09:00:00.000Z',
    });
    const { service } = makeService(db);

    await expect(
      service.ackOccurrence({
        backend_id: 'other-backend',
        schedule_id: SCHEDULE_ID,
        revision: 1,
        scheduled_for: '2024-06-01T09:00:00.000Z',
      }),
    ).rejects.toMatchObject({ code: 'backend_identity_mismatch', status: 403 });
  });

  it('flags local_advance_drift when backend advance differs from CP next', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: SCHEDULE_ID,
      backend_id: BACKEND_ID,
      cron: '0 * * * *',
      timezone: 'UTC',
      revision: 1,
      next_run_at: '2024-06-01T09:00:00.000Z',
      policy_json: JSON.stringify({
        concurrency: 'forbid',
        misfire: 'catch_up',
        max_catchup_runs: 2,
      }),
    });
    const { service } = makeService(db);

    const result = await service.ackOccurrence({
      backend_id: BACKEND_ID,
      schedule_id: SCHEDULE_ID,
      revision: 1,
      scheduled_for: '2024-06-01T09:00:00.000Z',
      enqueued_count: 2,
      // Intentional wrong local advance for drift signal
      locally_advanced_to: '2099-01-01T00:00:00.000Z',
    });

    expect(result.accepted).toBe(true);
    expect(result.status).toBe('cas_applied');
    // catch_up 2 steps from 09:00 → 11:00
    expect(result.next_run_at).toBe('2024-06-01T11:00:00.000Z');
    expect(result.local_advance_drift).toBe(true);
  });

  it('accepts non-canonical scheduled_for when epoch matches cursor', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: SCHEDULE_ID,
      backend_id: BACKEND_ID,
      revision: 1,
      next_run_at: '2024-06-01T09:00:00.000Z',
    });
    const { service } = makeService(db);

    const result = await service.ackOccurrence({
      backend_id: BACKEND_ID,
      schedule_id: SCHEDULE_ID,
      revision: 1,
      // no millis — still same instant
      scheduled_for: '2024-06-01T09:00:00.000Z',
      enqueued_count: 1,
    });

    expect(result.accepted).toBe(true);
    expect(result.status).toBe('cas_applied');
  });
});
