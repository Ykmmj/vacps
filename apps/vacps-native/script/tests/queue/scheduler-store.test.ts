import { describe, expect, it } from "vitest";

import { cronMatchesUtc, utcMinuteKey } from "../../src/queue/scheduler-store";

describe("cronMatchesUtc", () => {
  // 2024-01-15 12:30:00 UTC — Monday
  const d = new Date(Date.UTC(2024, 0, 15, 12, 30, 0));

  it("matches wildcards", () => {
    expect(cronMatchesUtc("* * * * *", d)).toBe(true);
  });

  it("matches exact minute/hour", () => {
    expect(cronMatchesUtc("30 12 * * *", d)).toBe(true);
    expect(cronMatchesUtc("0 12 * * *", d)).toBe(false);
    expect(cronMatchesUtc("30 13 * * *", d)).toBe(false);
  });

  it("matches lists and ranges", () => {
    expect(cronMatchesUtc("30 10,12,14 * * *", d)).toBe(true);
    expect(cronMatchesUtc("25-35 12 * * *", d)).toBe(true);
    expect(cronMatchesUtc("0-10 12 * * *", d)).toBe(false);
  });

  it("matches step values", () => {
    // every 15 minutes: 0,15,30,45
    expect(cronMatchesUtc("*/15 * * * *", d)).toBe(true);
    expect(cronMatchesUtc("*/10 * * * *", d)).toBe(true); // 30
    const at5 = new Date(Date.UTC(2024, 0, 15, 12, 5, 0));
    expect(cronMatchesUtc("*/15 * * * *", at5)).toBe(false);
  });

  it("matches day-of-week (Monday=1)", () => {
    expect(cronMatchesUtc("30 12 * * 1", d)).toBe(true);
    expect(cronMatchesUtc("30 12 * * 0", d)).toBe(false);
    expect(cronMatchesUtc("30 12 * * 1,3,5", d)).toBe(true);
  });

  it("treats 7 as Sunday", () => {
    const sun = new Date(Date.UTC(2024, 0, 14, 12, 0, 0)); // Sunday
    expect(cronMatchesUtc("0 12 * * 7", sun)).toBe(true);
    expect(cronMatchesUtc("0 12 * * 0", sun)).toBe(true);
  });

  it("matches month and day-of-month", () => {
    expect(cronMatchesUtc("30 12 15 1 *", d)).toBe(true);
    expect(cronMatchesUtc("30 12 16 1 *", d)).toBe(false);
    expect(cronMatchesUtc("30 12 * 2 *", d)).toBe(false);
  });

  it("rejects invalid field count", () => {
    expect(cronMatchesUtc("* * *", d)).toBe(false);
    expect(cronMatchesUtc("", d)).toBe(false);
  });
});

// nextCronRunAtIso lives in @vacps/contracts (timezone absolute time).

describe("utcMinuteKey", () => {
  it("formats UTC minute without seconds", () => {
    const d = new Date(Date.UTC(2024, 0, 15, 12, 5, 44));
    expect(utcMinuteKey(d)).toBe("2024-01-15T12:05");
  });
});
