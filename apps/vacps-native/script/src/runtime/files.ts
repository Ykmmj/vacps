import * as crypto from 'vacps:crypto';
import {
  File,
  O_RDONLY,
  O_WRONLY,
  O_CREAT,
  O_TRUNC,
  exists,
  mkdir,
  readDirectory,
  remove,
  rename,
  stat,
  type DirEntry,
  type FileStat,
} from 'vacps:fs';
import * as process from 'vacps:process';

import {
  truncateStringToUtf8Bytes,
  utf8ByteLengthOfString,
  utf8Decode,
  utf8PrefixEnd,
} from '../util/utf8';
import { assertSafeAbsolutePath, resolveWorkspacePath } from './path-guard';

export { utf8PrefixEnd } from '../util/utf8';

function clamp(n: number, min: number, max: number): number {
  if (!Number.isFinite(n)) return min;
  return Math.min(max, Math.max(min, n));
}

function runtimeError(
  message: string,
  code: string,
  statusCode: number,
  extra?: Record<string, unknown>,
): Error & { code: string; statusCode: number } {
  const e = new Error(message) as Error & {
    code: string;
    statusCode: number;
  } & Record<string, unknown>;
  e.code = code;
  e.statusCode = statusCode;
  if (extra) Object.assign(e, extra);
  return e;
}

function joinPath(dir: string, name: string): string {
  if (dir === '/') return `/${name}`;
  return `${dir.replace(/\/$/, '')}/${name}`;
}

function basename(p: string): string {
  const i = p.lastIndexOf('/');
  return i >= 0 ? p.slice(i + 1) : p;
}

function dirname(p: string): string {
  const i = p.lastIndexOf('/');
  if (i <= 0) return '/';
  return p.slice(0, i);
}

function relativeTo(root: string, full: string): string {
  if (full === root) return '';
  const prefix = root.endsWith('/') ? root : `${root}/`;
  if (full.startsWith(prefix)) return full.slice(prefix.length);
  return full;
}

function normalizeHash(h: string): string {
  return h.startsWith('sha256:') ? h.slice(7) : h;
}

function asUint8(buf: ArrayBuffer | Uint8Array): Uint8Array {
  return buf instanceof Uint8Array ? buf : new Uint8Array(buf);
}

// ── Product-local File helpers (open → read/write → close) ─

async function readTextFile(path: string): Promise<string> {
  const f = await File.open(path, O_RDONLY);
  try {
    return await f.readText();
  } finally {
    await f.close();
  }
}

async function writeTextFile(path: string, content: string): Promise<void> {
  const f = await File.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  try {
    await f.writeText(content);
  } finally {
    await f.close();
  }
}

async function readBytesFile(path: string): Promise<Uint8Array> {
  const f = await File.open(path, O_RDONLY);
  try {
    return await f.read();
  } finally {
    await f.close();
  }
}

async function readRangeFile(path: string, offset: number, maxBytes: number): Promise<Uint8Array> {
  const f = await File.open(path, O_RDONLY);
  try {
    return await f.readAt(offset, maxBytes);
  } finally {
    await f.close();
  }
}

async function hashFileBytes(path: string): Promise<{ sizeBytes: number; sha256Hex: string }> {
  const f = await File.open(path, O_RDONLY);
  try {
    const bytes = await f.read();
    return { sizeBytes: bytes.byteLength, sha256Hex: crypto.sha256Hex(bytes) };
  } finally {
    await f.close();
  }
}

// ── Core CRUD ─────────────────────────────────────────────────────

export async function filesStat(pathInput: string) {
  const path = assertSafeAbsolutePath(pathInput);
  let st: FileStat;
  try {
    st = await stat(path);
  } catch {
    throw runtimeError(`Path not found: ${path}`, 'path_not_found', 404);
  }

  let digest: string | null = null;
  if (st.type === 'file' && st.size <= 8 * 1024 * 1024) {
    try {
      digest = crypto.sha256Hex(await readBytesFile(path));
    } catch {
      digest = null;
    }
  }

  return {
    path: st.path,
    type: st.type,
    size_bytes: st.size,
    modified_at: st.mtimeMs > 0 ? new Date(st.mtimeMs).toISOString() : (null as string | null),
    sha256: digest,
    readable: st.readable,
    writable: st.writable,
  };
}

