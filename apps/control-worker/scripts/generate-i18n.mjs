import { compile } from '@inlang/paraglide-js';
import { fileURLToPath, URL } from 'node:url';

const packageRoot = new URL('../', import.meta.url);

await compile({
  project: fileURLToPath(new URL('project.inlang', packageRoot)),
  outdir: fileURLToPath(new URL('ui/src/paraglide', packageRoot)),
  strategy: ['localStorage', 'preferredLanguage', 'baseLocale'],
  localStorageKey: 'vps-agent-locale',
  emitTsDeclarations: true,
  silent: true,
});
