import type { BackendStatus } from '@vps-agent/contracts';
import { describe, expect, it, vi } from 'vitest';

import { BackendRepository } from '../src/registry/repository.js';

const partialStatus: BackendStatus = {
  health: {
    ok: true,
    backendId: 'test-node',
    version: '0.1.0',
    uptimeSeconds: 1,
    worker: { running: true, concurrency: 1 },
    redis: { connected: true },
    pi: { available: true },
  },
  metrics: {
    cpu: { usagePercent: 12.5, load1: 0.1, cores: 2 },
    memory: { totalBytes: 1024, usedBytes: 512 },
    disk: { totalBytes: 1024, usedBytes: 512 },
    queue: { waiting: 0, active: 0, failed: 0 },
  },
};

function repositoryWithPreparedStatementSpy() {
  const run = vi.fn().mockResolvedValue({});
  const bind = vi.fn(() => ({ run }));
  const prepare = vi.fn(() => ({ bind }));
  return {
    repository: new BackendRepository({ prepare } as unknown as D1Database),
    prepare,
    bind,
  };
}

describe('backend status persistence', () => {
  it('atomically preserves cached system telemetry for a health check result', async () => {
    const { repository, prepare, bind } = repositoryWithPreparedStatementSpy();

    await repository.recordStatus('test-node', partialStatus, { preserveSystem: true });

    expect(prepare).toHaveBeenCalledWith(
      expect.stringContaining("json_type(last_status, '$.system') = 'object'"),
    );
    expect(prepare).toHaveBeenCalledWith(expect.stringContaining('json_patch'));
    expect(bind).toHaveBeenCalledWith(
      JSON.stringify(partialStatus),
      JSON.stringify(partialStatus),
      expect.any(String),
      expect.any(String),
      'test-node',
    );
  });

  it('replaces cached telemetry when an incoming report contains system data', async () => {
    const { repository, prepare, bind } = repositoryWithPreparedStatementSpy();
    const fullStatus: BackendStatus = {
      ...partialStatus,
      system: {
        platform: 'linux',
        distribution: 'Ubuntu 24.04 LTS',
        version: '24.04',
        kernel: '6.8.0',
        architecture: 'x64',
      },
    };

    await repository.recordStatus('test-node', fullStatus, { preserveSystem: true });

    expect(prepare).toHaveBeenCalledWith(
      'UPDATE backends SET last_status = ?, last_checked_at = ?, updated_at = ? WHERE id = ?',
    );
    expect(bind).toHaveBeenCalledWith(
      JSON.stringify(fullStatus),
      expect.any(String),
      expect.any(String),
      'test-node',
    );
  });
});
