import { describe, expect, it } from "vitest";

import type { CreateTaskInput, TaskDispatch } from "@vacps/contracts";

import { openMemoryStore } from "../helpers/memory-store";
import { DEFAULT_SCHEDULE_POLICY, occurrenceId } from "../../src/queue/schedule-logic";
import { SchedulerStore } from "../../src/queue/scheduler-store";
import { TaskStore } from "../../src/storage/task-store";

const task: CreateTaskInput = {
  kind: "command",
  backend_id: "backend-1",
  program: "true",
  arguments: [],
  working_directory: "/tmp",
  timeout_seconds: 5,
  profile: "full",
  output: {
    capture_stdout: true,
    capture_stderr: true,
    preview_max_bytes: 8192,
    retention_seconds: 86_400,
    hard_max_bytes: 10_485_760,
  },
};

function seed(
  schedulers: SchedulerStore,
  opts: {
    id?: string;
    revision?: number;
    nextRunAt: string;
    misfire?: "skip" | "run_once" | "catch_up";
    max_catchup_runs?: number;
    enabled?: boolean;
    cron?: string;
  },
) {
  const id = opts.id ?? "11111111-1111-4111-8111-111111111111";
  schedulers.writeRow({
    id,
    cron: opts.cron ?? "0 * * * *",
    timezone: "UTC",
    enabled: opts.enabled ?? true,
    task,
    revision: opts.revision ?? 1,
    policy: {
      ...DEFAULT_SCHEDULE_POLICY,
      misfire: opts.misfire ?? "run_once",
      max_catchup_runs: opts.max_catchup_runs ?? 1,
    },
    nextRunAt: opts.nextRunAt,
  });
  return id;
}

