import { describe, expect, it } from "vitest";

import { assertSafeAbsolutePath, resolveWorkspacePath } from "../../src/runtime/path-guard";

describe("assertSafeAbsolutePath", () => {
  it("accepts normal absolute paths", () => {
    expect(assertSafeAbsolutePath("/tmp/foo")).toBe("/tmp/foo");
    expect(assertSafeAbsolutePath("/home/user/x")).toBe("/home/user/x");
  });

  it("normalizes . and //", () => {
    expect(assertSafeAbsolutePath("/tmp/./a//b")).toBe("/tmp/a/b");
  });

  it("rejects relative paths", () => {
    expect(() => assertSafeAbsolutePath("rel")).toThrow(/absolute/);
  });

  it("rejects empty / nullish", () => {
    expect(() => assertSafeAbsolutePath("")).toThrow(/required/);
  });

  it("rejects null byte", () => {
    expect(() => assertSafeAbsolutePath("/tmp/a\0b")).toThrow(/null byte/);
  });

  it("rejects /proc /sys /dev", () => {
    expect(() => assertSafeAbsolutePath("/proc/self")).toThrow(/not allowed/);
    expect(() => assertSafeAbsolutePath("/sys/kernel")).toThrow(/not allowed/);
    expect(() => assertSafeAbsolutePath("/dev/null")).toThrow(/not allowed/);
  });

  it("rejects escaping past root via ..", () => {
    expect(() => assertSafeAbsolutePath("/../etc")).toThrow(/escapes root/);
  });
});

describe("resolveWorkspacePath", () => {
  it("passes absolute paths through guard", () => {
    expect(resolveWorkspacePath("/ws", "/tmp/x")).toBe("/tmp/x");
  });

  it("joins relative under workspace", () => {
    expect(resolveWorkspacePath("/ws", "a/b")).toBe("/ws/a/b");
  });

  it("defaults workspace to /tmp", () => {
    expect(resolveWorkspacePath(undefined, "z")).toBe("/tmp/z");
  });

  it("rejects relative with ..", () => {
    expect(() => resolveWorkspacePath("/ws", "a/../b")).toThrow(/\.\./);
  });
});
