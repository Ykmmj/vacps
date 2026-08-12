import { build } from 'esbuild';
import { mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const outputDirectory = resolve(root, 'dist');
await mkdir(outputDirectory, { recursive: true });

await build({
  entryPoints: [resolve(root, 'tests/terminal_interaction_regression.ts')],
  outfile: resolve(outputDirectory, 'terminal-interaction-regression.mjs'),
  bundle: true,
  format: 'esm',
  platform: 'neutral',
  target: 'es2025',
  plugins: [
    {
      name: 'vacps-host-modules',
      setup(context) {
        context.onResolve({ filter: /^vacps:/ }, (args) => ({
          path: args.path,
          external: true,
        }));
      },
    },
  ],
  treeShaking: true,
  keepNames: true,
  charset: 'utf8',
  legalComments: 'none',
  logLevel: 'info',
});

console.log(`wrote ${resolve(outputDirectory, 'terminal-interaction-regression.mjs')}`);
