/**
 * Control-plane schedule e2e fixture tests.
 *
 * Drives the logical CP↔backend schedule path with FakeScheduleD1 + mocked
 * BackendClient — no live Worker / tunnel required.
 */
import { describe, expect, it, vi } from 'vitest';

import { authoritativeNextAfterOccurrence, canonicalUtcIso } from '@vacps/contracts';

import { ScheduleService } from '../src/schedules/schedule-service.js';
import { asD1, FakeScheduleD1, seedSchedule } from './fake-schedule-d1.js';
import {
  FIXTURE_BACKEND_ID,
  FIXTURE_SCHEDULE_ID,
  FIXTURE_SHANGHAI_0900_UTC,
  fixtureOccurrenceAck,
  fixtureSchedulerPutBody,
  fixtureTaskTemplate,
  SCHEDULE_E2E_STEPS,
} from './fixtures/schedule-control-plane.js';

function makeService(db: FakeScheduleD1) {
  const upsertScheduler = vi.fn().mockResolvedValue(undefined);
  const backends = {
    get: vi.fn().mockResolvedValue({
      id: FIXTURE_BACKEND_ID,
      name: 'e2e',
      baseUrl: 'https://e2e-agent.example',
      enabled: true,
    }),
  };
  const service = new ScheduleService(
    asD1(db),
    backends as never,
    { upsertScheduler } as never,
    {} as never,
  );
  return { service, upsertScheduler };
}

describe('schedule control-plane e2e fixtures', () => {
  it('exports a stable step checklist', () => {
    expect(SCHEDULE_E2E_STEPS).toContain('cp_cas_advances_authoritative_next');
    expect(fixtureSchedulerPutBody().next_run_at).toBe(FIXTURE_SHANGHAI_0900_UTC);
    expect(fixtureTaskTemplate.kind).toBe('command');
  });

  it('cp_sync_puts_absolute_next_run_at: wire body has revision + next_run_at', () => {
    const body = fixtureSchedulerPutBody({ revision: 2 });
    expect(body.revision).toBe(2);
    expect(body.next_run_at).toMatch(/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/);
    expect(body.timezone).toBe('Asia/Shanghai');
    // Shanghai 09:00 → 01:00 UTC
    expect(body.next_run_at).toBe('2026-07-31T01:00:00.000Z');
  });

  it('happy path: claim ack → cas_applied → resync with later next', async () => {
    const db = new FakeScheduleD1();
    const put = fixtureSchedulerPutBody({ revision: 1 });
    seedSchedule(db, {
      id: FIXTURE_SCHEDULE_ID,
      backend_id: FIXTURE_BACKEND_ID,
      cron: put.cron,
      timezone: put.timezone,
      revision: put.revision,
      next_run_at: put.next_run_at,
      task_json: JSON.stringify(put.task),
      policy_json: JSON.stringify(put.policy),
    });
    const { service, upsertScheduler } = makeService(db);

    const ack = fixtureOccurrenceAck({
      revision: 1,
      scheduled_for: put.next_run_at,
      // daily 09:00 Shanghai → next day 01:00 UTC
      locally_advanced_to: '2026-08-01T01:00:00.000Z',
      enqueued_count: 1,
    });

    const result = await service.ackOccurrence(ack);

    expect(result.accepted).toBe(true);
    expect(result.status).toBe('cas_applied');
    expect(result.next_run_at).toBeTruthy();
    expect(Date.parse(result.next_run_at!)).toBeGreaterThan(Date.parse(put.next_run_at));

    // CP is authority: next should match helper (run_once from scheduled_for past backlog)
    const expected = authoritativeNextAfterOccurrence(
      put.cron,
      put.timezone,
      put.next_run_at,
      new Date(),
      'run_once',
      1,
    );
    expect(result.next_run_at).toBe(expected);

    expect(upsertScheduler).toHaveBeenCalled();
    // upsertScheduler(backend, scheduleId, body)
    const syncBody = upsertScheduler.mock.calls[0]![2] as {
      revision: number;
      next_run_at: string;
    };
    expect(syncBody.revision).toBe(1);
    expect(syncBody.next_run_at).toBe(result.next_run_at);
  });

  it('stale_ack_is_already_advanced after first ack', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: FIXTURE_SCHEDULE_ID,
      backend_id: FIXTURE_BACKEND_ID,
      cron: '0 9 * * *',
      timezone: 'Asia/Shanghai',
      revision: 1,
      next_run_at: FIXTURE_SHANGHAI_0900_UTC,
      task_json: JSON.stringify(fixtureTaskTemplate),
    });
    const { service, upsertScheduler } = makeService(db);

    const ack = fixtureOccurrenceAck({
      scheduled_for: FIXTURE_SHANGHAI_0900_UTC,
      enqueued_count: 1,
    });
    const first = await service.ackOccurrence(ack);
    expect(first.status).toBe('cas_applied');
    upsertScheduler.mockClear();

    const second = await service.ackOccurrence(ack);
    expect(second.status).toBe('already_advanced');
    expect(second.accepted).toBe(true);
    expect(second.next_run_at).toBe(first.next_run_at);
    expect(upsertScheduler).not.toHaveBeenCalled();
  });

  it('old_revision_ack_is_ignored after config bump', async () => {
    const db = new FakeScheduleD1();
    seedSchedule(db, {
      id: FIXTURE_SCHEDULE_ID,
      backend_id: FIXTURE_BACKEND_ID,
      revision: 2,
      next_run_at: '2026-07-31T10:00:00.000Z', // new earlier/later config
      task_json: JSON.stringify(fixtureTaskTemplate),
    });
    const { service } = makeService(db);

    const stale = fixtureOccurrenceAck({
      revision: 1,
      scheduled_for: FIXTURE_SHANGHAI_0900_UTC,
    });
    const result = await service.ackOccurrence(stale);
    expect(result.status).toBe('revision_mismatch');
    expect(result.accepted).toBe(false);
    expect(db.schedules[0]!.next_run_at).toBe('2026-07-31T10:00:00.000Z');
  });

  it('same-revision later cursor survives conceptual re-sync merge rule', () => {
    // Documents native merge: same rev keeps later next (local after fire vs CP lag).
    const localNext = '2026-08-01T01:00:00.000Z';
    const incomingStale = FIXTURE_SHANGHAI_0900_UTC;
    const later = Math.max(Date.parse(localNext), Date.parse(incomingStale));
    expect(canonicalUtcIso(later)).toBe(localNext);
  });

  it('Shanghai wall clock fixture stays stable (IANA projection)', () => {
    // 2026-07-31 09:00 CST = 01:00 UTC year-round (no DST in Asia/Shanghai)
    expect(FIXTURE_SHANGHAI_0900_UTC).toBe('2026-07-31T01:00:00.000Z');
    const put = fixtureSchedulerPutBody();
    expect(put.cron).toBe('0 9 * * *');
    expect(put.timezone).toBe('Asia/Shanghai');
  });
});
