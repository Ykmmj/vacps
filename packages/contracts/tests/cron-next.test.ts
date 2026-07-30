import { describe, expect, it } from 'vitest';

import {
  authoritativeNextAfterOccurrence,
  canonicalUtcIso,
  laterUtcIso,
  nextCronRunAfter,
  nextCronRunAtIso,
} from '../src/cron-next.js';

describe('nextCronRunAfter', () => {
  it('finds next UTC minute match', () => {
    // 2024-01-15 12:00:30 UTC → next 12:30
    const after = new Date(Date.UTC(2024, 0, 15, 12, 0, 30));
    const next = nextCronRunAfter('30 12 * * *', 'UTC', after);
    expect(next?.toISOString()).toBe('2024-01-15T12:30:00.000Z');
  });

  it('skips current minute', () => {
    const after = new Date(Date.UTC(2024, 0, 15, 12, 30, 0));
    const next = nextCronRunAfter('30 12 * * *', 'UTC', after);
    expect(next?.toISOString()).toBe('2024-01-16T12:30:00.000Z');
  });

  it('respects Asia/Shanghai wall clock when Intl available', () => {
    // Shanghai = UTC+8. Local 09:00 → 01:00 UTC.
    const after = new Date(Date.UTC(2024, 5, 1, 0, 0, 0)); // before 01:00 UTC that day
    const next = nextCronRunAfter('0 9 * * *', 'Asia/Shanghai', after);
    expect(next).toBeDefined();
    // 2024-06-01 09:00 CST = 2024-06-01 01:00 UTC
    expect(next!.toISOString()).toBe('2024-06-01T01:00:00.000Z');
  });

  it('returns ISO helper', () => {
    const after = new Date(Date.UTC(2024, 0, 15, 0, 0, 0));
    const iso = nextCronRunAtIso('0 0 * * *', 'UTC', after);
    expect(iso).toBe('2024-01-16T00:00:00.000Z');
  });

  it('rejects bad cron', () => {
    expect(nextCronRunAfter('* *', 'UTC', new Date())).toBeUndefined();
  });
});

describe('canonicalUtcIso / laterUtcIso', () => {
  it('normalizes Z without millis', () => {
    expect(canonicalUtcIso('2026-07-31T01:00:00Z')).toBe('2026-07-31T01:00:00.000Z');
  });

  it('laterUtcIso compares by epoch not string', () => {
    expect(laterUtcIso('2026-07-31T01:00:00Z', '2026-07-31T02:00:00.000Z')).toBe(
      '2026-07-31T02:00:00.000Z',
    );
  });

  it('laterUtcIso keeps local when incoming empty', () => {
    expect(laterUtcIso('2026-07-31T01:00:00.000Z', null)).toBe('2026-07-31T01:00:00.000Z');
  });
});

describe('authoritativeNextAfterOccurrence', () => {
  it('run_once jumps past backlog from scheduled_for', () => {
    const next = authoritativeNextAfterOccurrence(
      '0 * * * *',
      'UTC',
      '2024-06-01T09:00:00.000Z',
      new Date('2024-06-01T13:30:00.000Z'),
      'run_once',
    );
    expect(next).toBe('2024-06-01T14:00:00.000Z');
  });

  it('catch_up advances one step only', () => {
    const next = authoritativeNextAfterOccurrence(
      '0 * * * *',
      'UTC',
      '2024-06-01T09:00:00.000Z',
      new Date('2024-06-01T13:30:00.000Z'),
      'catch_up',
    );
    expect(next).toBe('2024-06-01T10:00:00.000Z');
  });

  it('catch_up honors enqueued_count steps', () => {
    const next = authoritativeNextAfterOccurrence(
      '0 * * * *',
      'UTC',
      '2024-06-01T09:00:00.000Z',
      new Date('2024-06-01T13:30:00.000Z'),
      'catch_up',
      2,
    );
    expect(next).toBe('2024-06-01T11:00:00.000Z');
  });
});
