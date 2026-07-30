import { build } from "esbuild";
import { mkdir, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = resolve(import.meta.dirname, "..");
const monorepoRoot = resolve(root, "../../..");
const outputDirectory = resolve(root, "dist");
const release = process.env.VACPS_JS_MODE === "release";
const require = createRequire(import.meta.url);

await mkdir(outputDirectory, { recursive: true });

/** Bundle workspace packages + zod; keep only vacps:* external for QuickJS. */
const hostModulePlugin = {
  name: "vacps-host-modules",
  setup(buildContext) {
    buildContext.onResolve({ filter: /^vacps:/ }, (args) => ({
      path: args.path,
      external: true,
    }));
  },
};

const result = await build({
  entryPoints: [resolve(root, "src/main.ts")],
  outfile: resolve(outputDirectory, "vacps.mjs"),
  bundle: true,
  format: "esm",
  platform: "neutral",
  target: "es2025",
  plugins: [hostModulePlugin],
  // Resolve @vacps/contracts from monorepo packages/
  alias: {
    "@vacps/contracts": resolve(monorepoRoot, "packages/contracts/src/index.ts"),
  },
  // zod comes from script/node_modules or monorepo
  nodePaths: [
    resolve(root, "node_modules"),
    resolve(monorepoRoot, "node_modules"),
    resolve(monorepoRoot, "packages/contracts/node_modules"),
  ],
  treeShaking: true,
  keepNames: true,
  minifySyntax: release,
  minifyWhitespace: release,
  minifyIdentifiers: false,
  sourcemap: "external",
  sourcesContent: true,
  metafile: true,
  charset: "utf8",
  legalComments: "none",
  logLevel: "info",
});

await writeFile(
  resolve(outputDirectory, "vacps-meta.json"),
  JSON.stringify(result.metafile, null, 2),
);

console.log(`wrote ${resolve(outputDirectory, "vacps.mjs")}`);
