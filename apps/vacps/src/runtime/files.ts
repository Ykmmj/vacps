import { createHash, randomBytes } from 'node:crypto';
import type { Stats } from 'node:fs';
import {
  access,
  constants,
  mkdir,
  open,
  readFile,
  readdir,
  rename,
  rm,
  stat,
} from 'node:fs/promises';
import { basename, dirname, join, relative, sep } from 'node:path';
import { spawn } from 'node:child_process';

import { sha256Hex } from './output.js';
import { assertSafeAbsolutePath, resolveWorkspacePath } from './path-guard.js';

export async function filesStat(pathInput: string) {
  const path = assertSafeAbsolutePath(pathInput);
  const info = await stat(path);
  let readable = false;
  let writable = false;
  try {
    await access(path, constants.R_OK);
    readable = true;
  } catch {
    /* ignore */
  }
  try {
    await access(path, constants.W_OK);
    writable = true;
  } catch {
    /* ignore */
  }
  let digest: string | null = null;
  if (info.isFile() && info.size <= 8 * 1024 * 1024) {
    digest = sha256Hex(await readFile(path));
  }
  return {
    path,
    type: info.isDirectory() ? 'directory' : info.isSymbolicLink() ? 'symlink' : 'file',
    size_bytes: info.size,
    modified_at: info.mtime.toISOString(),
    sha256: digest,
    readable,
    writable,
  };
}

export async function filesRead(input: {
  path: string;
  startLine?: number | undefined;
  endLine?: number | undefined;
  maxBytes?: number | undefined;
  encoding?: 'utf-8' | 'base64' | undefined;
}) {
  const path = assertSafeAbsolutePath(input.path);
  const encoding = input.encoding ?? 'utf-8';
  const maxBytes = clamp(input.maxBytes ?? 32_768, 1, 256 * 1024);

  if (
    input.startLine !== undefined &&
    input.endLine !== undefined &&
    input.endLine < input.startLine
  ) {
    throw Object.assign(
      new Error(`end_line (${input.endLine}) must be >= start_line (${input.startLine}).`),
      { code: 'invalid_line_range', statusCode: 400 },
    );
  }

  const buffer = await readFile(path);
  const totalBytes = buffer.length;
  const digest = sha256Hex(buffer);
  const info = await stat(path);
  const modifiedAt = info.mtime.toISOString();

  if (encoding === 'base64') {
    const slice = buffer.subarray(0, maxBytes);
    const truncated = slice.length < totalBytes;
    // Schema v2: no duplicated top-level total_* / sha256 fields.
    return {
      path,
      content: slice.toString('base64'),
      file: {
        kind: 'binary' as const,
        encoding: 'base64' as const,
        size_bytes: totalBytes,
        total_lines: null as number | null,
        sha256: digest,
        modified_at: modifiedAt,
      },
      range: {
        requested_start_line: null as number | null,
        requested_end_line: null as number | null,
        returned_start_line: null as number | null,
        returned_end_line: null as number | null,
        returned_bytes: slice.length,
        complete: !truncated,
        truncated,
        truncation_reason: truncated ? ('max_bytes' as const) : null,
        next_start_line: null as number | null,
      },
    };
  }

  const text = buffer.toString('utf8');
  const lines = text.split('\n');
  // Empty file → one empty line for line-oriented tools.
  const lineCount = lines.length;
  const requestedStart = input.startLine ?? 1;
  const requestedEnd = input.endLine ?? lineCount;
  const start = clamp(requestedStart, 1, Math.max(1, lineCount));
  const end = clamp(requestedEnd, start, Math.max(start, lineCount));

  let selected = lines.slice(start - 1, end);
  let content = selected.join('\n');
  let truncated = false;
  let truncationReason: 'max_bytes' | null = null;
  let nextStart: number | null = null;

  while (Buffer.byteLength(content, 'utf8') > maxBytes && selected.length > 1) {
    selected = selected.slice(0, -1);
    content = selected.join('\n');
    truncated = true;
    truncationReason = 'max_bytes';
    nextStart = start + selected.length;
  }
  if (Buffer.byteLength(content, 'utf8') > maxBytes) {
    content = Buffer.from(content, 'utf8').subarray(0, maxBytes).toString('utf8');
    truncated = true;
    truncationReason = 'max_bytes';
    nextStart = start;
  }

  const returnedStart = start;
  const returnedEnd = selected.length === 0 ? start - 1 : start + selected.length - 1;
  const rangeComplete = !truncated && returnedEnd >= end;
  const returnedBytes = Buffer.byteLength(content, 'utf8');

  return {
    path,
    content,
    file: {
      kind: 'text' as const,
      encoding: 'utf-8' as const,
      size_bytes: totalBytes,
      total_lines: lineCount,
      sha256: digest,
      modified_at: modifiedAt,
    },
    range: {
      requested_start_line: requestedStart,
      requested_end_line: requestedEnd,
      returned_start_line: returnedStart,
      returned_end_line: Math.max(returnedStart, returnedEnd),
      returned_bytes: returnedBytes,
      complete: rangeComplete,
      // Alias kept for older clients that checked range_complete.
      range_complete: rangeComplete,
      truncated,
      truncation_reason: truncationReason,
      next_start_line: truncated ? nextStart : null,
    },
  };
}

