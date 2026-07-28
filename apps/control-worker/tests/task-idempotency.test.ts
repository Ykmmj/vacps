import { describe, expect, it } from 'vitest';

import { hashTaskRequest } from '../src/tasks/task-service.js';
import type { CreateTaskInput } from '@vacps/contracts';

const baseCommand = {
  backend_id: 'backend-01',
  kind: 'command' as const,
  working_directory: '/tmp',
  timeout_seconds: 60,
  profile: 'full',
  program: 'printf',
  arguments: ['A\n'],
  output: {
    capture_stdout: true,
    capture_stderr: true,
    preview_max_bytes: 8192,
    retention_seconds: 86_400,
    hard_max_bytes: 10_485_760,
  },
} satisfies CreateTaskInput;

describe('task request hash', () => {
  it('is stable for identical payloads', async () => {
    const a = await hashTaskRequest(baseCommand);
    const b = await hashTaskRequest({ ...baseCommand });
    expect(a).toBe(b);
    expect(a.startsWith('sha256:')).toBe(true);
  });

  it('changes when arguments change', async () => {
    const a = await hashTaskRequest(baseCommand);
    const b = await hashTaskRequest({
      ...baseCommand,
      arguments: ['B\n'],
    });
    expect(a).not.toBe(b);
  });

  it('changes when name changes', async () => {
    const a = await hashTaskRequest({ ...baseCommand, name: 'one' });
    const b = await hashTaskRequest({ ...baseCommand, name: 'two' });
    expect(a).not.toBe(b);
  });
});
