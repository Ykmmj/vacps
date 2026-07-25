import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const packageDirectory = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const scriptNames = ['install-agent.sh', 'uninstall-agent.sh'];

await mkdir(resolve(packageDirectory, 'web'), { recursive: true });
await Promise.all(
  scriptNames.map((scriptName) =>
    copyFile(
      resolve(packageDirectory, `../../scripts/${scriptName}`),
      resolve(packageDirectory, `web/${scriptName}`),
    ),
  ),
);
