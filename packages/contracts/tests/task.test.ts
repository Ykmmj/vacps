import { describe, expect, it } from 'vitest';

import { createTaskSchema, taskSummary, taskToCommand } from '../src/task.js';

describe('createTaskSchema (Schema v3 kind)', () => {
  it('accepts a command task with absolute working_directory', () => {
    const parsed = createTaskSchema.parse({
      backend_id: 'vacps-715f765653e6',
      kind: 'command',
      program: 'uname',
      arguments: ['-a'],
      working_directory: '/srv/app',
      timeout_seconds: 300,
    });
    expect(parsed).toMatchObject({ kind: 'command', profile: 'full' });
    expect(taskToCommand(parsed)).toContain('uname');
    expect(taskSummary(parsed)).toContain('uname');
  });

  it('accepts a shell task', () => {
    const parsed = createTaskSchema.parse({
      backend_id: 'vacps-715f765653e6',
      kind: 'shell',
      command: 'npm ci && npm run build',
      timeout_seconds: 1800,
    });
    expect(parsed.kind).toBe('shell');
    expect(parsed.shell).toBe('/bin/bash');
    expect(parsed.load_user_environment).toBe(true);
  });

  it('accepts an agent task', () => {
    expect(
      createTaskSchema.parse({
        backend_id: 'vacps-715f765653e6',
        kind: 'agent',
        working_directory: '/tmp',
        timeout_seconds: 300,
        prompt: 'inspect the host',
        profile: 'diagnostic',
      }),
    ).toMatchObject({ kind: 'agent', profile: 'diagnostic' });
  });

  it('rejects relative working directories', () => {
    expect(() =>
      createTaskSchema.parse({
        backend_id: 'vacps-715f765653e6',
        kind: 'agent',
        working_directory: 'relative',
        timeout_seconds: 300,
        prompt: 'inspect the host',
      }),
    ).toThrow();
  });

  it('rejects legacy type+shell.mode shape', () => {
    expect(() =>
      createTaskSchema.parse({
        backend_id: 'vacps-715f765653e6',
        type: 'shell',
        cwd: '/tmp',
        timeoutSeconds: 300,
        shell: { mode: 'exec', program: 'uname' },
      }),
    ).toThrow();
  });
});
