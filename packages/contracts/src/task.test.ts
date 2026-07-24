import { describe, expect, it } from 'vitest';

import { createTaskSchema } from './task.js';

describe('createTaskSchema', () => {
  it('accepts an absolute-path Shell task', () => {
    expect(
      createTaskSchema.parse({
        backendId: 'vps-la-01',
        type: 'shell',
        command: 'uname -a',
        cwd: '/srv/app',
        profile: 'full',
        timeoutSeconds: 300,
      }),
    ).toMatchObject({ type: 'shell', profile: 'full' });
  });

  it('rejects relative working directories', () => {
    expect(() =>
      createTaskSchema.parse({
        backendId: 'vps-la-01',
        type: 'agent',
        prompt: 'inspect the host',
        cwd: 'relative',
        profile: 'full',
        timeoutSeconds: 300,
      }),
    ).toThrow();
  });
});
