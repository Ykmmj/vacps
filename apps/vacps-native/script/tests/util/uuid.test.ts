import { describe, expect, it } from "vitest";

import { randomUuidV4 } from "../../src/util/uuid";

describe("randomUuidV4", () => {
  it("matches UUID v4 shape", () => {
    const id = randomUuidV4();
    expect(id).toMatch(
      /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/,
    );
  });

  it("generates unique values", () => {
    const a = randomUuidV4();
    const b = randomUuidV4();
    expect(a).not.toBe(b);
  });
});
