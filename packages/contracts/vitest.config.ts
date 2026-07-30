import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    // Unit tests live under tests/ — not colocated in src/.
    include: ['tests/**/*.test.ts'],
    environment: 'node',
    passWithNoTests: false,
  },
});
