import { resolve, relative, isAbsolute, sep } from 'node:path';

const FORBIDDEN_PREFIXES = ['/proc', '/sys', '/dev'];

export function assertSafeAbsolutePath(filePath: string): string {
  if (!filePath || typeof filePath !== 'string') throw pathError('path is required.');
  if (!isAbsolute(filePath)) throw pathError('path must be absolute.');
  if (filePath.includes('\0')) throw pathError('path contains a null byte.');
  const normalized = resolve(filePath);
  for (const prefix of FORBIDDEN_PREFIXES) {
    if (normalized === prefix || normalized.startsWith(`${prefix}/`)) {
      throw pathError(`path under ${prefix} is not allowed.`);
    }
  }
  return normalized;
}

export function resolveWorkspacePath(workspace: string | undefined, filePath: string): string {
  if (isAbsolute(filePath)) {
    if (filePath.includes('..')) {
      // Absolute with .. segments is resolved below.
    }
    return assertSafeAbsolutePath(filePath);
  }
  if (filePath.includes('\0') || filePath.split(/[/\\]/).includes('..')) {
    throw pathError('relative path must not contain "..".');
  }
  const root = workspace ? assertSafeAbsolutePath(workspace) : process.cwd();
  const resolved = resolve(root, filePath);
  const rel = relative(root, resolved);
  if (rel.startsWith('..') || isAbsolute(rel)) {
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

export function toPosixRelative(from: string, to: string): string {
  return relative(from, to).split(sep).join('/');
}
