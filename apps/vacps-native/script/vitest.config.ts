import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { defineConfig } from 'vitest/config';

const root = path.dirname(fileURLToPath(import.meta.url));

/**
 * Unit tests for pure / lightly-mocked script logic.
 * Full QuickJS Runtime/host behavior is verified by running JavaScript through
 * the compiled product binary.
 */
export default defineConfig({
  test: {
    // Unit tests live under tests/ — not colocated in src/ (product code only).
    include: ['tests/**/*.test.ts'],
    environment: 'node',
    passWithNoTests: false,
  },
  resolve: {
    alias: {
      'vacps:crypto': path.join(root, 'tests/mocks/vacps-crypto.ts'),
      'vacps:store': path.join(root, 'tests/mocks/vacps-store-mock.ts'),
      'vacps:host': path.join(root, 'tests/mocks/vacps-host.ts'),
      'vacps:log': path.join(root, 'tests/mocks/vacps-log.ts'),
    },
  },
});