describe("claimAndEnqueue CAS + txn", () => {
  it("claims once; second claim on same cursor misses", () => {
    const db = openMemoryStore();
    const schedulers = new SchedulerStore(db);
    const tasks = new TaskStore(db);
    const id = seed(schedulers, { nextRunAt: "2024-06-01T09:00:00.000Z" });
    const s = schedulers.get(id)!;
    const nowMs = Date.parse("2024-06-01T09:00:30.000Z");

    const insert = (slot: {
      occurrenceId: string;
      scheduledForMs: number;
      revision: number;
    }) => {
      const dispatch = {
        ...task,
        task_id: slot.occurrenceId,
        source: "schedule",
        schedule_id: id,
      } as TaskDispatch;
      tasks.insertOccurrenceTask(dispatch, {
        scheduleId: id,
        scheduleRevision: slot.revision,
        scheduledForMs: slot.scheduledForMs,
      });
    };

    const r1 = schedulers.claimAndEnqueue(s, nowMs, insert);
    expect(r1.claimed).toBe(true);
    expect(r1.slots).toHaveLength(1);
    expect(tasks.getTask(r1.slots[0]!.occurrenceId)?.status).toBe("queued");

    const after = schedulers.get(id)!;
    expect(after.nextRunAt).toBe("2024-06-01T10:00:00.000Z");

    // Re-read and try claim with stale cursor — CAS miss
    const r2 = schedulers.claimAndEnqueue(
      { ...s, nextRunAt: s.nextRunAt },
      nowMs,
      insert,
    );
    expect(r2.claimed).toBe(false);
    expect(r2.reason).toBe("cas_miss");

    // Only one task
    const rows = db.query("SELECT COUNT(*) AS c FROM tasks;");
    expect(Number(rows[0]!["c"])).toBe(1);
  });

  it("same occurrence is idempotent (duplicate insert ok)", () => {
    const db = openMemoryStore();
    const schedulers = new SchedulerStore(db);
    const tasks = new TaskStore(db);
    const id = seed(schedulers, { nextRunAt: "2024-06-01T09:00:00.000Z" });
    const ms = Date.parse("2024-06-01T09:00:00.000Z");
    const oid = occurrenceId(id, 1, ms);
    const dispatch = {
      ...task,
      task_id: oid,
      source: "schedule",
      schedule_id: id,
    } as TaskDispatch;
    expect(
      tasks.insertOccurrenceTask(dispatch, {
        scheduleId: id,
        scheduleRevision: 1,
        scheduledForMs: ms,
      }),
    ).toBe(true);
    expect(
      tasks.insertOccurrenceTask(dispatch, {
        scheduleId: id,
        scheduleRevision: 1,
        scheduledForMs: ms,
      }),
    ).toBe(false);
  });

  it("insert failure rolls back cursor (simulated)", () => {
    const db = openMemoryStore();
    const schedulers = new SchedulerStore(db);
    const id = seed(schedulers, { nextRunAt: "2024-06-01T09:00:00.000Z" });
    const s = schedulers.get(id)!;
    const nowMs = Date.parse("2024-06-01T09:05:00.000Z");

    expect(() =>
      schedulers.claimAndEnqueue(s, nowMs, () => {
        throw new Error("forced insert failure");
      }),
    ).toThrow(/forced insert failure/);

    const after = schedulers.get(id)!;
    expect(after.nextRunAt).toBe("2024-06-01T09:00:00.000Z");
  });

  it("higher revision merge can move next earlier", () => {
    const db = openMemoryStore();
    const schedulers = new SchedulerStore(db);
    const id = seed(schedulers, {
      nextRunAt: "2026-08-01T09:00:00.000Z",
      revision: 5,
    });
    const ok = schedulers.upsertFromWire({
      id,
      cron: "0 18 * * *",
      timezone: "UTC",
      enabled: true,
      task,
      revision: 6,
      policy: DEFAULT_SCHEDULE_POLICY,
      nextRunAt: "2026-07-31T18:00:00.000Z",
    });
    expect(ok).toBe(true);
    expect(schedulers.get(id)!.revision).toBe(6);
    expect(schedulers.get(id)!.nextRunAt).toBe("2026-07-31T18:00:00.000Z");
  });

  it("same revision stale next does not rewind cursor", () => {
    const db = openMemoryStore();
    const schedulers = new SchedulerStore(db);
    const id = seed(schedulers, {
      nextRunAt: "2026-08-01T09:00:00.000Z",
      revision: 5,
    });
    schedulers.upsertFromWire({
      id,
      cron: "0 9 * * *",
      timezone: "UTC",
      enabled: true,
      task,
      revision: 5,
      policy: DEFAULT_SCHEDULE_POLICY,
      nextRunAt: "2026-07-01T09:00:00.000Z",
    });
    expect(schedulers.get(id)!.nextRunAt).toBe("2026-08-01T09:00:00.000Z");
  });

  it("stale lower revision sync ignored", () => {
    const db = openMemoryStore();
    const schedulers = new SchedulerStore(db);
    const id = seed(schedulers, {
      nextRunAt: "2026-08-01T09:00:00.000Z",
      revision: 5,
      cron: "0 9 * * *",
    });
    const ok = schedulers.upsertFromWire({
      id,
      cron: "0 0 * * *",
      timezone: "UTC",
      enabled: true,
      task,
      revision: 4,
      policy: DEFAULT_SCHEDULE_POLICY,
      nextRunAt: "2026-07-01T00:00:00.000Z",
    });
    expect(ok).toBe(false);
    expect(schedulers.get(id)!.cron).toBe("0 9 * * *");
  });

  it("disabled schedule is not claimed; already queued tasks untouched", () => {
    const db = openMemoryStore();
    const schedulers = new SchedulerStore(db);
    const tasks = new TaskStore(db);
    const id = seed(schedulers, {
      nextRunAt: "2024-06-01T09:00:00.000Z",
      enabled: true,
    });
    // Pre-existing queued task
    tasks.createTask(
      {
        ...task,
        task_id: "manual-1",
        source: "schedule",
        schedule_id: id,
      } as TaskDispatch,
      "queued",
    );

    schedulers.upsertFromWire({
      id,
      cron: "0 * * * *",
      timezone: "UTC",
      enabled: false,
      task,
      revision: 2,
      policy: DEFAULT_SCHEDULE_POLICY,
      nextRunAt: null,
    });

    const s = schedulers.get(id)!;
    const r = schedulers.claimAndEnqueue(
      s,
      Date.parse("2024-06-01T10:00:00.000Z"),
      () => {
        throw new Error("should not insert");
      },
    );
    expect(r.claimed).toBe(false);
    expect(r.reason).toBe("disabled");
    expect(tasks.getTask("manual-1")?.status).toBe("queued");
  });

  it("CAS compares raw next_run_at string (canonical writes)", () => {
    const db = openMemoryStore();
    const schedulers = new SchedulerStore(db);
    const id = seed(schedulers, { nextRunAt: "2024-06-01T09:00:00Z" }); // non-canonical input
    const s = schedulers.get(id)!;
    // writeRow canonicalizes
    expect(s.nextRunAt).toBe("2024-06-01T09:00:00.000Z");
    const r = schedulers.claimAndEnqueue(
      s,
      Date.parse("2024-06-01T09:01:00.000Z"),
      () => {
        /* no task */
      },
    );
    expect(r.claimed).toBe(true);
  });
});
