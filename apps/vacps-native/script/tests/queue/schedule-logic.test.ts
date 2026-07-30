import { describe, expect, it } from "vitest";

import {
  canonicalUtcIso,
  mergeSchedulerWire,
  occurrenceId,
  planMisfire,
  DEFAULT_SCHEDULE_POLICY,
} from "../../src/queue/schedule-logic";

const baseTask = {
  kind: "command" as const,
  backend_id: "b1",
  program: "true",
  arguments: [] as string[],
  working_directory: "/tmp",
  timeout_seconds: 1,
  profile: "full",
  output: {
    capture_stdout: true,
    capture_stderr: true,
    preview_max_bytes: 8192,
    retention_seconds: 86_400,
    hard_max_bytes: 10_485_760,
  },
};

describe("occurrenceId", () => {
  it("uses ms epoch not ISO text", () => {
    const ms = Date.parse("2026-07-31T01:00:00.000Z");
    expect(occurrenceId("sch", 17, ms)).toBe(`sch:17:${ms}`);
    // same instant, different ISO → same id input ms
    expect(occurrenceId("sch", 17, Date.parse("2026-07-31T01:00:00Z"))).toBe(
      occurrenceId("sch", 17, ms),
    );
  });
});

describe("mergeSchedulerWire", () => {
  const local = {
    revision: 5,
    cron: "0 9 * * *",
    timezone: "UTC",
    enabled: true,
    task: baseTask,
    policy: DEFAULT_SCHEDULE_POLICY,
    nextRunAt: "2026-08-01T09:00:00.000Z",
  };

  it("ignores stale revision", () => {
    const r = mergeSchedulerWire(local, {
      ...local,
      revision: 4,
      nextRunAt: "2026-07-01T00:00:00.000Z",
    });
    expect(r.action).toBe("ignore");
  });

  it("higher rev accepts earlier next_run_at", () => {
    const r = mergeSchedulerWire(local, {
      ...local,
      revision: 6,
      cron: "0 18 * * *",
      nextRunAt: "2026-07-31T18:00:00.000Z",
    });
    expect(r.action).toBe("apply");
    if (r.action !== "apply") return;
    expect(r.revision).toBe(6);
    expect(r.cron).toBe("0 18 * * *");
    expect(r.nextRunAt).toBe("2026-07-31T18:00:00.000Z");
  });

  it("same rev keeps later cursor; empty incoming does not clear", () => {
    const r = mergeSchedulerWire(local, {
      ...local,
      revision: 5,
      nextRunAt: "2026-07-01T00:00:00.000Z",
    });
    expect(r.action).toBe("apply");
    if (r.action !== "apply") return;
    expect(r.nextRunAt).toBe("2026-08-01T09:00:00.000Z");

    const r2 = mergeSchedulerWire(local, {
      ...local,
      revision: 5,
      nextRunAt: undefined,
    });
    expect(r2.action).toBe("apply");
    if (r2.action !== "apply") return;
    expect(r2.nextRunAt).toBe("2026-08-01T09:00:00.000Z");
  });

  it("same rev accepts incoming when local has no next", () => {
    const r = mergeSchedulerWire(
      { ...local, nextRunAt: undefined },
      {
        ...local,
        revision: 5,
        nextRunAt: "2026-08-02T09:00:00.000Z",
      },
    );
    expect(r.action).toBe("apply");
    if (r.action !== "apply") return;
    expect(r.nextRunAt).toBe("2026-08-02T09:00:00.000Z");
  });

  it("canonicalizes non-millis Z on write path", () => {
    expect(canonicalUtcIso("2026-07-31T01:00:00Z")).toBe("2026-07-31T01:00:00.000Z");
  });
});

describe("planMisfire", () => {
  // Hourly at :00 UTC. stored next = 09:00, now = 13:30 → missed 09..13
  const nextRaw = "2024-06-01T09:00:00.000Z";
  const nowMs = Date.parse("2024-06-01T13:30:00.000Z");

  it("run_once enqueues only first slot and advances past now", () => {
    const plan = planMisfire({
      cron: "0 * * * *",
      timezone: "UTC",
      policy: { ...DEFAULT_SCHEDULE_POLICY, misfire: "run_once" },
      nextRunAtRaw: nextRaw,
      nowMs,
    });
    expect(plan).toBeDefined();
    expect(plan!.enqueueSlots).toEqual(["2024-06-01T09:00:00.000Z"]);
    expect(plan!.scheduledForRaw).toBe(nextRaw);
    expect(Date.parse(plan!.advancedNext!)).toBeGreaterThan(nowMs);
    // next hourly after backlog jump should be 14:00
    expect(plan!.advancedNext).toBe("2024-06-01T14:00:00.000Z");
  });

  it("skip enqueues nothing and advances past now", () => {
    const plan = planMisfire({
      cron: "0 * * * *",
      timezone: "UTC",
      policy: { ...DEFAULT_SCHEDULE_POLICY, misfire: "skip" },
      nextRunAtRaw: nextRaw,
      nowMs,
    });
    expect(plan!.enqueueSlots).toEqual([]);
    expect(plan!.advancedNext).toBe("2024-06-01T14:00:00.000Z");
  });

  it("catch_up respects max_catchup_runs", () => {
    const plan = planMisfire({
      cron: "0 * * * *",
      timezone: "UTC",
      policy: {
        ...DEFAULT_SCHEDULE_POLICY,
        misfire: "catch_up",
        max_catchup_runs: 2,
      },
      nextRunAtRaw: nextRaw,
      nowMs,
    });
    expect(plan!.enqueueSlots).toEqual([
      "2024-06-01T09:00:00.000Z",
      "2024-06-01T10:00:00.000Z",
    ]);
    // after last enqueued (10:00) → 11:00 still due; leave for next tick
    expect(plan!.advancedNext).toBe("2024-06-01T11:00:00.000Z");
  });

  it("returns undefined when not yet due", () => {
    const plan = planMisfire({
      cron: "0 * * * *",
      timezone: "UTC",
      policy: DEFAULT_SCHEDULE_POLICY,
      nextRunAtRaw: "2099-01-01T00:00:00.000Z",
      nowMs,
    });
    expect(plan).toBeUndefined();
  });

  it("advances from scheduled_for not wall clock as base slot", () => {
    // late start: scheduled 09:00, now 11:00 — occurrence id slot remains 09:00
    const plan = planMisfire({
      cron: "0 * * * *",
      timezone: "UTC",
      policy: { ...DEFAULT_SCHEDULE_POLICY, misfire: "run_once" },
      nextRunAtRaw: "2024-06-01T09:00:00.000Z",
      nowMs: Date.parse("2024-06-01T11:00:00.000Z"),
    });
    expect(plan!.enqueueSlots[0]).toBe("2024-06-01T09:00:00.000Z");
    expect(plan!.scheduledForMs).toBe(Date.parse("2024-06-01T09:00:00.000Z"));
  });
});