export async function filesList(input: {
  path: string;
  limit?: number | undefined;
  includeHidden?: boolean | undefined;
  cursor?: string | undefined;
}) {
  const path = assertSafeAbsolutePath(input.path);
  const limit = clamp(input.limit ?? 200, 1, 2000);
  const includeHidden = Boolean(input.includeHidden);
  const offset = decodeOffsetCursor(input.cursor);
  const entries = await readdir(path, { withFileTypes: true });
  const visible = entries.filter((entry) => includeHidden || !entry.name.startsWith('.'));
  const slice = visible.slice(offset, offset + limit);
  const matches = [];
  for (const entry of slice) {
    const full = join(path, entry.name);
    let size = 0;
    let modified_at = new Date(0).toISOString();
    try {
      const info = await stat(full);
      size = info.size;
      modified_at = info.mtime.toISOString();
    } catch {
      /* ignore */
    }
    matches.push({
      path: full,
      name: entry.name,
      type: entry.isDirectory() ? 'directory' : 'file',
      size_bytes: size,
      modified_at,
    });
  }
  const nextOffset = offset + matches.length;
  return {
    path,
    entries: matches,
    returned_count: matches.length,
    truncated: nextOffset < visible.length,
    next_cursor: nextOffset < visible.length ? encodeOffsetCursor(nextOffset) : null,
  };
}

function encodeOffsetCursor(offset: number): string {
  return Buffer.from(JSON.stringify({ o: offset }), 'utf8').toString('base64url');
}

function decodeOffsetCursor(cursor: string | undefined): number {
  if (!cursor) return 0;
  try {
    const decoded = JSON.parse(Buffer.from(cursor, 'base64url').toString('utf8')) as {
      o?: number;
    };
    return typeof decoded.o === 'number' && decoded.o >= 0 ? Math.trunc(decoded.o) : 0;
  } catch {
    throw Object.assign(new Error('Invalid cursor.'), {
      code: 'validation_error',
      statusCode: 400,
    });
  }
}

export async function filesGlob(input: {
  pattern: string;
  path?: string | undefined;
  includeHidden?: boolean | undefined;
  respectGitignore?: boolean | undefined;
  limit?: number | undefined;
  cursor?: string | undefined;
}) {
  const root = assertSafeAbsolutePath(input.path ?? process.cwd());
  const limit = clamp(input.limit ?? 200, 1, 2000);
  const includeHidden = Boolean(input.includeHidden);
  const respectGitignore = input.respectGitignore !== false;
  const pattern = input.pattern;
  const offset = decodeOffsetCursor(input.cursor);

  // Prefer ripgrep file listing when available.
  // Globstar dialect: **/foo also matches foo at the root (zero intermediate segments).
  try {
    if (!(await commandAvailable('rg')))
      throw Object.assign(new Error('rg missing'), { code: 'ENOENT' });
    const args = ['--files', '--glob', pattern];
    if (pattern.startsWith('**/')) args.push('--glob', pattern.slice(3));
    if (!includeHidden) args.push('--glob', '!.*/**');
    if (!respectGitignore) args.push('--no-ignore');
    const listed = await runCapture('rg', args, root, 30_000);
    if (listed.exitCode === 0 || listed.stdout) {
      const allLines = listed.stdout
        .split('\n')
        .map((line) => line.trim())
        .filter(Boolean);
      const page = allLines.slice(offset, offset + limit);
      const matches = [];
      for (const rel of page) {
        const full = join(root, rel);
        try {
          const info = await stat(full);
          matches.push({
            path: full,
            type: info.isDirectory() ? 'directory' : 'file',
            size_bytes: info.size,
            modified_at: info.mtime.toISOString(),
          });
        } catch {
          matches.push({ path: full, type: 'file', size_bytes: 0, modified_at: null });
        }
      }
      const nextOffset = offset + matches.length;
      const truncated = nextOffset < allLines.length;
      return {
        matches,
        returned_count: matches.length,
        truncated,
        next_cursor: truncated ? encodeOffsetCursor(nextOffset) : null,
      };
    }
  } catch {
    /* fallback */
  }

  const all: Array<Record<string, unknown>> = [];
  await walk(root, root, includeHidden, async (full, info: Stats) => {
    const rel = relative(root, full).split(sep).join('/');
    if (!globMatch(pattern, rel) && !globMatch(pattern, basename(full))) return;
    all.push({
      path: full,
      type: info.isDirectory() ? 'directory' : 'file',
      size_bytes: info.size,
      modified_at: info.mtime.toISOString(),
    });
  });
  const page = all.slice(offset, offset + limit);
  const nextOffset = offset + page.length;
  const truncated = nextOffset < all.length;
  return {
    matches: page,
    returned_count: page.length,
    truncated,
    next_cursor: truncated ? encodeOffsetCursor(nextOffset) : null,
  };
}

