import { describe, expect, it } from 'vitest';

import {
  nextCronRunAfter,
  nextCronRunAtIso,
} from '../src/cron-next.js';
import {
  DEFAULT_SCHEDULE_POLICY,
  DST_POLICY_SPEC,
  MAX_SCHEDULE_ADVANCE_STEPS,
  MISFIRE_POLICY_SPEC,
  REVISION_MERGE_SPEC,
  scheduleOccurrenceId,
} from '../src/schedule-semantics.js';

describe('frozen schedule semantics exports', () => {
  it('defaults and caps are stable', () => {
    expect(DEFAULT_SCHEDULE_POLICY.misfire).toBe('run_once');
    expect(DEFAULT_SCHEDULE_POLICY.max_catchup_runs).toBe(1);
    expect(MAX_SCHEDULE_ADVANCE_STEPS).toBe(32);
    expect(DST_POLICY_SPEC.gap).toBe('skip_missing_local_time');
    expect(DST_POLICY_SPEC.overlap).toBe('fire_each_matching_utc_instant');
    expect(REVISION_MERGE_SPEC.same_rev).toBe('keep_later_next_cursor');
    expect(MISFIRE_POLICY_SPEC.run_once.length).toBeGreaterThan(10);
  });

  it('occurrence id uses ms epoch', () => {
    const ms = Date.parse('2026-07-31T01:00:00.000Z');
    expect(scheduleOccurrenceId('sch', 3, ms)).toBe(`sch:3:${ms}`);
  });
});

describe('DST gap (spring forward)', () => {
  it('skips non-existent America/Los_Angeles 02:00 on 2024-03-10', () => {
    // After 2024-03-09 02:00 PST (10:00 UTC). Next 02:00 wall should not be
    // 2024-03-10 02:00 (gap); clocks jump 02:00 → 03:00.
    const after = new Date('2024-03-09T10:00:00.000Z'); // 02:00 PST
    const next = nextCronRunAfter('0 2 * * *', 'America/Los_Angeles', after);
    expect(next).toBeDefined();
    // 2024-03-11 02:00 PDT = 09:00 UTC (day after spring-forward)
    expect(next!.toISOString()).toBe('2024-03-11T09:00:00.000Z');
  });
});

describe('DST overlap (fall back)', () => {
  it('first match after midnight is earlier offset 01:30', () => {
    // 2024-11-03 LA: 01:30 occurs twice (PDT then PST).
    const after = new Date('2024-11-03T07:00:00.000Z'); // 00:00 PDT
    const first = nextCronRunAtIso('30 1 * * *', 'America/Los_Angeles', after);
    expect(first).toBeDefined();
    // First 01:30 PDT = 08:30 UTC
    expect(first).toBe('2024-11-03T08:30:00.000Z');

    // Second walk from first → second 01:30 PST = 09:30 UTC
    const second = nextCronRunAtIso(
      '30 1 * * *',
      'America/Los_Angeles',
      new Date(first!),
    );
    expect(second).toBe('2024-11-03T09:30:00.000Z');
  });
});
