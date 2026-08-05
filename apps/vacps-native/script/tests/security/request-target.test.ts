import { describe, expect, it } from 'vitest';

import { requestTargetFromParts, requestTargetOf } from '../../src/security/request-target';

describe('requestTargetOf (vacps-request-v2)', () => {
  it('keeps pathname and search, drops fragment', () => {
    expect(requestTargetOf('https://agent.example/fs/read?path=%2Fa#frag')).toBe(
      '/fs/read?path=%2Fa',
    );
    expect(requestTargetOf('https://agent.example/tasks')).toBe('/tasks');
    expect(requestTargetOf('https://agent.example/')).toBe('/');
    expect(requestTargetOf('https://agent.example')).toBe('/');
  });

  it('preserves query when the path is only the authority (no slash before ?)', () => {
    // WHATWG: https://host?x=1 → pathname "/" + search "?x=1"
    expect(requestTargetOf('https://host?x=1')).toBe('/?x=1');
    expect(requestTargetOf('https://host?x=1#frag')).toBe('/?x=1');
  });

  it('preserves trailing slash on pathname (not router-normalized)', () => {
    expect(requestTargetOf('https://agent.example/tasks/')).toBe('/tasks/');
    expect(requestTargetOf('https://agent.example/tasks/?x=1')).toBe('/tasks/?x=1');
  });

  it('accepts path-only inputs via the same base URL as Node/Worker', () => {
    expect(requestTargetOf('/tasks')).toBe('/tasks');
    expect(requestTargetOf('/fs/read?path=a')).toBe('/fs/read?path=a');
  });
});

describe('requestTargetFromParts', () => {
  it('joins path and query with a single leading ?', () => {
    expect(requestTargetFromParts('/fs/read', 'path=a')).toBe('/fs/read?path=a');
    expect(requestTargetFromParts('/fs/read', '?path=a')).toBe('/fs/read?path=a');
    expect(requestTargetFromParts('/tasks', '')).toBe('/tasks');
    expect(requestTargetFromParts('/tasks', undefined)).toBe('/tasks');
    expect(requestTargetFromParts('', 'x=1')).toBe('/?x=1');
  });

  it('does not strip a trailing slash from the raw pre-router path', () => {
    // Auth must sign/verify request.raw.path, not router-normalized path.
    expect(requestTargetFromParts('/tasks/', undefined)).toBe('/tasks/');
    expect(requestTargetFromParts('/tasks/', 'x=1')).toBe('/tasks/?x=1');
  });
});