export async function filesRead(input: {
  path: string;
  startLine?: number;
  endLine?: number;
  maxBytes?: number;
  encoding?: 'utf-8' | 'base64';
}) {
  const path = assertSafeAbsolutePath(input.path);
  const encoding = input.encoding ?? 'utf-8';
  const maxBytes = clamp(input.maxBytes ?? 32_768, 1, 256 * 1024);

  if (
    input.startLine !== undefined &&
    input.endLine !== undefined &&
    input.endLine < input.startLine
  ) {
    throw runtimeError(
      `end_line (${input.endLine}) must be >= start_line (${input.startLine}).`,
      'invalid_line_range',
      400,
    );
  }

  if (!(await exists(path))) {
    throw runtimeError(`Path not found: ${path}`, 'path_not_found', 404);
  }

  // Digest of full file; content loaded only for the requested window.
  const digestInfo = await hashFileBytes(path);
  const sizeBytes = digestInfo.sizeBytes;
  const digest = digestInfo.sha256Hex;

  if (encoding === 'base64') {
    const end = Math.min(maxBytes, sizeBytes);
    const slice = asUint8(await readRangeFile(path, 0, end));
    return {
      path,
      content: crypto.base64Encode(slice),
      file: {
        kind: 'binary' as const,
        encoding: 'base64' as const,
        size_bytes: sizeBytes,
        sha256: digest,
        truncated: sizeBytes > end,
      },
    };
  }

  let startLine = 1;
  let endLine = 0;
  let content: string;
  let truncated: boolean;

  if (input.startLine !== undefined || input.endLine !== undefined) {
    // Line selection still needs a text window; cap to avoid whole multi-GB files.
    const lineCap = Math.min(sizeBytes, 4 * 1024 * 1024);
    const raw = asUint8(await readRangeFile(path, 0, lineCap));
    const fullText = utf8Decode(raw);
    const lines = fullText.split('\n');
    const from = Math.max(1, input.startLine ?? 1);
    const to = Math.min(lines.length, input.endLine ?? lines.length);
    startLine = from;
    endLine = to;
    const segment = lines.slice(from - 1, to).join('\n');
    content = truncateStringToUtf8Bytes(segment, maxBytes);
    truncated = utf8ByteLengthOfString(segment) > maxBytes || sizeBytes > lineCap;
  } else {
    const raw = asUint8(await readRangeFile(path, 0, maxBytes));
    const end = utf8PrefixEnd(raw, Math.min(maxBytes, raw.byteLength));
    content = utf8Decode(raw.subarray(0, end));
    truncated = sizeBytes > end;
  }

  return {
    path,
    content,
    file: {
      kind: 'text' as const,
      encoding: 'utf-8' as const,
      size_bytes: sizeBytes,
      sha256: digest,
      start_line: startLine,
      end_line: endLine || undefined,
      truncated,
      ...(truncated ? { truncation_reason: 'max_bytes' as const } : {}),
    },
  };
}

export async function filesWrite(input: {
  path: string;
  content: string;
  mode: 'create' | 'overwrite' | 'create_or_overwrite';
  createParentDirectories?: boolean;
  expectedSha256?: string;
}) {
  const path = assertSafeAbsolutePath(input.path);
  const pathExists = await exists(path);
  if (input.mode === 'create' && pathExists) {
    throw runtimeError('Path already exists.', 'path_exists', 409);
  }
  if (input.mode === 'overwrite' && !pathExists) {
    throw runtimeError('Path not found.', 'path_not_found', 404);
  }
  if (pathExists && input.expectedSha256) {
    const current = crypto.sha256Hex(await readTextFile(path));
    if (normalizeHash(input.expectedSha256) !== current) {
      throw runtimeError('The file changed after it was read.', 'file_version_conflict', 409, {
        current_sha256: current,
      });
    }
  }
  if (input.createParentDirectories !== false) {
    const parent = dirname(path);
    if (parent && parent !== '/') {
      await mkdir(parent);
    }
  }
  await writeTextFile(path, input.content);
  return {
    path,
    operation: pathExists ? 'overwritten' : 'created',
    size_bytes: utf8ByteLengthOfString(input.content),
    sha256: crypto.sha256Hex(input.content),
  };
}

