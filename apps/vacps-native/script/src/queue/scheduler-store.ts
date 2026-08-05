import type { CreateTaskInput, SchedulePolicy } from '@vacps/contracts';
import type { Store } from 'vacps:store';

import { migrateAgentDb } from '../storage/schema';
import {
  DEFAULT_SCHEDULE_POLICY,
  mergeSchedulerWire,
  parseSchedulePolicy,
  planMisfire,
  occurrenceId,
  canonicalUtcIso,
  type MisfirePlan,
} from './schedule-logic';

export interface StoredScheduler {
  id: string;
  cron: string;
  timezone: string;
  enabled: boolean;
  task: CreateTaskInput;
  revision: number;
  policy: SchedulePolicy;
  /** Absolute UTC ISO for next fire (raw from DB; prefer canonical writes). */
  nextRunAt?: string;
  lastClaimedAt?: string;
  /** Observability only — not used for correctness. */
  lastFiredMinute?: string;
  updatedAt: string;
}

export interface ClaimEnqueueSlot {
  occurrenceId: string;
  scheduledForIso: string;
  scheduledForMs: number;
  revision: number;
}

export interface ClaimResult {
  claimed: boolean;
  /** CAS missed or not due. */
  reason?: 'not_due' | 'cas_miss' | 'disabled' | 'no_cursor';
  plan?: MisfirePlan;
  slots: ClaimEnqueueSlot[];
  advancedNext: string | null;
}

/**
 * Local schedule store. Firing uses absolute next_run_at; revision-gated merge on sync.
 */
export class SchedulerStore {
  private constructor(private readonly db: Store) {}

  static async create(db: Store): Promise<SchedulerStore> {
    await migrateAgentDb(db);
    return new SchedulerStore(db);
  }

  /**
   * Apply control-plane wire with revision merge rules.
   * Returns whether local row was written.
   */
  async upsertFromWire(input: {
    id: string;
    cron: string;
    timezone: string;
    enabled: boolean;
    task: CreateTaskInput;
    revision?: number;
    policy?: SchedulePolicy;
    nextRunAt?: string | null;
    /** If higher/new rev has no next and enabled, fill with this (caller-computed). */
    computeNextIfMissing?: () => string | undefined;
  }): Promise<boolean> {
    const local = await this.get(input.id);
    const incomingRev = input.revision ?? local?.revision ?? 1;
    const policy = input.policy ?? local?.policy ?? DEFAULT_SCHEDULE_POLICY;

    const incoming: {
      revision: number;
      cron: string;
      timezone: string;
      enabled: boolean;
      task: unknown;
      policy: SchedulePolicy;
      nextRunAt?: string | null;
    } = {
      revision: incomingRev,
      cron: input.cron,
      timezone: input.timezone,
      enabled: input.enabled,
      task: input.task,
      policy,
    };
    if (input.nextRunAt !== undefined) {
      incoming.nextRunAt = input.nextRunAt;
    }

    const merged = mergeSchedulerWire(
      local
        ? {
            revision: local.revision,
            cron: local.cron,
            timezone: local.timezone,
            enabled: local.enabled,
            task: local.task,
            policy: local.policy,
            ...(local.nextRunAt ? { nextRunAt: local.nextRunAt } : {}),
          }
        : undefined,
      incoming,
    );

    if (merged.action === 'ignore') return false;

    let nextRunAt = merged.nextRunAt;
    if (merged.enabled && !nextRunAt && input.computeNextIfMissing) {
      nextRunAt = input.computeNextIfMissing();
    }
    if (!merged.enabled) nextRunAt = undefined;
    if (nextRunAt) nextRunAt = canonicalUtcIso(nextRunAt);

    await this.writeRow({
      id: input.id,
      cron: merged.cron,
      timezone: merged.timezone,
      enabled: merged.enabled,
      task: merged.task as CreateTaskInput,
      revision: merged.revision,
      policy: merged.policy,
      ...(nextRunAt ? { nextRunAt } : {}),
    });
    return true;
  }