export async function filesGrep(input: {
  pattern: string;
  path?: string | undefined;
  filePattern?: string | undefined;
  caseSensitive?: boolean | undefined;
  fixedString?: boolean | undefined;
  contextBefore?: number | undefined;
  contextAfter?: number | undefined;
  maxMatches?: number | undefined;
  maxBytes?: number | undefined;
}) {
  const target = assertSafeAbsolutePath(input.path ?? process.cwd());
  const maxMatches = clamp(input.maxMatches ?? 100, 1, 500);
  const contextBefore = clamp(input.contextBefore ?? 0, 0, 10);
  const contextAfter = clamp(input.contextAfter ?? 0, 0, 10);
  const maxBytes = clamp(input.maxBytes ?? 64_000, 1, 256 * 1024);

  let info: Stats;
  try {
    info = await stat(target);
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    if (code === 'ENOENT') {
      throw Object.assign(new Error(`path not found: ${target}`), {
        code: 'path_not_found',
        statusCode: 400,
      });
    }
    if (code === 'ENOTDIR') {
      // Parent component is not a directory — treat as bad path input.
      throw Object.assign(new Error(`path is not a file or directory: ${target}`), {
        code: 'invalid_path',
        statusCode: 400,
      });
    }
    throw error;
  }

  if (!info.isFile() && !info.isDirectory()) {
    throw Object.assign(new Error(`path must be a file or directory: ${target}`), {
      code: 'invalid_path',
      statusCode: 400,
    });
  }

  const pathKind = info.isFile() ? ('file' as const) : ('directory' as const);

  if (await commandAvailable('rg')) {
    try {
      return await filesGrepWithRg(
        input,
        target,
        pathKind,
        maxMatches,
        contextBefore,
        contextAfter,
        maxBytes,
      );
    } catch (error) {
      const err = error as NodeJS.ErrnoException & { statusCode?: number };
      // Missing rg binary → fallback. Validation errors must not fall through as internal.
      if (err.code === 'ENOENT' && !err.statusCode) {
        /* use node fallback */
      } else {
        throw mapGrepSpawnError(error, target);
      }
    }
  }
  return filesGrepFallback(input, target, pathKind, maxMatches, maxBytes);
}

function mapGrepSpawnError(error: unknown, target: string): Error {
  const err = error as NodeJS.ErrnoException & { statusCode?: number; code?: string };
  if (err.statusCode) return err as Error;
  if (err.code === 'ENOTDIR') {
    return Object.assign(new Error(`path is not a file or directory: ${target}`), {
      code: 'invalid_path',
      statusCode: 400,
    });
  }
  if (err.code === 'ENOENT') {
    return Object.assign(new Error(`path not found: ${target}`), {
      code: 'path_not_found',
      statusCode: 400,
    });
  }
  return err as Error;
}

