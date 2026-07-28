import { describe, expect, it } from 'vitest';

import { ProcessManager } from './process-manager.js';

describe('ProcessManager', () => {
  it('rejects /bin/sh with load_user_environment=true', async () => {
    const manager = new ProcessManager('backend-test');
    await expect(
      manager.exec({
        toolName: 'shell.exec',
        command: 'echo hi',
        shell: '/bin/sh',
        loadUserEnvironment: true,
      }),
    ).rejects.toMatchObject({ code: 'validation_error', statusCode: 400 });
  });

  it('returns a full process snapshot with output_cursor', async () => {
    const manager = new ProcessManager('backend-test');
    const result = await manager.exec({
      toolName: 'command.exec',
      program: 'printf',
      arguments: ['ok'],
      yieldTimeMs: 2_000,
    });
    expect(result.process_id).toBeTruthy();
    expect(result.backend_id).toBe('backend-test');
    expect(result.output_cursor).toBeNull();
    expect(result.stdout).toBeTruthy();
    expect(result.stderr).toBeTruthy();
    expect(typeof result.duration_ms).toBe('number');
  });

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

  it('rejects idempotency key reuse with different arguments', async () => {
    const manager = new ProcessManager('vacps-test');
    await manager.exec({
      toolName: 'command.exec',
      program: '/bin/echo',
      arguments: ['first'],
      idempotencyKey: 'same-key',
      yieldTimeMs: 2_000,
    });
    await expect(
      manager.exec({
        toolName: 'command.exec',
        program: '/bin/echo',
        arguments: ['second'],
        idempotencyKey: 'same-key',
        yieldTimeMs: 2_000,
      }),
    ).rejects.toMatchObject({ code: 'idempotency_conflict' });
  });

  it('replays the same request when idempotency key and hash match', async () => {
    const manager = new ProcessManager('vacps-test');
    const first = await manager.exec({
      toolName: 'command.exec',
      program: '/bin/echo',
      arguments: ['same'],
      idempotencyKey: 'replay-key',
      yieldTimeMs: 2_000,
    });
    const second = await manager.exec({
      toolName: 'command.exec',
      program: '/bin/echo',
      arguments: ['same'],
      idempotencyKey: 'replay-key',
      yieldTimeMs: 2_000,
    });
    expect(second.idempotency?.replayed).toBe(true);
    expect(second.process_id).toBe(first.process_id);
  });

  it('splits process.read chunks so returned UTF-8 bytes never exceed max_bytes', async () => {
    const manager = new ProcessManager('vacps-test');
    const payload = 'x'.repeat(5000);
    const started = await manager.exec({
      program: '/bin/echo',
      arguments: ['-n', payload],
      timeoutMs: 5_000,
      yieldTimeMs: 2_000,
    });
    expect(started.status).toBe('exited');

    const first = manager.read(started.process_id, { maxBytes: 2048 });
    const total1 = first.chunks.reduce(
      (sum, chunk) => sum + Buffer.byteLength(chunk.data, 'utf8'),
      0,
    );
    expect(total1).toBeLessThanOrEqual(2048);
    expect(total1).toBe(2048);
    expect(first.returned_bytes).toBe(2048);
    expect(first.eof).toBe(false);

    const second = manager.read(started.process_id, {
      cursor: first.next_cursor ?? undefined,
      maxBytes: 2048,
    });
    const total2 = second.chunks.reduce(
      (sum, chunk) => sum + Buffer.byteLength(chunk.data, 'utf8'),
      0,
    );
    expect(total2).toBeLessThanOrEqual(2048);
    expect(total1 + total2).toBeLessThanOrEqual(5000);
  }, 10_000);
});