  /** Unconditional local write (tests / internal). */
  async writeRow(input: {
    id: string;
    cron: string;
    timezone: string;
    enabled: boolean;
    task: CreateTaskInput;
    revision: number;
    policy: SchedulePolicy;
    nextRunAt?: string;
  }): Promise<void> {
    const now = new Date().toISOString();
    const next = input.nextRunAt ? (canonicalUtcIso(input.nextRunAt) ?? null) : null;
    await this.db.run(
      `INSERT INTO schedulers(
         id, cron, timezone, enabled, task_json, last_fired_minute,
         next_run_at, revision, last_claimed_at, policy_json, updated_at
       ) VALUES(?, ?, ?, ?, ?, NULL, ?, ?, NULL, ?, ?)
       ON CONFLICT(id) DO UPDATE SET
         cron = excluded.cron,
         timezone = excluded.timezone,
         enabled = excluded.enabled,
         task_json = excluded.task_json,
         next_run_at = excluded.next_run_at,
         revision = excluded.revision,
         policy_json = excluded.policy_json,
         updated_at = excluded.updated_at;`,
      [
        input.id,
        input.cron,
        input.timezone,
        input.enabled ? 1 : 0,
        JSON.stringify(input.task),
        next,
        input.revision,
        JSON.stringify(input.policy),
        now,
      ],
    );
  }

  async remove(id: string): Promise<void> {
    await this.db.run('DELETE FROM schedulers WHERE id = ?;', [id]);
  }

  async list(): Promise<StoredScheduler[]> {
    const rows = await this.db.query('SELECT * FROM schedulers ORDER BY id ASC;');
    return rows.map(rowToScheduler);
  }

  async get(id: string): Promise<StoredScheduler | undefined> {
    const rows = await this.db.query('SELECT * FROM schedulers WHERE id = ?;', [id]);
    if (rows.length === 0) return undefined;
    return rowToScheduler(rows[0]!);
  }

  /**
   * Enabled schedules whose absolute next_run_at is due (SQL-bounded, earliest first).
   * Rows without next_run_at are out of contract and never selected.
   */
  async listDue(nowMs: number, limit = 128): Promise<StoredScheduler[]> {
    const nowIso = canonicalUtcIso(nowMs) ?? new Date(nowMs).toISOString();
    const rows = await this.db.query(
      `SELECT * FROM schedulers
       WHERE enabled = 1
         AND next_run_at IS NOT NULL
         AND next_run_at <= ?
       ORDER BY next_run_at ASC
       LIMIT ?;`,
      [nowIso, limit],
    );
    return rows.map(rowToScheduler);
  }