async function filesGrepWithRg(
  input: {
    pattern: string;
    filePattern?: string | undefined;
    caseSensitive?: boolean | undefined;
    fixedString?: boolean | undefined;
  },
  target: string,
  pathKind: 'file' | 'directory',
  maxMatches: number,
  contextBefore: number,
  contextAfter: number,
  maxBytes: number,
) {
  const args = ['--json', '--line-number', '--column', `--max-count=${maxMatches}`];
  if (!input.caseSensitive) args.push('-i');
  if (input.fixedString) args.push('-F');
  if (contextBefore) args.push(`-B${contextBefore}`);
  if (contextAfter) args.push(`-A${contextAfter}`);
  // file_pattern only applies when searching a directory tree.
  if (pathKind === 'directory' && input.filePattern) args.push('--glob', input.filePattern);
  args.push('--', input.pattern, target);

  // spawn cwd must be a directory. For a file path, use its parent.
  const cwd = pathKind === 'file' ? dirname(target) : target;
  const result = await runCapture('rg', args, cwd, 60_000);
  const matches: Array<Record<string, unknown>> = [];
  let bytes = 0;
  for (const line of result.stdout.split('\n')) {
    if (!line.trim()) continue;
    let parsed: {
      type?: string;
      data?: {
        path?: { text?: string };
        line_number?: number;
        submatches?: Array<{ start?: number }>;
        lines?: { text?: string };
      };
    };
    try {
      parsed = JSON.parse(line) as typeof parsed;
    } catch {
      continue;
    }
    if (parsed.type !== 'match' || !parsed.data) continue;
    const text = parsed.data.lines?.text ?? '';
    bytes += Buffer.byteLength(text, 'utf8');
    if (bytes > maxBytes) break;
    const matchPath = parsed.data.path?.text;
    matches.push({
      path: matchPath
        ? matchPath.startsWith('/')
          ? matchPath
          : join(cwd, matchPath)
        : target,
      line_number: parsed.data.line_number ?? 0,
      column_number: (parsed.data.submatches?.[0]?.start ?? 0) + 1,
      line: text.replace(/\n$/, ''),
      before: [],
      after: [],
    });
    if (matches.length >= maxMatches) break;
  }
  return {
    matches,
    match_count: matches.length,
    truncated: matches.length >= maxMatches || bytes >= maxBytes,
    next_cursor: null,
    engine: 'rg' as const,
  };
}

async function filesGrepFallback(
  input: {
    pattern: string;
    filePattern?: string | undefined;
    caseSensitive?: boolean | undefined;
    fixedString?: boolean | undefined;
  },
  target: string,
  pathKind: 'file' | 'directory',
  maxMatches: number,
  maxBytes: number,
) {
  const flags = input.caseSensitive ? '' : 'i';
  const regex = input.fixedString ? null : new RegExp(input.pattern, flags);
  const needle = input.fixedString ? input.pattern : null;
  const matches: Array<Record<string, unknown>> = [];
  let bytes = 0;
  let truncated = false;

  const visitFile = async (full: string, size: number, rootForRel: string) => {
    if (matches.length >= maxMatches || bytes >= maxBytes) {
      truncated = true;
      return;
    }
    if (pathKind === 'directory' && input.filePattern) {
      const rel = relative(rootForRel, full).split(sep).join('/');
      if (!globMatch(input.filePattern, basename(full)) && !globMatch(input.filePattern, rel)) {
        return;
      }
    }
    if (size > 2 * 1024 * 1024) return;
    let text: string;
    try {
      text = await readFile(full, 'utf8');
    } catch {
      return;
    }
    const lines = text.split('\n');
    for (let index = 0; index < lines.length; index += 1) {
      if (matches.length >= maxMatches || bytes >= maxBytes) {
        truncated = true;
        break;
      }
      const line = lines[index] ?? '';
      let column = 0;
      if (needle !== null) {
        const found = input.caseSensitive
          ? line.indexOf(needle)
          : line.toLowerCase().indexOf(needle.toLowerCase());
        if (found < 0) continue;
        column = found + 1;
      } else if (regex) {
        const match = regex.exec(line);
        if (!match) continue;
        column = (match.index ?? 0) + 1;
      } else continue;
      bytes += Buffer.byteLength(line, 'utf8');
      matches.push({
        path: full,
        line_number: index + 1,
        column_number: column,
        line,
        before: [],
        after: [],
      });
    }
  };

  if (pathKind === 'file') {
    const info = await stat(target);
    await visitFile(target, info.size, dirname(target));
  } else {
    await walk(target, target, false, async (full, info) => {
      if (!info.isFile()) return;
      await visitFile(full, info.size, target);
    });
  }

  return {
    matches,
    match_count: matches.length,
    truncated: truncated || matches.length >= maxMatches || bytes >= maxBytes,
    next_cursor: null,
    engine: 'node_fallback' as const,
  };
}