export async function filesList(input: {
  path: string;
  limit?: number;
  includeHidden?: boolean;
  cursor?: string;
}) {
  const path = assertSafeAbsolutePath(input.path);
  if (!(await exists(path))) {
    throw runtimeError(`Path not found: ${path}`, 'path_not_found', 404);
  }
  const limit = clamp(input.limit ?? 200, 1, 2000);
  const includeHidden = input.includeHidden === true;
  const offset = decodeOffsetCursor(input.cursor);
  const entries = await readDirectory(path);
  const filtered = entries.filter((e) => includeHidden || !e.name.startsWith('.'));
  const slice = filtered.slice(offset, offset + limit);
  const nextOffset = offset + slice.length;
  const truncated = nextOffset < filtered.length;
  return {
    path,
    entries: slice.map((e) => ({
      path: joinPath(path, e.name),
      name: e.name,
      type: e.isDir ? 'directory' : 'file',
      size_bytes: e.size,
    })),
    returned_count: slice.length,
    truncated,
    next_cursor: truncated ? encodeOffsetCursor(nextOffset) : null,
  };
}

export async function filesMkdir(input: { path: string; recursive?: boolean }) {
  const path = assertSafeAbsolutePath(input.path);
  await mkdir(path);
  return { path, operation: 'created', type: 'directory' };
}

