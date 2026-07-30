/**
 * Path rules aligned with apps/vacps/src/runtime/path-guard.ts.
 * Absolute paths only for control-plane file APIs (C++ vacps:fs also enforces).
 */

const FORBIDDEN_PREFIXES = ['/proc', '/sys', '/dev'] as const;

export function assertSafeAbsolutePath(filePath: string): string {
  if (!filePath || typeof filePath !== 'string') throw pathError('path is required.');
  if (!filePath.startsWith('/')) throw pathError('path must be absolute.');
  if (filePath.includes('\0')) throw pathError('path contains a null byte.');
  const normalized = normalizeAbsolute(filePath);
  for (const prefix of FORBIDDEN_PREFIXES) {
    if (normalized === prefix || normalized.startsWith(`${prefix}/`)) {
      throw pathError(`path under ${prefix} is not allowed.`);
    }
  }
  return normalized;
}

function normalizeAbsolute(filePath: string): string {
  const parts: string[] = [];
  for (const seg of filePath.split('/')) {
    if (!seg || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) throw pathError('path escapes root.');
      parts.pop();
      continue;
    }
    parts.push(seg);
  }
  return `/${parts.join('/')}`;
}

/** Resolve relative path under workspace (absolute paths still go through assertSafeAbsolutePath). */
export function resolveWorkspacePath(workspace: string | undefined, filePath: string): string {
  if (filePath.startsWith('/')) {
    return assertSafeAbsolutePath(filePath);
  }
  if (filePath.includes('\0') || filePath.split('/').includes('..')) {
    throw pathError('relative path must not contain "..".');
  }
  const root = workspace ? assertSafeAbsolutePath(workspace) : '/tmp';
  const joined = root === '/' ? `/${filePath}` : `${root}/${filePath}`;
  const resolved = normalizeAbsolute(joined);
  if (resolved !== root && !resolved.startsWith(`${root}/`)) {
    throw pathError('path escapes workspace.');
  }
  return assertSafeAbsolutePath(resolved);
}

function pathError(message: string): Error & { code: string; statusCode: number } {
  const error = new Error(message) as Error & { code: string; statusCode: number };
  error.code = 'path_not_allowed';
  error.statusCode = 400;
  return error;
}