export async function filesEdit(input: {
  path: string;
  oldText: string;
  newText: string;
  replaceAll?: boolean | undefined;
  expectedSha256?: string | undefined;
}) {
  const path = assertSafeAbsolutePath(input.path);
  const before = await readFile(path);
  const beforeHash = sha256Hex(before);
  if (input.expectedSha256 && normalizeHash(input.expectedSha256) !== beforeHash) {
    throw Object.assign(new Error('The file changed after it was read.'), {
      code: 'file_version_conflict',
      statusCode: 409,
      current_sha256: beforeHash,
    });
  }
  const text = before.toString('utf8');
  const count = countOccurrences(text, input.oldText);
  if (count === 0) {
    throw Object.assign(new Error('old_text was not found.'), {
      code: 'old_text_not_found',
      statusCode: 409,
    });
  }
  if (count > 1 && !input.replaceAll) {
    throw Object.assign(new Error('old_text is not unique; set replace_all=true to replace all.'), {
      code: 'old_text_not_unique',
      statusCode: 409,
      match_count: count,
    });
  }
  const next = input.replaceAll
    ? text.split(input.oldText).join(input.newText)
    : text.replace(input.oldText, input.newText);
  await atomicWrite(path, next);
  const afterHash = sha256Hex(next);
  return {
    path,
    replacement_count: input.replaceAll ? count : 1,
    before_sha256: beforeHash,
    after_sha256: afterHash,
    bytes_changed: Math.abs(Buffer.byteLength(next) - before.length),
  };
}

export async function filesWrite(input: {
  path: string;
  content: string;
  mode: 'create' | 'overwrite' | 'create_or_overwrite';
  expectedSha256?: string | undefined;
  createParentDirectories?: boolean | undefined;
}) {
  const path = assertSafeAbsolutePath(input.path);
  let exists = true;
  try {
    await stat(path);
  } catch {
    exists = false;
  }
  if (input.mode === 'create' && exists) {
    throw Object.assign(new Error('File already exists.'), {
      code: 'file_exists',
      statusCode: 409,
    });
  }
  if (input.mode === 'overwrite' && !exists) {
    throw Object.assign(new Error('File does not exist.'), {
      code: 'file_not_found',
      statusCode: 404,
    });
  }
  if (exists && input.expectedSha256) {
    const current = sha256Hex(await readFile(path));
    if (normalizeHash(input.expectedSha256) !== current) {
      throw Object.assign(new Error('The file changed after it was read.'), {
        code: 'file_version_conflict',
        statusCode: 409,
        current_sha256: current,
      });
    }
  }
  if (input.createParentDirectories) {
    await mkdir(dirname(path), { recursive: true });
  }
  await atomicWrite(path, input.content);
  return {
    path,
    operation: exists ? 'overwritten' : 'created',
    size_bytes: Buffer.byteLength(input.content, 'utf8'),
    sha256: sha256Hex(input.content),
  };
}

export async function filesMove(input: {
  from: string;
  to: string;
  overwrite?: boolean | undefined;
  expectedSha256?: string | undefined;
}) {
  const from = assertSafeAbsolutePath(input.from);
  const to = assertSafeAbsolutePath(input.to);
  const fromInfo = await stat(from);
  if (fromInfo.isFile() && input.expectedSha256) {
    const current = sha256Hex(await readFile(from));
    if (normalizeHash(input.expectedSha256) !== current) {
      throw Object.assign(new Error('The file changed after it was read.'), {
        code: 'file_version_conflict',
        statusCode: 409,
        current_sha256: current,
      });
    }
  }
  if (!input.overwrite) {
    try {
      await stat(to);
      throw Object.assign(new Error('Destination already exists.'), {
        code: 'file_exists',
        statusCode: 409,
      });
    } catch (error) {
      if ((error as { code?: string }).code === 'file_exists') throw error;
    }
  }
  await mkdir(dirname(to), { recursive: true });
  await rename(from, to);
  return { from, to, operation: 'moved' };
}

