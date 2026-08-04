/**
 * Resolve applyPatch operation paths under a workspace root.
 *
 * Business scoping for patch targets — not a filesystem sandbox.
 * Absolute targets must stay under workspace; relative paths may not contain "..".
 */

import { normalizeAbsolutePath, requireAbsolutePath } from './absolute-path';

function invalidPath(message: string): Error & { code: string; statusCode: number } {
  const error = new Error(message) as Error & { code: string; statusCode: number };
  error.code = 'invalid_path';
  error.statusCode = 400;
  return error;
}

/**
 * Resolve a patch operation path under an absolute workspace.
 * Absolute targets must stay under workspace; relative paths may not contain "..".
 */
export function resolveUnderWorkspace(workspace: string, filePath: string): string {
  if (!filePath || typeof filePath !== 'string') {
    throw invalidPath('path is required.');
  }
  if (filePath.includes('\0')) {
    throw invalidPath('path contains a null byte.');
  }
  const root = requireAbsolutePath(workspace);
  let resolved: string;
  if (filePath.startsWith('/')) {
    resolved = requireAbsolutePath(filePath);
  } else {
    if (filePath.split('/').includes('..')) {
      throw invalidPath('relative path must not contain "..".');
    }
    const joined = root === '/' ? `/${filePath}` : `${root}/${filePath}`;
    resolved = normalizeAbsolutePath(joined);
  }
  const underRoot =
    root === '/' || resolved === root || resolved.startsWith(`${root}/`);
  if (!underRoot) {
    throw invalidPath('path escapes workspace.');
  }
  return resolved;
}
