/**
 * MCP / tool path sandbox (allowlist roots).
 *
 * - Default deny except configured roots (dataDir, /tmp, VACPS_FS_ALLOWED_ROOTS).
 * - Always reject /proc, /sys, /dev (even if somehow listed as roots).
 * - Lexical normalization; absolute paths outside roots are rejected.
 * - C++ vacps:fs is pure I/O — this policy is enforced only at tool boundaries.
 */

const KERNEL_FORBIDDEN = ['/proc', '/sys', '/dev'] as const;

/** Longest-first so more specific roots win prefix checks. */
let allowedRoots: string[] = ['/tmp'];

export type PathGuardConfig = {
  /** Agent data directory (always allowed when set). */
  dataDir?: string;
  /** Extra absolute roots (e.g. from VACPS_FS_ALLOWED_ROOTS). */
  extraRoots?: string[];
  /** When true, reset to only /tmp before applying (tests). */
  replace?: boolean;
};

/**
 * Configure allowlist roots. Call once at application initialize.
 * Always includes `/tmp` for ephemeral task work unless `replace` + empty extra.
 */
export function configurePathGuard(opts: PathGuardConfig): void {
  const roots = new Set<string>();
  if (!opts.replace) {
    roots.add('/tmp');
  }
  if (opts.dataDir && opts.dataDir.trim()) {
    roots.add(normalizeAbsolute(opts.dataDir.trim()));
  }
  for (const raw of opts.extraRoots ?? []) {
    if (!raw || !raw.trim()) continue;
    roots.add(normalizeAbsolute(raw.trim()));
  }
  if (roots.size === 0) {
    roots.add('/tmp');
  }
  allowedRoots = [...roots]
    .filter((r) => !isKernelForbidden(r))
    .sort((a, b) => b.length - a.length);
}

export function getAllowedRoots(): readonly string[] {
  return allowedRoots;
}

/** Test helper: restore default /tmp-only roots. */
export function resetPathGuardForTests(): void {
  allowedRoots = ['/tmp'];
}

export function assertSafeAbsolutePath(filePath: string): string {
  if (!filePath || typeof filePath !== 'string') throw pathError('path is required.');
  if (!filePath.startsWith('/')) throw pathError('path must be absolute.');
  if (filePath.includes('\0')) throw pathError('path contains a null byte.');
  const normalized = normalizeAbsolute(filePath);
  if (isKernelForbidden(normalized)) {
    throw pathError(`path under kernel filesystem is not allowed.`);
  }
  if (!isUnderAllowedRoot(normalized)) {
    throw pathError(
      `path is outside allowed roots (${allowedRoots.join(', ') || 'none'}).`,
    );
  }
  return normalized;
}

function isKernelForbidden(normalized: string): boolean {
  for (const prefix of KERNEL_FORBIDDEN) {
    if (normalized === prefix || normalized.startsWith(`${prefix}/`)) return true;
  }
  return false;
}

export function isUnderAllowedRoot(normalized: string): boolean {
  for (const root of allowedRoots) {
    if (normalized === root || normalized.startsWith(`${root}/`)) return true;
  }
  return false;
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

/** Resolve relative path under workspace; workspace itself must be allowed. */
export function resolveWorkspacePath(workspace: string | undefined, filePath: string): string {
  if (filePath.startsWith('/')) {
    return assertSafeAbsolutePath(filePath);
  }
  if (filePath.includes('\0') || filePath.split('/').includes('..')) {
    throw pathError('relative path must not contain "..".');
  }
  const root = workspace ? assertSafeAbsolutePath(workspace) : assertSafeAbsolutePath('/tmp');
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
