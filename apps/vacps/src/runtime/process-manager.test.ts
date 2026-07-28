import { describe, expect, it } from 'vitest';

import { ProcessManager } from './process-manager.js';

describe('ProcessManager', () => {
  it('runs a short command to completion within yield time', async () => {
    const manager = new ProcessManager('vacps-test');
    const result = await manager.exec({
      program: '/bin/echo',
      arguments: ['hi'],
      timeoutMs: 5_000,
      yieldTimeMs: 2_000,
    });
    expect(result.status).toBe('exited');
    expect(result.exit_code).toBe(0);
    expect(result.stdout.preview.trim()).toBe('hi');
  }, 10_000);

  it('returns running when yield is shorter than the process', async () => {
    const manager = new ProcessManager('vacps-test');
    const result = await manager.exec({
      program: 'node',
      arguments: ['-e', 'setTimeout(() => {}, 5000)'],
      timeoutMs: 10_000,
      yieldTimeMs: 200,
    });
    expect(result.status).toBe('running');
    const terminated = manager.terminate(result.process_id, 'sigkill');
    expect(['cancelled', 'signaled', 'running', 'exited']).toContain(terminated.status);
  });
});
