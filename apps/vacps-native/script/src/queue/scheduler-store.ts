import type { CreateTaskInput, SchedulePolicy } from "@vacps/contracts";
import type { Store } from "vacps:store";

import { migrateAgentDb } from "../storage/schema";
import {
  DEFAULT_SCHEDULE_POLICY,
  mergeSchedulerWire,
  parseSchedulePolicy,
  planMisfire,
  occurrenceId,
  canonicalUtcIso,
  type MisfirePlan,
} from "./schedule-logic";

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
  reason?: "not_due" | "cas_miss" | "disabled" | "no_cursor";
  plan?: MisfirePlan;
  slots: ClaimEnqueueSlot[];
  advancedNext: string | null;
}

/**
 * Local schedule store. Firing uses absolute next_run_at; revision-gated merge on sync.
 */
export class SchedulerStore {
  constructor(private readonly db: Store) {
    migrateAgentDb(db);
  }

  /**
   * Apply control-plane wire with revision merge rules.
   * Returns whether local row was written.
   */
  upsertFromWire(input: {
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
  }): boolean {
    const local = this.get(input.id);
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

    if (merged.action === "ignore") return false;

    let nextRunAt = merged.nextRunAt;
    if (merged.enabled && !nextRunAt && input.computeNextIfMissing) {
      nextRunAt = input.computeNextIfMissing();
    }
    if (!merged.enabled) nextRunAt = undefined;
    if (nextRunAt) nextRunAt = canonicalUtcIso(nextRunAt);

    this.writeRow({
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
  writeRow(input: {
    id: string;
    cron: string;
    timezone: string;
    enabled: boolean;
    task: CreateTaskInput;
    revision: number;
    policy: SchedulePolicy;
    nextRunAt?: string;
  }): void {
    const now = new Date().toISOString();
    const next = input.nextRunAt ? canonicalUtcIso(input.nextRunAt) ?? null : null;
    this.db.run(
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

  remove(id: string): void {
    this.db.run("DELETE FROM schedulers WHERE id = ?;", [id]);
  }

  list(): StoredScheduler[] {
    const rows = this.db.query("SELECT * FROM schedulers ORDER BY id ASC;");
    return rows.map(rowToScheduler);
  }

  get(id: string): StoredScheduler | undefined {
    const rows = this.db.query("SELECT * FROM schedulers WHERE id = ?;", [id]);
    if (rows.length === 0) return undefined;
    return rowToScheduler(rows[0]!);
  }

  listEnabled(): StoredScheduler[] {
    return this.list().filter((s) => s.enabled);
  }

  /**
   * CAS claim of current next_run_at occurrence and advance cursor.
   * Does NOT insert tasks — caller inserts in same transaction via claimAndEnqueueWith.
   *
   * Prefer {@link claimAndEnqueue} which runs UPDATE+INSERT in one transaction.
   */
  claimAndEnqueue(
    schedule: StoredScheduler,
    nowMs: number,
    insertTask: (slot: ClaimEnqueueSlot, schedule: StoredScheduler) => void,
  ): ClaimResult {
    if (!schedule.enabled) {
      return { claimed: false, reason: "disabled", slots: [], advancedNext: null };
    }
    if (!schedule.nextRunAt) {
      return { claimed: false, reason: "no_cursor", slots: [], advancedNext: null };
    }

    const plan = planMisfire({
      cron: schedule.cron,
      timezone: schedule.timezone,
      policy: schedule.policy,
      nextRunAtRaw: schedule.nextRunAt,
      nowMs,
    });
    if (!plan) {
      return { claimed: false, reason: "not_due", slots: [], advancedNext: null };
    }

    const slots: ClaimEnqueueSlot[] = plan.enqueueSlots.map((iso) => {
      const ms = Date.parse(iso);
      return {
        occurrenceId: occurrenceId(schedule.id, schedule.revision, ms),
        scheduledForIso: iso,
        scheduledForMs: ms,
        revision: schedule.revision,
      };
    });

    const advanced = plan.advancedNext;
    const claimedAt = canonicalUtcIso(plan.scheduledForMs) ?? plan.scheduledForRaw;
    const nowIso = new Date().toISOString();

    try {
      this.db.begin();
      const upd = this.db.run(
        `UPDATE schedulers SET
           next_run_at = ?,
           last_claimed_at = ?,
           last_fired_minute = ?,
           updated_at = ?
         WHERE id = ?
           AND revision = ?
           AND enabled = 1
           AND next_run_at = ?;`,
        [
          advanced,
          claimedAt,
          claimedAt.slice(0, 16), // observability minute-ish
          nowIso,
          schedule.id,
          schedule.revision,
          plan.scheduledForRaw, // CAS: raw string from read
        ],
      );
      if (upd.changes !== 1) {
        this.db.rollback();
        return {
          claimed: false,
          reason: "cas_miss",
          plan,
          slots: [],
          advancedNext: null,
        };
      }

      for (const slot of slots) {
        insertTask(slot, schedule);
      }

      this.db.commit();
      return {
        claimed: true,
        plan,
        slots,
        advancedNext: advanced,
      };
    } catch (e) {
      try {
        this.db.rollback();
      } catch {
        /* ignore */
      }
      throw e;
    }
  }
}

function rowToScheduler(row: Record<string, unknown>): StoredScheduler {
  let policy = DEFAULT_SCHEDULE_POLICY;
  if (row["policy_json"]) {
    try {
      policy = parseSchedulePolicy(JSON.parse(String(row["policy_json"])));
    } catch {
      policy = DEFAULT_SCHEDULE_POLICY;
    }
  }
  const out: StoredScheduler = {
    id: String(row["id"]),
    cron: String(row["cron"]),
    timezone: String(row["timezone"]),
    enabled: Number(row["enabled"]) === 1,
    task: JSON.parse(String(row["task_json"])) as CreateTaskInput,
    revision: Number(row["revision"] ?? 1) || 1,
    policy,
    updatedAt: String(row["updated_at"]),
  };
  if (row["next_run_at"]) out.nextRunAt = String(row["next_run_at"]);
  if (row["last_claimed_at"]) out.lastClaimedAt = String(row["last_claimed_at"]);
  if (row["last_fired_minute"]) out.lastFiredMinute = String(row["last_fired_minute"]);
  return out;
}

/**
 * Minimal 5-field cron matcher (minute hour dom month dow), UTC wall clock.
 * Fallback when next_run_at is missing (legacy only).
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
  if (field === "*") return true;
  const wanted = new Set<number>();
  for (const part of field.split(",")) {
    if (part === "*") return true;
    if (part.includes("/")) {
      const [range, stepS] = part.split("/");
      const step = Number(stepS);
      if (!Number.isInteger(step) || step <= 0) continue;
      const [a, b] =
        range === "*"
          ? [0, 6]
          : range!.includes("-")
            ? range!.split("-").map(Number)
            : [Number(range), Number(range)];
      for (let v = a!; v <= (b ?? a!); v += step) wanted.add(v % 7);
      continue;
    }
    if (part.includes("-")) {
      const [a, b] = part.split("-").map(Number);
      for (let v = a!; v <= b!; v++) wanted.add(v === 7 ? 0 : v);
      continue;
    }
    const n = Number(part);
    if (Number.isInteger(n)) wanted.add(n === 7 ? 0 : n);
  }
  return wanted.has(jsDow);
}

function fieldMatches(field: string, value: number, min: number, max: number): boolean {
  if (field === "*") return true;
  for (const part of field.split(",")) {
    if (part === "*") return true;
    if (part.includes("/")) {
      const [range, stepS] = part.split("/");
      const step = Number(stepS);
      if (!Number.isInteger(step) || step <= 0) continue;
      let a = min;
      let b = max;
      if (range && range !== "*") {
        if (range.includes("-")) {
          const [x, y] = range.split("-").map(Number);
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
    if (part.includes("-")) {
      const [a, b] = part.split("-").map(Number);
      if (value >= a! && value <= b!) return true;
      continue;
    }
    if (Number(part) === value) return true;
  }
  return false;
}

export function utcMinuteKey(date: Date = new Date()): string {
  const y = date.getUTCFullYear();
  const m = String(date.getUTCMonth() + 1).padStart(2, "0");
  const d = String(date.getUTCDate()).padStart(2, "0");
  const h = String(date.getUTCHours()).padStart(2, "0");
  const min = String(date.getUTCMinutes()).padStart(2, "0");
  return `${y}-${m}-${d}T${h}:${min}`;
}
