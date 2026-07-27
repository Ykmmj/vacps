import { describe, expect, it, vi } from 'vitest';

import { RegistrationTokenRepository } from '../src/registry/registration-token-repository.js';

function repositoryWithChanges(changes: number) {
  const run = vi.fn().mockResolvedValue({ meta: { changes } });
  const bind = vi.fn(() => ({ run }));
  const prepare = vi.fn(() => ({ bind }));
  return {
    repository: new RegistrationTokenRepository({ prepare } as unknown as D1Database),
    prepare,
    bind,
  };
}

describe('registration tokens', () => {
  it('stores only a hash when issuing a ten-minute registration token', async () => {
    const { repository, bind } = repositoryWithChanges(1);
    const issued = await repository.issue(Date.UTC(2026, 6, 27, 0, 0, 0));

    expect(issued.token).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(issued.expiresAt).toBe('2026-07-27T00:10:00.000Z');
    expect(bind).toHaveBeenCalledWith(
      expect.any(String),
      expect.not.stringContaining(issued.token),
      issued.expiresAt,
      '2026-07-27T00:00:00.000Z',
    );
  });

  it('requires exactly one successful conditional consume', async () => {
    const { repository, prepare } = repositoryWithChanges(0);
    await expect(repository.consume('a'.repeat(43))).rejects.toMatchObject({
      code: 'invalid_registration_token',
      status: 401,
    });
    expect(prepare).toHaveBeenCalledWith(expect.stringContaining('consumed_at IS NULL'));
    expect(prepare).toHaveBeenCalledWith(expect.stringContaining('expires_at > ?'));
  });
});