  /**
   * CAS claim of current next_run_at + conditional occurrence inserts in **one**
   * `store.transaction()` unit (BEGIN … UPDATE … INSERTs … COMMIT on db_pool).
   *
   * Cursor CAS advances whenever id/revision/enabled/next_run_at match, even when
   * concurrency=forbid suppresses every insert (enqueued_count stays 0).
   * Each INSERT is gated by: concurrency='queue' OR no active (queued|running)
   * task for this schedule_id — so queue inserts all planned slots and forbid
   * inserts only when no earlier task for the schedule is active.
   *
   * `beforeInsert` is optional and must not touch the DB — tests use it to throw
   * before the transaction to simulate insert failure / rollback of the claim.
   */
  async claimAndEnqueue(
    schedule: StoredScheduler,
    nowMs: number,
    beforeInsert?: (slot: ClaimEnqueueSlot, schedule: StoredScheduler) => void | Promise<void>,
  ): Promise<ClaimResult> {
    if (!schedule.enabled) {
      return { claimed: false, reason: 'disabled', slots: [], advancedNext: null };
    }
    if (!schedule.nextRunAt) {
      return { claimed: false, reason: 'no_cursor', slots: [], advancedNext: null };
    }

    const plan = planMisfire({
      cron: schedule.cron,
      timezone: schedule.timezone,
      policy: schedule.policy,
      nextRunAtRaw: schedule.nextRunAt,
      nowMs,
    });
    if (!plan) {
      return { claimed: false, reason: 'not_due', slots: [], advancedNext: null };
    }

    const plannedSlots: ClaimEnqueueSlot[] = plan.enqueueSlots.map((iso) => {
      const ms = Date.parse(iso);
      return {
        occurrenceId: occurrenceId(schedule.id, schedule.revision, ms),
        scheduledForIso: iso,
        scheduledForMs: ms,
        revision: schedule.revision,
      };
    });

    if (beforeInsert) {
      for (const slot of plannedSlots) {
        await beforeInsert(slot, schedule);
      }
    }

    const advanced = plan.advancedNext;
    const claimedAt = canonicalUtcIso(plan.scheduledForMs) ?? plan.scheduledForRaw;
    const nowIso = new Date().toISOString();
    const concurrency = schedule.policy.concurrency;

    type Step = { sql: string; params?: readonly (string | number | null)[] };
    const steps: Step[] = [
      {
        sql: `UPDATE schedulers SET
           next_run_at = ?,
           last_claimed_at = ?,
           last_fired_minute = ?,
           updated_at = ?
         WHERE id = ?
           AND revision = ?
           AND enabled = 1
           AND next_run_at = ?;`,
        params: [
          advanced,
          claimedAt,
          claimedAt.slice(0, 16),
          nowIso,
          schedule.id,
          schedule.revision,
          plan.scheduledForRaw,
        ],
      },
    ];

    for (const slot of plannedSlots) {
      const dispatch = {
        ...schedule.task,
        task_id: slot.occurrenceId,
        source: 'schedule' as const,
        schedule_id: schedule.id,
        idempotency_key: slot.occurrenceId,
      };
      // Conditional insert: queue always attempts; forbid only when no active task.
      steps.push({
        sql: `INSERT INTO tasks(
          id, backend_id, kind, status, profile, input_json,
          cancel_requested, created_at, updated_at,
          schedule_id, schedule_revision, scheduled_for_ms
        )
        SELECT ?, ?, ?, 'queued', ?, ?, 0, ?, ?, ?, ?, ?
        WHERE ? = 'queue'
           OR NOT EXISTS (
             SELECT 1 FROM tasks
             WHERE schedule_id = ?
               AND status IN ('queued', 'running')
           )
        ON CONFLICT(id) DO NOTHING;`,
        params: [
          dispatch.task_id,
          dispatch.backend_id,
          dispatch.kind,
          dispatch.profile ?? null,
          JSON.stringify(dispatch),
          nowIso,
          nowIso,
          schedule.id,
          slot.revision,
          slot.scheduledForMs,
          concurrency,
          schedule.id,
        ],
      });
    }

    const results = await this.db.transaction(steps);
    const first = results[0];
    const changes =
      first != null && !Array.isArray(first) ? first.changes : 0;
    if (changes !== 1) {
      return {
        claimed: false,
        reason: 'cas_miss',
        plan,
        slots: [],
        advancedNext: null,
      };
    }

    // Only report slots whose INSERT actually changed a row (not suppressed / conflict).
    const insertedSlots: ClaimEnqueueSlot[] = [];
    for (let i = 0; i < plannedSlots.length; i++) {
      const stepResult = results[i + 1];
      const inserted =
        stepResult != null && !Array.isArray(stepResult) ? stepResult.changes : 0;
      if (inserted === 1) {
        insertedSlots.push(plannedSlots[i]!);
      }
    }

    return {
      claimed: true,
      plan,
      slots: insertedSlots,
      advancedNext: advanced,
    };
  }
}

