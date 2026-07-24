import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const packageDirectory = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const outputPath = resolve(packageDirectory, 'web/install-agent.sh');

await mkdir(dirname(outputPath), { recursive: true });
await copyFile(resolve(packageDirectory, '../../scripts/install-agent.sh'), outputPath);
