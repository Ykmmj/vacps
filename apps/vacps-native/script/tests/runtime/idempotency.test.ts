import { describe, expect, it } from "vitest";

import { hashRequest, IdempotencyStore } from "../../src/runtime/idempotency";

describe("IdempotencyStore", () => {
  it("returns null on first lookup", () => {
    const s = new IdempotencyStore();
    expect(s.lookup("files.write", "k1", "h1")).toBeNull();
  });

  it("ignores missing key", () => {
    const s = new IdempotencyStore();
    s.store("files.write", undefined, "h1", { ok: true });
    expect(s.lookup("files.write", undefined, "h1")).toBeNull();
  });

  it("stores and replays matching key+hash", () => {
    const s = new IdempotencyStore();
    s.store("files.write", "k1", "h1", { path: "/a" });
    expect(s.lookup("files.write", "k1", "h1")).toEqual({ path: "/a" });
  });

  it("throws on key reuse with different hash", () => {
    const s = new IdempotencyStore();
    s.store("files.write", "k1", "h1", { path: "/a" });
    expect(() => s.lookup("files.write", "k1", "h2")).toThrow(/idempotency/);
    try {
      s.lookup("files.write", "k1", "h2");
    } catch (e) {
      expect((e as { code: string }).code).toBe("idempotency_conflict");
      expect((e as { statusCode: number }).statusCode).toBe(409);
    }
  });

  it("withIdempotencyMeta attaches meta when key present", () => {
    const s = new IdempotencyStore();
    const body = s.withIdempotencyMeta("k", "hash", true, { ok: true });
    expect(body).toMatchObject({
      ok: true,
      idempotency: { key: "k", replayed: true, request_hash: "hash" },
    });
  });

  it("withIdempotencyMeta leaves body when no key", () => {
    const s = new IdempotencyStore();
    expect(s.withIdempotencyMeta(undefined, "h", false, { ok: true })).toEqual({ ok: true });
  });
});

describe("hashRequest", () => {
  it("is stable under key order", () => {
    const a = hashRequest({ b: 1, a: 2 });
    const b = hashRequest({ a: 2, b: 1 });
    expect(a).toBe(b);
    expect(a.startsWith("sha256:")).toBe(true);
  });

  it("differs for different payloads", () => {
    expect(hashRequest({ x: 1 })).not.toBe(hashRequest({ x: 2 }));
  });
});