export async function filesDelete(input: {
  path: string;
  recursive?: boolean | undefined;
  expectedSha256?: string | undefined;
  expectedType?: 'file' | 'directory' | undefined;
  dryRun?: boolean | undefined;
}) {
  const path = assertSafeAbsolutePath(input.path);
  const info = await stat(path);
  const type = info.isDirectory() ? 'directory' : 'file';
  if (input.expectedType && input.expectedType !== type) {
    throw Object.assign(new Error(`Expected ${input.expectedType} but found ${type}.`), {
      code: 'type_mismatch',
      statusCode: 409,
    });
  }
  if (type === 'file' && input.expectedSha256) {
    const current = sha256Hex(await readFile(path));
    if (normalizeHash(input.expectedSha256) !== current) {
      throw Object.assign(new Error('The file changed after it was read.'), {
        code: 'file_version_conflict',
        statusCode: 409,
        current_sha256: current,
      });
    }
  }

  if (input.dryRun) {
    if (type === 'file') {
      return {
        path,
        operation: 'delete_preview',
        dry_run: true,
        type,
        file_count: 1,
        total_bytes: info.size,
      };
    }
    let fileCount = 0;
    let totalBytes = 0;
    await walk(path, path, true, async (_full, child) => {
      if (child.isFile()) {
        fileCount += 1;
        totalBytes += child.size;
      }
    });
    return {
      path,
      operation: 'delete_preview',
      dry_run: true,
      type,
      file_count: fileCount,
      total_bytes: totalBytes,
    };
  }

  await rm(path, { recursive: Boolean(input.recursive) || type === 'directory', force: false });
  return { path, operation: 'deleted', dry_run: false, type };
}

export async function filesMkdir(input: { path: string; recursive?: boolean | undefined }) {
  const path = assertSafeAbsolutePath(input.path);
  await mkdir(path, { recursive: input.recursive !== false });
  return { path, operation: 'created', type: 'directory' };
}

export async function applyPatch(input: {
  patch: string;
  workspacePath?: string | undefined;
  dryRun?: boolean | undefined;
  atomic?: boolean | undefined;
}) {
  const workspace = input.workspacePath
    ? assertSafeAbsolutePath(input.workspacePath)
    : process.cwd();
  const operations = parsePatch(input.patch);
  if (operations.length === 0) {
    throw Object.assign(new Error('Patch contained no operations.'), {
      code: 'invalid_patch',
      statusCode: 400,
    });
  }

  const planned = operations.map((op) => ({
    ...op,
    absolute: resolveWorkspacePath(workspace, op.path),
    ...(op.toPath ? { absoluteTo: resolveWorkspacePath(workspace, op.toPath) } : {}),
  }));

  const backups = new Map<string, Buffer | null>();
  const results: Array<Record<string, unknown>> = [];

  const applyOne = async (op: (typeof planned)[number]) => {
    if (op.kind === 'add') {
      let exists = true;
      try {
        await stat(op.absolute);
      } catch {
        exists = false;
      }
      if (exists)
        throw Object.assign(new Error(`Cannot add existing file ${op.path}`), {
          code: 'file_exists',
          statusCode: 409,
        });
      const before = null;
      if (!input.dryRun) {
        await mkdir(dirname(op.absolute), { recursive: true });
        await atomicWrite(op.absolute, op.content ?? '');
      }
      results.push({
        path: op.path,
        operation: 'added',
        before_sha256: null,
        after_sha256: sha256Hex(op.content ?? ''),
        additions: (op.content ?? '').split('\n').length,
        deletions: 0,
      });
      backups.set(op.absolute, before);
      return;
    }
    if (op.kind === 'delete') {
      const before = await readFile(op.absolute);
      if (!input.dryRun) await rm(op.absolute);
      results.push({
        path: op.path,
        operation: 'deleted',
        before_sha256: sha256Hex(before),
        after_sha256: null,
        additions: 0,
        deletions: before.toString('utf8').split('\n').length,
      });
      backups.set(op.absolute, before);
      return;
    }
    if (op.kind === 'move') {
      const before = await readFile(op.absolute);
      if (!input.dryRun) {
        await mkdir(dirname(op.absoluteTo!), { recursive: true });
        await rename(op.absolute, op.absoluteTo!);
      }
      results.push({
        path: op.path,
        operation: 'moved',
        to: op.toPath,
        before_sha256: sha256Hex(before),
        after_sha256: sha256Hex(before),
        additions: 0,
        deletions: 0,
      });
      return;
    }
    // update
    const beforeBuf = await readFile(op.absolute);
    const before = beforeBuf.toString('utf8');
    const after = applyHunks(before, op.hunks ?? []);
    if (!input.dryRun) await atomicWrite(op.absolute, after);
    results.push({
      path: op.path,
      operation: 'updated',
      before_sha256: sha256Hex(beforeBuf),
      after_sha256: sha256Hex(after),
      additions: (op.hunks ?? []).reduce(
        (sum, hunk) => sum + hunk.filter((line) => line.startsWith('+')).length,
        0,
      ),
      deletions: (op.hunks ?? []).reduce(
        (sum, hunk) => sum + hunk.filter((line) => line.startsWith('-')).length,
        0,
      ),
    });
    backups.set(op.absolute, beforeBuf);
  };

  try {
    for (const op of planned) await applyOne(op);
  } catch (error) {
    if (input.atomic !== false && !input.dryRun) {
      for (const [path, content] of backups) {
        if (content === null) await rm(path, { force: true });
        else await atomicWrite(path, content);
      }
    }
    throw error;
  }

  return {
    applied: !input.dryRun,
    dry_run: Boolean(input.dryRun),
    atomic: input.atomic !== false,
    files: results,
  };
}

