import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomUUID } from 'node:crypto';

import { afterEach, describe, expect, it } from 'vitest';

import { TaskStore } from './task-store.js';

const temporaryDirectories: string[] = [];

afterEach(async () => {
  await Promise.all(
    temporaryDirectories
      .splice(0)
      .map((directory) => rm(directory, { recursive: true, force: true })),
  );
});

describe('TaskStore', () => {
  it('persists a task and its audited command', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'vps-agent-store-'));
    temporaryDirectories.push(directory);
    const taskId = randomUUID();
    const store = new TaskStore(join(directory, 'agent.db'));
    store.createTask({
      taskId,
      backendId: 'vps-la-01',
      type: 'shell',
      command: 'uname -a',
      cwd: '/tmp',
      timeoutSeconds: 30,
      profile: 'full',
      source: 'api',
    });
    store.startCommand({
      id: 'command-1',
      taskId,
      sequence: 1,
      command: 'uname -a',
      cwd: '/tmp',
      status: 'running',
      stdoutPath: '/tmp/stdout.log',
      stderrPath: '/tmp/stderr.log',
      startedAt: new Date().toISOString(),
    });
    store.finishCommand({
      id: 'command-1',
      status: 'succeeded',
      exitCode: 0,
      finishedAt: new Date().toISOString(),
    });

    expect(store.getTask(taskId)?.status).toBe('queued');
    expect(store.listCommands(taskId)).toMatchObject([
      { id: 'command-1', exitCode: 0, status: 'succeeded' },
    ]);
    store.close();
  });
});
