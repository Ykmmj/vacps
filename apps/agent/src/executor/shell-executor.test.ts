import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, describe, expect, it } from 'vitest';

import { ShellExecutor } from './shell-executor.js';

const temporaryDirectories: string[] = [];

afterEach(async () => {
  await Promise.all(
    temporaryDirectories
      .splice(0)
      .map((directory) => rm(directory, { recursive: true, force: true })),
  );
});

describe('ShellExecutor', () => {
  it('captures stdout, stderr, and a successful exit code', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'vacps-shell-'));
    temporaryDirectories.push(directory);
    const outcome = await new ShellExecutor().execute({
      command: 'printf "out"; printf "err" >&2',
      cwd: directory,
      timeoutSeconds: 5,
      stdoutPath: join(directory, 'stdout.log'),
      stderrPath: join(directory, 'stderr.log'),
    });

    expect(outcome).toMatchObject({
      status: 'succeeded',
      exitCode: 0,
      stdout: 'out',
      stderr: 'err',
    });
    await expect(readFile(join(directory, 'stdout.log'), 'utf8')).resolves.toBe('out');
    await expect(readFile(join(directory, 'stderr.log'), 'utf8')).resolves.toBe('err');
  });
});
