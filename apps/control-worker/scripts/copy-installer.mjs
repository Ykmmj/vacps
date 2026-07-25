import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const packageDirectory = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const scripts = [{ source: 'install-agent.sh', target: 'agent.sh' }];

await mkdir(resolve(packageDirectory, 'web'), { recursive: true });
await Promise.all(
  scripts.map(({ source, target }) =>
    copyFile(
      resolve(packageDirectory, `../../scripts/${source}`),
      resolve(packageDirectory, `web/${target}`),
    ),
  ),
);