export async function filesDelete(input: {
  path: string;
  recursive?: boolean;
  expectedSha256?: string;
  expectedType?: 'file' | 'directory';
  dryRun?: boolean;
}) {
  const path = assertSafeAbsolutePath(input.path);
  let st: FileStat;
  try {
    st = await stat(path);
  } catch {
    throw runtimeError(`Path not found: ${path}`, 'path_not_found', 404);
  }
  const type = st.type === 'directory' ? 'directory' : 'file';
  if (input.expectedType && input.expectedType !== type) {
    throw runtimeError(`Expected ${input.expectedType} but found ${type}.`, 'type_mismatch', 409);
  }
  if (type === 'file' && input.expectedSha256) {
    const current = crypto.sha256Hex(await readBytesFile(path));
    if (normalizeHash(input.expectedSha256) !== current) {
      throw runtimeError('The file changed after it was read.', 'file_version_conflict', 409, {
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
        total_bytes: st.size,
      };
    }
    let fileCount = 0;
    let totalBytes = 0;
    await walk(path, true, async (_full, isDir, size) => {
      if (!isDir) {
        fileCount += 1;
        totalBytes += size;
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

  if (type === 'directory' && !input.recursive) {
    const entries = await readDirectory(path);
    if (entries.length > 0) {
      throw runtimeError(
        'Directory is not empty; pass recursive=true to delete.',
        'directory_not_empty',
        409,
      );
    }
  }

  await remove(path);
  return { path, operation: 'deleted', dry_run: false, type };
}

export async function filesMove(input: {
  from: string;
  to: string;
  overwrite?: boolean;
  expectedSha256?: string;
}) {
  const from = assertSafeAbsolutePath(input.from);
  const to = assertSafeAbsolutePath(input.to);
  if (!(await exists(from))) {
    throw runtimeError(`Path not found: ${from}`, 'path_not_found', 404);
  }
  let fromSt: FileStat;
  try {
    fromSt = await stat(from);
  } catch {
    throw runtimeError(`Path not found: ${from}`, 'path_not_found', 404);
  }
  if (fromSt.type === 'file' && input.expectedSha256) {
    const current = crypto.sha256Hex(await readBytesFile(from));
    if (normalizeHash(input.expectedSha256) !== current) {
      throw runtimeError('The file changed after it was read.', 'file_version_conflict', 409, {
        current_sha256: current,
      });
    }
  }
  const overwrite = input.overwrite === true;
  if (!overwrite && (await exists(to))) {
    throw runtimeError('Destination already exists.', 'path_exists', 409);
  }
  const parent = dirname(to);
  if (parent && parent !== '/') {
    await mkdir(parent);
  }
  if (overwrite && (await exists(to))) {
    await remove(to);
  }
  await rename(from, to);
  return { from, to, operation: 'moved' };
}

// ── Glob / Grep / Edit / Patch ────────────────────────────────────

export async function filesGlob(input: {
  pattern: string;
  path?: string;
  includeHidden?: boolean;
  limit?: number;
  cursor?: string;
  respectGitignore?: boolean;
}) {
  const root = assertSafeAbsolutePath(input.path ?? '/tmp');
  const limit = clamp(input.limit ?? 200, 1, 2000);
  const includeHidden = Boolean(input.includeHidden);
  const respectGitignore = input.respectGitignore !== false;
  const pattern = input.pattern;
  const offset = decodeOffsetCursor(input.cursor);

  // Prefer ripgrep --files when available.
  try {
    const args = ['--files', '--glob', pattern];
    if (pattern.startsWith('**/')) args.push('--glob', pattern.slice(3));
    if (!includeHidden) args.push('--glob', '!.*/**');
    // rg respects .gitignore by default; --no-ignore disables it.
    if (!respectGitignore) args.push('--no-ignore');
    const listed = await process
      .run('/usr/bin/rg', args, {
        cwd: root,
        timeoutMs: 30_000,
      })
      .catch(async () => process.run('rg', args, { cwd: root, timeoutMs: 30_000 }));
    if (listed.exitCode === 0 || listed.stdout) {
      const allLines = listed.stdout
        .split('\n')
        .map((line) => line.trim())
        .filter(Boolean);
      const page = allLines.slice(offset, offset + limit);
      const matches = page.map((rel) => ({
        path: joinPath(root, rel),
        type: 'file' as const,
        size_bytes: 0,
      }));
      const nextOffset = offset + matches.length;
      const truncated = nextOffset < allLines.length;
      return {
        matches,
        returned_count: matches.length,
        truncated,
        next_cursor: truncated ? encodeOffsetCursor(nextOffset) : null,
        engine: 'rg' as const,
        respects_gitignore: respectGitignore,
      };
    }
  } catch {
    /* walk fallback */
  }

  const ignoreGlobs = respectGitignore ? await loadGitignore(root) : [];
  const all: Array<{ path: string; type: string; size_bytes: number }> = [];
  await walk(root, includeHidden, async (full, isDir, size) => {
    const rel = relativeTo(root, full);
    if (respectGitignore && isGitignored(rel, ignoreGlobs)) return;
    if (!globMatch(pattern, rel) && !globMatch(pattern, basename(full))) return;
    all.push({
      path: full,
      type: isDir ? 'directory' : 'file',
      size_bytes: size,
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
    engine: 'walk' as const,
    respects_gitignore: respectGitignore,
  };
}

export async function filesGrep(input: {
  pattern: string;
  path?: string;
  filePattern?: string;
  caseSensitive?: boolean;
  fixedString?: boolean;
  contextBefore?: number;
  contextAfter?: number;
  maxMatches?: number;
  maxBytes?: number;
  cursor?: string;
}) {
  const target = assertSafeAbsolutePath(input.path ?? '/tmp');
  const maxMatches = clamp(input.maxMatches ?? 100, 1, 500);
  const contextBefore = clamp(input.contextBefore ?? 0, 0, 10);
  const contextAfter = clamp(input.contextAfter ?? 0, 0, 10);
  const maxBytes = clamp(input.maxBytes ?? 64_000, 1, 256 * 1024);
  const caseSensitive = input.caseSensitive === true;
  const fixed = input.fixedString === true;
  const offset = decodeOffsetCursor(input.cursor);

  // Prefer rg
  try {
    const args = ['--json'];
    if (!caseSensitive) args.push('-i');
    if (fixed) args.push('-F');
    if (contextBefore > 0) args.push('-B', String(contextBefore));
    if (contextAfter > 0) args.push('-A', String(contextAfter));
    if (input.filePattern) args.push('--glob', input.filePattern);
    args.push('--', input.pattern, target);
    const result = await process
      .run('/usr/bin/rg', args, { timeoutMs: 30_000 })
      .catch(async () => process.run('rg', args, { timeoutMs: 30_000 }));
    if (result.stdout) {
      // Collect all matches then page by cursor offset.
      const all = parseRgJson(result.stdout, 10_000, maxBytes * 4);
      const page = all.slice(offset, offset + maxMatches);
      const nextOffset = offset + page.length;
      // Only truncated when more matches remain after this page.
      const truncated = nextOffset < all.length;
      return {
        matches: page,
        match_count: page.length,
        truncated,
        next_cursor: truncated ? encodeOffsetCursor(nextOffset) : null,
        engine: 'rg' as const,
      };
    }
  } catch {
    /* fallback */
  }

  const allMatches: Array<Record<string, unknown>> = [];
  let bytes = 0;
  let hitLimit = false;
  const re = fixed ? null : new RegExp(input.pattern, caseSensitive ? 'g' : 'gi');

  const visitFile = async (filePath: string) => {
    if (hitLimit) return;
    if (input.filePattern) {
      const name = basename(filePath);
      if (!globMatch(input.filePattern, name) && !globMatch(input.filePattern, filePath)) {
        return;
      }
    }
    let text: string;
    try {
      text = await readTextFile(filePath);
    } catch {
      return;
    }
    const lines = text.split('\n');
    for (let i = 0; i < lines.length; i++) {
      if (allMatches.length >= 10_000 || bytes >= maxBytes * 4) {
        hitLimit = true;
        break;
      }
      const line = lines[i]!;
      const hit = fixed
        ? caseSensitive
          ? line.includes(input.pattern)
          : line.toLowerCase().includes(input.pattern.toLowerCase())
        : re
          ? new RegExp(input.pattern, caseSensitive ? '' : 'i').test(line)
          : false;
      if (!hit) continue;
      const before = lines.slice(Math.max(0, i - contextBefore), i);
      const after = lines.slice(i + 1, i + 1 + contextAfter);
      allMatches.push({
        path: filePath,
        line_number: i + 1,
        line,
        context_before: before,
        context_after: after,
      });
      bytes += line.length + filePath.length;
    }
  };

  try {
    await readDirectory(target);
    await walk(target, true, async (full, isDir) => {
      if (!isDir) await visitFile(full);
    });
  } catch {
    await visitFile(target);
  }

  const page = allMatches.slice(offset, offset + maxMatches);
  const nextOffset = offset + page.length;
  const hasMoreInMemory = nextOffset < allMatches.length;
  // hitLimit: collection stopped early (more may exist on disk) but no safe cursor beyond buffer.
  const truncated = hasMoreInMemory || hitLimit;
  return {
    matches: page,
    match_count: page.length,
    truncated,
    next_cursor: hasMoreInMemory ? encodeOffsetCursor(nextOffset) : null,
    engine: 'walk' as const,
  };
}

export async function filesEdit(input: {
  path: string;
  oldText: string;
  newText: string;
  replaceAll?: boolean;
  expectedSha256?: string;
}) {
  const path = assertSafeAbsolutePath(input.path);
  const text = await readTextFile(path);
  const beforeHash = crypto.sha256Hex(text);
  if (input.expectedSha256 && normalizeHash(input.expectedSha256) !== beforeHash) {
    throw runtimeError('The file changed after it was read.', 'file_version_conflict', 409, {
      current_sha256: beforeHash,
    });
  }
  const count = countOccurrences(text, input.oldText);
  if (count === 0) {
    throw runtimeError('old_text was not found.', 'old_text_not_found', 409);
  }
  if (count > 1 && !input.replaceAll) {
    throw runtimeError(
      'old_text is not unique; set replace_all=true to replace all.',
      'old_text_not_unique',
      409,
      { match_count: count },
    );
  }
  const next = input.replaceAll
    ? text.split(input.oldText).join(input.newText)
    : text.replace(input.oldText, input.newText);
  await writeTextFile(path, next);
  return {
    path,
    replacement_count: input.replaceAll ? count : 1,
    before_sha256: beforeHash,
    after_sha256: crypto.sha256Hex(next),
    bytes_changed: Math.abs(next.length - text.length),
  };
}

export async function applyPatch(input: {
  patch: string;
  workspacePath?: string;
  dryRun?: boolean;
  atomic?: boolean;
}) {
  const workspace = input.workspacePath ? assertSafeAbsolutePath(input.workspacePath) : '/tmp';
  const operations = parsePatch(input.patch);
  if (operations.length === 0) {
    throw runtimeError('Patch contained no operations.', 'invalid_patch', 400);
  }

  const planned = operations.map((op) => ({
    ...op,
    absolute: resolveWorkspacePath(workspace, op.path),
  }));

  const backups = new Map<string, string | null>();
  const results: Array<Record<string, unknown>> = [];

  try {
    for (const op of planned) {
      if (op.kind === 'add') {
        if (await exists(op.absolute)) {
          throw runtimeError(`Cannot add existing file ${op.path}`, 'file_exists', 409);
        }
        if (!input.dryRun) {
          await mkdir(dirname(op.absolute));
          await writeTextFile(op.absolute, op.content ?? '');
          backups.set(op.absolute, null);
        }
        results.push({ path: op.path, operation: 'add', dry_run: Boolean(input.dryRun) });
      } else if (op.kind === 'delete') {
        if (!(await exists(op.absolute))) {
          throw runtimeError(`Cannot delete missing file ${op.path}`, 'path_not_found', 404);
        }
        if (!input.dryRun) {
          const before = await readTextFile(op.absolute);
          backups.set(op.absolute, before);
          await remove(op.absolute);
        }
        results.push({ path: op.path, operation: 'delete', dry_run: Boolean(input.dryRun) });
      } else if (op.kind === 'update') {
        if (!(await exists(op.absolute))) {
          throw runtimeError(`Cannot update missing file ${op.path}`, 'path_not_found', 404);
        }
        const before = await readTextFile(op.absolute);
        const after = applyHunks(before, op.hunks ?? []);
        if (!input.dryRun) {
          backups.set(op.absolute, before);
          await writeTextFile(op.absolute, after);
        }
        results.push({
          path: op.path,
          operation: 'update',
          dry_run: Boolean(input.dryRun),
          before_sha256: crypto.sha256Hex(before),
          after_sha256: crypto.sha256Hex(after),
        });
      }
    }
  } catch (error) {
    if (input.atomic !== false && !input.dryRun) {
      for (const [path, content] of backups) {
        try {
          if (content === null) await remove(path);
          else await writeTextFile(path, content);
        } catch {
          /* best-effort rollback */
        }
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

export async function detectCapabilities() {
  let rgAvailable = false;
  let rgVersion: string | null = null;
  try {
    const r = await process.run('rg', ['--version'], { timeoutMs: 3_000 });
    rgAvailable = r.exitCode === 0 || r.stdout.length > 0;
    if (rgAvailable) {
      rgVersion = (r.stdout.split('\n')[0] ?? '').trim() || null;
    }
  } catch {
    rgAvailable = false;
  }
  return {
    files: {
      read: true,
      write: true,
      edit: true,
      glob: true,
      grep: true,
      apply_patch: true,
      list: true,
      stat: true,
      move: true,
      delete: true,
      mkdir: true,
    },
    process: {
      exec: true,
      start: true,
      read: true,
      write: true,
      terminate: true,
    },
    tools: {
      rg: { available: rgAvailable, version: rgVersion },
    },
    pi: { available: false },
  };
}

// ── Internals ─────────────────────────────────────────────────────

async function walk(
  root: string,
  includeHidden: boolean,
  visit: (path: string, isDir: boolean, size: number) => Promise<void>,
  depth = 0,
): Promise<void> {
  if (depth > 32) return;
  let entries: DirEntry[];
  try {
    entries = await readDirectory(root);
  } catch {
    return;
  }
  for (const e of entries) {
    if (!includeHidden && e.name.startsWith('.')) continue;
    const full = joinPath(root, e.name);
    await visit(full, e.isDir, e.size);
    if (e.isDir) await walk(full, includeHidden, visit, depth + 1);
  }
}

function globMatch(pattern: string, value: string): boolean {
  return globToRegExp(pattern.replace(/\\/g, '/')).test(value.replace(/\\/g, '/'));
}

function globToRegExp(pattern: string): RegExp {
  let source = '^';
  for (let index = 0; index < pattern.length;) {
    const char = pattern[index]!;
    if (char === '*') {
      if (pattern[index + 1] === '*') {
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

function countOccurrences(text: string, needle: string): number {
  if (!needle) return 0;
  let count = 0;
  let i = 0;
  while (true) {
    const at = text.indexOf(needle, i);
    if (at < 0) break;
    count += 1;
    i = at + needle.length;
  }
  return count;
}

function encodeOffsetCursor(offset: number): string {
  return crypto.base64UrlEncode(JSON.stringify({ o: offset }));
}

function decodeOffsetCursor(cursor: string | undefined): number {
  if (!cursor) return 0;
  try {
    const bytes = new Uint8Array(crypto.base64UrlDecode(cursor));
    let json = '';
    for (let i = 0; i < bytes.length; i++) json += String.fromCharCode(bytes[i]!);
    const decoded = JSON.parse(json) as { o?: number };
    return typeof decoded.o === 'number' && decoded.o >= 0 ? Math.trunc(decoded.o) : 0;
  } catch {
    throw runtimeError('Invalid cursor.', 'validation_error', 400);
  }
}

/** Minimal .gitignore: bare patterns + leading / + trailing / + unanchored *.ext */
async function loadGitignore(root: string): Promise<string[]> {
  try {
    const text = await readTextFile(joinPath(root, '.gitignore'));
    return text
      .split('\n')
      .map((l) => l.trim())
      .filter((l) => l && !l.startsWith('#') && !l.startsWith('!'));
  } catch {
    return [];
  }
}

function isGitignored(relPath: string, patterns: string[]): boolean {
  const rel = relPath.replace(/^\//, '');
  const base = basename(rel);
  for (const pat of patterns) {
    if (pat.endsWith('/')) {
      const dir = pat.slice(0, -1).replace(/^\//, '');
      if (rel === dir || rel.startsWith(`${dir}/`)) return true;
      continue;
    }
    const p = pat.replace(/^\//, '');
    if (p.includes('*') || p.includes('?')) {
      if (globMatch(p, rel) || globMatch(p, base) || globMatch(`**/${p}`, rel)) return true;
    } else if (rel === p || rel.startsWith(`${p}/`) || base === p) {
      return true;
    }
  }
  return false;
}

function parseRgJson(
  stdout: string,
  maxMatches: number,
  maxBytes: number,
): Array<Record<string, unknown>> {
  const matches: Array<Record<string, unknown>> = [];
  let bytes = 0;
  for (const line of stdout.split('\n')) {
    if (!line.trim() || matches.length >= maxMatches || bytes >= maxBytes) break;
    try {
      const obj = JSON.parse(line) as {
        type?: string;
        data?: {
          path?: { text?: string };
          line_number?: number;
          lines?: { text?: string };
        };
      };
      if (obj.type !== 'match' || !obj.data) continue;
      const path = obj.data.path?.text ?? '';
      const text = (obj.data.lines?.text ?? '').replace(/\n$/, '');
      matches.push({
        path,
        line_number: obj.data.line_number ?? 0,
        line: text,
      });
      bytes += text.length + path.length;
    } catch {
      /* ignore non-json */
    }
  }
  return matches;
}

function parsePatch(patch: string): Array<{
  kind: 'add' | 'update' | 'delete';
  path: string;
  content?: string;
  hunks?: string[][];
}> {
  const lines = patch.replace(/\r\n/g, '\n').split('\n');
  const ops: Array<{
    kind: 'add' | 'update' | 'delete';
    path: string;
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
      throw runtimeError('Patch hunk did not match file content.', 'patch_conflict', 409);
    }
    text = text.replace(oldBlock, newBlock);
  }
  return text;
}
