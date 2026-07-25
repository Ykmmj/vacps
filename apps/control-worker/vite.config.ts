import tailwindcss from '@tailwindcss/vite';
import { paraglideVitePlugin } from '@inlang/paraglide-js';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { defineConfig } from 'vite';

export default defineConfig({
  root: 'ui',
  plugins: [
    paraglideVitePlugin({
      project: './project.inlang',
      outdir: './ui/src/paraglide',
      strategy: ['localStorage', 'preferredLanguage', 'baseLocale'],
      localStorageKey: 'vps-agent-locale',
      emitTsDeclarations: true,
    }),
    tailwindcss(),
    svelte(),
  ],
  build: {
    emptyOutDir: true,
    outDir: '../web',
  },
});
