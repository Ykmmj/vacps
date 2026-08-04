/**
 * Absolute path input validation (shape only — not a sandbox).
 *
 * Accepts non-empty absolute paths without NUL; lexically normalizes
 * `.` / `..` / `//`. Does not restrict roots, kernel mounts, or symlinks.
 */

function pathValidationError(message: string): Error & { code: string; statusCode: number } {
  const error = new Error(message) as Error & { code: string; statusCode: number };
  error.code = 'invalid_path';
  error.statusCode = 400;
  return error;
}

/** Lexically normalize an absolute path (`..` stops at `/`). */
export function normalizeAbsolutePath(filePath: string): string {
  const parts: string[] = [];
  for (const seg of filePath.split('/')) {
    if (!seg || seg === '.') continue;
    if (seg === '..') {
      if (parts.length > 0) parts.pop();
      continue;
    }
    parts.push(seg);
  }
  return parts.length === 0 ? '/' : `/${parts.join('/')}`;
}

/**
 * Require a non-empty absolute path without embedded NUL.
 * Returns the lexically normalized form.
 */
export function requireAbsolutePath(filePath: string): string {
  if (!filePath || typeof filePath !== 'string') {
    throw pathValidationError('path is required.');
  }
  if (!filePath.startsWith('/')) {
    throw pathValidationError('path must be absolute.');
  }
  if (filePath.includes('\0')) {
    throw pathValidationError('path contains a null byte.');
  }
  return normalizeAbsolutePath(filePath);
}
