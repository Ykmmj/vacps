import { describe, expect, it } from 'vitest';

import { createTaskSchema, shellToCommand } from './task.js';

describe('createTaskSchema', () => {
  it('accepts a shell exec task with absolute cwd', () => {
    const parsed = createTaskSchema.parse({
      backendId: 'vacps-715f765653e6',
      type: 'shell',
      cwd: '/srv/app',
      timeoutSeconds: 300,
      shell: { mode: 'exec', program: 'uname', arguments: ['-a'] },
    });
    expect(parsed).toMatchObject({ type: 'shell', profile: 'full' });
    if (parsed.type === 'shell') {
      expect(shellToCommand(parsed.shell)).toContain('uname');
    }
  });

  it('accepts an agent task', () => {
    expect(
      createTaskSchema.parse({
        backendId: 'vacps-715f765653e6',
        type: 'agent',
        cwd: '/tmp',
        timeoutSeconds: 300,
        agent: { prompt: 'inspect the host', profile: 'diagnostic' },
      }),
    ).toMatchObject({ type: 'agent' });
  });

  it('rejects relative working directories', () => {
    expect(() =>
      createTaskSchema.parse({
        backendId: 'vacps-715f765653e6',
        type: 'agent',
        cwd: 'relative',
        timeoutSeconds: 300,
        agent: { prompt: 'inspect the host' },
      }),
    ).toThrow();
  });
});
