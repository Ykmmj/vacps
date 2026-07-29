import { describe, expect, it } from 'vitest';

import {
  RETENTION_DAYS,
  computeExpiresAt,
  environmentFromLabels,
  isTerminalTaskStatus,
  isTestTask,
  parseLabelsJson,
  retentionClassFor,
  retentionDaysFor,
  scopeCountAcceptable,
} from '../src/tasks/retention.js';

describe('isTerminalTaskStatus', () => {
  it('marks finished statuses terminal', () => {
    for (const s of ['succeeded', 'failed', 'cancelled', 'timed_out', 'dispatch_failed'] as const) {
      expect(isTerminalTaskStatus(s)).toBe(true);
    }
  });

  it('marks active statuses non-terminal', () => {
    for (const s of ['created', 'dispatching', 'queued', 'running', 'waiting_for_approval']) {
      expect(isTerminalTaskStatus(s)).toBe(false);
    }
  });
});

describe('test task detection', () => {
  it('detects environment=test', () => {
    expect(isTestTask({ environment: 'test' }, null)).toBe(true);
    expect(isTestTask({}, 'test')).toBe(true);
  });

  it('detects suite / purpose labels', () => {
    expect(isTestTask({ suite: 'mcp-regression' }, null)).toBe(true);
    expect(isTestTask({ purpose: 'acceptance-test' }, null)).toBe(true);
    expect(isTestTask({ purpose: 'regression' }, null)).toBe(true);
  });

  it('does not flag production work', () => {
    expect(isTestTask({ environment: 'production' }, 'production')).toBe(false);
    expect(isTestTask(null, null)).toBe(false);
  });
});

describe('retention policy', () => {
  it('uses short TTL for test class', () => {
    expect(retentionDaysFor('test', 'succeeded')).toBe(RETENTION_DAYS.test);
    expect(retentionClassFor('succeeded', { environment: 'test' }, null)).toBe('test');
  });

  it('keeps failures longer than successes', () => {
    expect(retentionDaysFor('failure', 'failed')).toBe(RETENTION_DAYS.failed);
    expect(retentionDaysFor('success', 'succeeded')).toBe(RETENTION_DAYS.succeeded);
    expect(RETENTION_DAYS.failed).toBeGreaterThan(RETENTION_DAYS.succeeded);
  });

  it('computes expires_at from terminal_at', () => {
    const terminalAt = '2026-07-01T00:00:00.000Z';
    const expires = computeExpiresAt(terminalAt, 'test', 'succeeded');
    expect(Date.parse(expires) - Date.parse(terminalAt)).toBe(RETENTION_DAYS.test * 86_400_000);
  });

  it('extracts environment from labels', () => {
    expect(environmentFromLabels({ environment: 'staging' })).toBe('staging');
    expect(environmentFromLabels({})).toBeNull();
  });
});

describe('parseLabelsJson', () => {
  it('parses object maps', () => {
    expect(parseLabelsJson('{"suite":"a","n":1}')).toEqual({ suite: 'a' });
  });

  it('tolerates invalid JSON', () => {
    expect(parseLabelsJson('not-json')).toEqual({});
    expect(parseLabelsJson(null)).toEqual({});
  });
});

describe('cleanup scope guard', () => {
  it('requires exact match only', () => {
    expect(scopeCountAcceptable(2, 2)).toBe(true);
    expect(scopeCountAcceptable(0, 0)).toBe(true);
    expect(scopeCountAcceptable(2, 3)).toBe(false);
    expect(scopeCountAcceptable(100, 104)).toBe(false);
    expect(scopeCountAcceptable(100, 95)).toBe(false);
  });
});

describe('idempotency TTL', () => {
  it('uses shorter TTL for test tasks', async () => {
    const { IDEMPOTENCY_TTL_DAYS, computeIdempotencyExpiresAt } =
      await import('../src/tasks/retention.js');
    const from = '2026-07-01T00:00:00.000Z';
    const testExp = computeIdempotencyExpiresAt(from, { environment: 'test' }, null);
    const prodExp = computeIdempotencyExpiresAt(from, { environment: 'production' }, 'production');
    expect(Date.parse(testExp) - Date.parse(from)).toBe(IDEMPOTENCY_TTL_DAYS.test * 86_400_000);
    expect(Date.parse(prodExp) - Date.parse(from)).toBe(IDEMPOTENCY_TTL_DAYS.default * 86_400_000);
  });
});