function rowToScheduler(row: Record<string, unknown>): StoredScheduler {
  let policy = DEFAULT_SCHEDULE_POLICY;
  if (row['policy_json']) {
    try {
      policy = parseSchedulePolicy(JSON.parse(String(row['policy_json'])));
    } catch {
      policy = DEFAULT_SCHEDULE_POLICY;
    }
  }
  const out: StoredScheduler = {
    id: String(row['id']),
    cron: String(row['cron']),
    timezone: String(row['timezone']),
    enabled: Number(row['enabled']) === 1,
    task: JSON.parse(String(row['task_json'])) as CreateTaskInput,
    revision: Number(row['revision'] ?? 1) || 1,
    policy,
    updatedAt: String(row['updated_at']),
  };
  if (row['next_run_at']) out.nextRunAt = String(row['next_run_at']);
  if (row['last_claimed_at']) out.lastClaimedAt = String(row['last_claimed_at']);
  if (row['last_fired_minute']) out.lastFiredMinute = String(row['last_fired_minute']);
  return out;
}

/**
 * Minimal 5-field cron matcher (minute hour dom month dow), UTC wall clock.
 * Fallback when next_run_at is missing.
 */
export function cronMatchesUtc(cron: string, date: Date): boolean {
  const parts = cron.trim().split(/\s+/);
  if (parts.length !== 5) return false;
  const [min, hour, dom, mon, dow] = parts as [string, string, string, string, string];
  const jsDow = date.getUTCDay();
  return (
    fieldMatches(min, date.getUTCMinutes(), 0, 59) &&
    fieldMatches(hour, date.getUTCHours(), 0, 23) &&
    fieldMatches(dom, date.getUTCDate(), 1, 31) &&
    fieldMatches(mon, date.getUTCMonth() + 1, 1, 12) &&
    fieldMatchesDow(dow, jsDow)
  );
}

function fieldMatchesDow(field: string, jsDow: number): boolean {
  if (field === '*') return true;
  const wanted = new Set<number>();
  for (const part of field.split(',')) {
    if (part === '*') return true;
    if (part.includes('/')) {
      const [range, stepS] = part.split('/');
      const step = Number(stepS);
      if (!Number.isInteger(step) || step <= 0) continue;
      const [a, b] =
        range === '*'
          ? [0, 6]
          : range!.includes('-')
            ? range!.split('-').map(Number)
            : [Number(range), Number(range)];
      for (let v = a!; v <= (b ?? a!); v += step) wanted.add(v % 7);
      continue;
    }
    if (part.includes('-')) {
      const [a, b] = part.split('-').map(Number);
      for (let v = a!; v <= b!; v++) wanted.add(v === 7 ? 0 : v);
      continue;
    }
    const n = Number(part);
    if (Number.isInteger(n)) wanted.add(n === 7 ? 0 : n);
  }
  return wanted.has(jsDow);
}

function fieldMatches(field: string, value: number, min: number, max: number): boolean {
  if (field === '*') return true;
  for (const part of field.split(',')) {
    if (part === '*') return true;
    if (part.includes('/')) {
      const [range, stepS] = part.split('/');
      const step = Number(stepS);
      if (!Number.isInteger(step) || step <= 0) continue;
      let a = min;
      let b = max;
      if (range && range !== '*') {
        if (range.includes('-')) {
          const [x, y] = range.split('-').map(Number);
          a = x!;
          b = y!;
        } else {
          a = Number(range);
          b = max;
        }
      }
      for (let v = a; v <= b; v += step) {
        if (v === value) return true;
      }
      continue;
    }
    if (part.includes('-')) {
      const [a, b] = part.split('-').map(Number);
      if (value >= a! && value <= b!) return true;
      continue;
    }
    if (Number(part) === value) return true;
  }
  return false;
}

export function utcMinuteKey(date: Date = new Date()): string {
  const y = date.getUTCFullYear();
  const m = String(date.getUTCMonth() + 1).padStart(2, '0');
  const d = String(date.getUTCDate()).padStart(2, '0');
  const h = String(date.getUTCHours()).padStart(2, '0');
  const min = String(date.getUTCMinutes()).padStart(2, '0');
  return `${y}-${m}-${d}T${h}:${min}`;
}