async function atomicWrite(path: string, content: string | Buffer) {
  const temp = `${path}.vacps-tmp-${randomBytes(6).toString('hex')}`;
  const handle = await open(temp, 'w');
  try {
    await handle.writeFile(content);
    await handle.sync();
  } finally {
    await handle.close();
  }
  await rename(temp, path);
}

function parsePatch(patch: string): Array<{
  kind: 'add' | 'update' | 'delete' | 'move';
  path: string;
  toPath?: string;
  content?: string;
  hunks?: string[][];
}> {
  const lines = patch.replace(/\r\n/g, '\n').split('\n');
  const ops: Array<{
    kind: 'add' | 'update' | 'delete' | 'move';
    path: string;
    toPath?: string;
    content?: string;
    hunks?: string[][];
  }> = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i] ?? '';
    if (line.startsWith('*** Add File: ')) {
      const path = line.slice('*** Add File: '.length).trim();
      i += 1;
      const contentLines: string[] = [];
      while (i < lines.length && !lines[i]!.startsWith('*** ')) {
        const row = lines[i]!;
        contentLines.push(row.startsWith('+') ? row.slice(1) : row);
        i += 1;
      }
      ops.push({ kind: 'add', path, content: contentLines.join('\n') });
      continue;
    }
    if (line.startsWith('*** Delete File: ')) {
      ops.push({ kind: 'delete', path: line.slice('*** Delete File: '.length).trim() });
      i += 1;
      continue;
    }
    if (line.startsWith('*** Update File: ')) {
      const path = line.slice('*** Update File: '.length).trim();
      i += 1;
      const hunks: string[][] = [];
      let current: string[] | null = null;
      while (i < lines.length && !lines[i]!.startsWith('*** ')) {
        const row = lines[i]!;
        if (row.startsWith('@@')) {
          if (current) hunks.push(current);
          current = [];
        } else if (current) {
          current.push(row);
        }
        i += 1;
      }
      if (current) hunks.push(current);
      ops.push({ kind: 'update', path, hunks });
      continue;
    }
    if (line.startsWith('*** Move to: ') || line.startsWith('*** Rename to: ')) {
      // Consumed with previous file context if present — treat standalone poorly.
      i += 1;
      continue;
    }
    i += 1;
  }
  return ops;
}

function applyHunks(original: string, hunks: string[][]): string {
  let text = original;
  for (const hunk of hunks) {
    const oldLines: string[] = [];
    const newLines: string[] = [];
    for (const row of hunk) {
      if (row.startsWith('-')) oldLines.push(row.slice(1));
      else if (row.startsWith('+')) newLines.push(row.slice(1));
      else if (row.startsWith(' ')) {
        oldLines.push(row.slice(1));
        newLines.push(row.slice(1));
      }
    }
    const oldBlock = oldLines.join('\n');
    const newBlock = newLines.join('\n');
    if (!text.includes(oldBlock)) {
      throw Object.assign(new Error('Patch hunk did not match file content.'), {
        code: 'patch_conflict',
        statusCode: 409,
      });
    }
    text = text.replace(oldBlock, newBlock);
  }
  return text;
}

