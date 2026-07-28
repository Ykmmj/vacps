import { describe, expect, it } from 'vitest';

import { hashTaskRequest } from '../src/tasks/task-service.js';
import type { CreateTaskInput } from '@vacps/contracts';

const baseShell = {
  backendId: 'backend-01',
  type: 'shell' as const,
  cwd: '/tmp',
  timeoutSeconds: 60,
  profile: 'full',
  shell: { mode: 'exec' as const, program: 'printf', arguments: ['A\n'] },
  output: {
    captureStdout: true,
    captureStderr: true,
    previewMaxBytes: 8192,
    retentionSeconds: 86_400,
    hardMaxBytes: 10_485_760,
  },
} satisfies CreateTaskInput;

describe('task request hash', () => {
  it('is stable for identical payloads', async () => {
    const a = await hashTaskRequest(baseShell);
    const b = await hashTaskRequest({ ...baseShell });
    expect(a).toBe(b);
    expect(a.startsWith('sha256:')).toBe(true);
  });

  it('changes when arguments change', async () => {
    const a = await hashTaskRequest(baseShell);
    const b = await hashTaskRequest({
      ...baseShell,
      shell: { mode: 'exec', program: 'printf', arguments: ['B\n'] },
    });
    expect(a).not.toBe(b);
  });

  it('changes when name changes', async () => {
    const a = await hashTaskRequest({ ...baseShell, name: 'one' });
    const b = await hashTaskRequest({ ...baseShell, name: 'two' });
    expect(a).not.toBe(b);
  });
});