async function walk(
  root: string,
  current: string,
  includeHidden: boolean,
  visit: (path: string, info: Stats) => Promise<void>,
) {
  const entries = await readdir(current, { withFileTypes: true });
  for (const entry of entries) {
    if (!includeHidden && entry.name.startsWith('.')) continue;
    const full = join(current, entry.name);
    let info: Stats;
    try {
      info = await stat(full);
    } catch {
      continue;
    }
    await visit(full, info);
    if (entry.isDirectory()) await walk(root, full, includeHidden, visit);
  }
}

// Globstar dialect (bash-like):
// - * matches within a single path segment
// - ** matches across segments, including zero segments
// - **/ plus *.txt therefore matches both root example.txt and dir/a.txt
function globMatch(pattern: string, value: string): boolean {
  const normalizedPattern = pattern.replace(/\\/g, '/');
  const normalizedValue = value.replace(/\\/g, '/');
  const regex = globToRegExp(normalizedPattern);
  return regex.test(normalizedValue);
}

function globToRegExp(pattern: string): RegExp {
  let source = '^';
  for (let index = 0; index < pattern.length;) {
    const char = pattern[index]!;
    if (char === '*') {
      if (pattern[index + 1] === '*') {
        // ** optionally followed by /
        if (pattern[index + 2] === '/') {
          source += '(?:.*/)?';
          index += 3;
        } else {
          source += '.*';
          index += 2;
        }
      } else {
        source += '[^/]*';
        index += 1;
      }
      continue;
    }
    if (char === '?') {
      source += '[^/]';
      index += 1;
      continue;
    }
    if ('+.^${}()|[]\\'.includes(char)) source += `\\${char}`;
    else source += char;
    index += 1;
  }
  source += '$';
  return new RegExp(source);
}

export async function commandAvailable(name: string): Promise<boolean> {
  try {
    const result = await runCapture(name, ['--version'], process.cwd(), 3_000);
    return result.exitCode === 0 || result.stdout.length > 0 || result.stderr.length > 0;
  } catch {
    return false;
  }
}

export async function detectCapabilities() {
  const rgAvailable = await commandAvailable('rg');
  let rgVersion: string | null = null;
  if (rgAvailable) {
    try {
      const result = await runCapture('rg', ['--version'], process.cwd(), 3_000);
      rgVersion =
        (result.stdout.split('\n')[0] ?? result.stderr.split('\n')[0] ?? '').trim() || null;
    } catch {
      rgVersion = null;
    }
  }
  // Schema v2 nested shape — no dotted feature keys.
  return {
    schema_version: '2.0',
    features: {
      command_exec: true,
      shell_exec: true,
      interactive_process: true,
      file_patch: true,
      git_tools: true,
    },
    engines: {
      grep: {
        active: rgAvailable ? 'ripgrep' : 'node',
        available: rgAvailable,
        version: rgVersion,
        fallback: 'node',
        regex_flavor: rgAvailable ? 'rust' : 'javascript',
        respects_gitignore: rgAvailable,
      },
      glob: {
        dialect: 'globstar',
        respects_gitignore: true,
      },
    },
    limits: {
      command_timeout_max_ms: 3_600_000,
      process_read_max_bytes: 1_048_576,
      file_read_max_bytes: 262_144,
    },
    // Compatibility for older control planes until fully migrated.
    executables: {
      rg: { available: rgAvailable, version: rgVersion },
    },
  };
}

function countOccurrences(haystack: string, needle: string): number {
  if (!needle) return 0;
  let count = 0;
  let index = 0;
  while (true) {
    const found = haystack.indexOf(needle, index);
    if (found === -1) break;
    count += 1;
    index = found + needle.length;
  }
  return count;
}

function normalizeHash(value: string): string {
  return value.startsWith('sha256:') ? value : `sha256:${value}`;
}

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, Math.trunc(value)));
}

function runCapture(
  program: string,
  args: string[],
  cwd: string,
  timeoutMs: number,
): Promise<{ exitCode: number | null; stdout: string; stderr: string }> {
  return new Promise((resolve, reject) => {
    const child = spawn(program, args, { cwd, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      reject(Object.assign(new Error(`${program} timed out`), { code: 'timed_out' }));
    }, timeoutMs);
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => (stdout += chunk));
    child.stderr.on('data', (chunk) => (stderr += chunk));
    child.on('error', (error) => {
      clearTimeout(timer);
      reject(error);
    });
    child.on('close', (code) => {
      clearTimeout(timer);
      resolve({ exitCode: code, stdout, stderr });
    });
  });
}
