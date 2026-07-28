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
  const buffer = await readFile(path);
  const totalBytes = buffer.length;
  const digest = sha256Hex(buffer);
  const info = await stat(path);

  if (encoding === 'base64') {
    const slice = buffer.subarray(0, maxBytes);
    return {
      path,
      kind: 'binary' as const,
      content: slice.toString('base64'),
      encoding: 'base64' as const,
      start_line: null,
      end_line: null,
      total_lines: null,
      total_bytes: totalBytes,
      returned_bytes: slice.length,
      truncated: slice.length < totalBytes,
      next_start_line: null,
      sha256: digest,
      modified_at: info.mtime.toISOString(),
    };
  }

  const text = buffer.toString('utf8');
  const lines = text.split('\n');
  const start = clamp(input.startLine ?? 1, 1, Math.max(1, lines.length));
  let end = input.endLine ?? lines.length;
  end = clamp(end, start, lines.length);

  let selected = lines.slice(start - 1, end);
  let content = selected.join('\n');
  let truncated = false;
  let nextStart: number | null = null;

  while (Buffer.byteLength(content, 'utf8') > maxBytes && selected.length > 1) {
    selected = selected.slice(0, -1);
    content = selected.join('\n');
    truncated = true;
    nextStart = start + selected.length;
  }
  if (Buffer.byteLength(content, 'utf8') > maxBytes) {
    content = Buffer.from(content, 'utf8').subarray(0, maxBytes).toString('utf8');
    truncated = true;
    nextStart = start + 1;
  }
  if (!truncated && end < lines.length) {
    truncated = true;
    nextStart = end + 1;
  }

  return {
    path,
    kind: 'text' as const,
    content,
    encoding: 'utf-8' as const,
    start_line: start,
    end_line: start + selected.length - 1,
    total_lines: lines.length,
    total_bytes: totalBytes,
    returned_bytes: Buffer.byteLength(content, 'utf8'),
    truncated,
    next_start_line: truncated ? nextStart : null,
    sha256: digest,
    modified_at: info.mtime.toISOString(),
  };
}

export async function filesList(input: {
  path: string;
  limit?: number | undefined;
  includeHidden?: boolean | undefined;
}) {
  const path = assertSafeAbsolutePath(input.path);
  const limit = clamp(input.limit ?? 200, 1, 2000);
  const includeHidden = Boolean(input.includeHidden);
  const entries = await readdir(path, { withFileTypes: true });
  const matches = [];
  for (const entry of entries) {
    if (!includeHidden && entry.name.startsWith('.')) continue;
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
    if (matches.length >= limit) break;
  }
  return {
    path,
    matches,
    returned_count: matches.length,
    truncated: entries.length > matches.length,
    next_cursor: null,
  };
}

export async function filesGlob(input: {
  pattern: string;
  path?: string | undefined;
  includeHidden?: boolean | undefined;
  respectGitignore?: boolean | undefined;
  limit?: number | undefined;
}) {
  const root = assertSafeAbsolutePath(input.path ?? process.cwd());
  const limit = clamp(input.limit ?? 200, 1, 2000);
  const includeHidden = Boolean(input.includeHidden);
  const respectGitignore = input.respectGitignore !== false;
  const pattern = input.pattern;

  // Prefer ripgrep file listing when available.
  try {
    const args = ['--files', '--glob', pattern];
    if (!includeHidden) args.push('--glob', '!.*/**');
    if (!respectGitignore) args.push('--no-ignore');
    const listed = await runCapture('rg', args, root, 30_000);
    if (listed.exitCode === 0 || listed.stdout) {
      const lines = listed.stdout
        .split('\n')
        .map((line) => line.trim())
        .filter(Boolean)
        .slice(0, limit + 1);
      const truncated = lines.length > limit;
      const matches = [];
      for (const rel of lines.slice(0, limit)) {
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
      return { matches, returned_count: matches.length, truncated, next_cursor: null };
    }
  } catch {
    /* fallback */
  }

  const matches: Array<Record<string, unknown>> = [];
  await walk(root, root, includeHidden, async (full, info: Stats) => {
    const rel = relative(root, full).split(sep).join('/');
    if (!globMatch(pattern, rel) && !globMatch(pattern, basename(full))) return;
    matches.push({
      path: full,
      type: info.isDirectory() ? 'directory' : 'file',
      size_bytes: info.size,
      modified_at: info.mtime.toISOString(),
    });
  });
  const truncated = matches.length > limit;
  return {
    matches: matches.slice(0, limit),
    returned_count: Math.min(matches.length, limit),
    truncated,
    next_cursor: null,
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
  const root = assertSafeAbsolutePath(input.path ?? process.cwd());
  const maxMatches = clamp(input.maxMatches ?? 100, 1, 500);
  const contextBefore = clamp(input.contextBefore ?? 0, 0, 10);
  const contextAfter = clamp(input.contextAfter ?? 0, 0, 10);
  const maxBytes = clamp(input.maxBytes ?? 64_000, 1, 256 * 1024);

  const args = ['--json', '--line-number', '--column', `--max-count=${maxMatches}`];
  if (!input.caseSensitive) args.push('-i');
  if (input.fixedString) args.push('-F');
  if (contextBefore) args.push(`-B${contextBefore}`);
  if (contextAfter) args.push(`-A${contextAfter}`);
  if (input.filePattern) args.push('--glob', input.filePattern);
  args.push('--', input.pattern, root);

  const result = await runCapture('rg', args, root, 60_000);
  const matches: Array<Record<string, unknown>> = [];
  let bytes = 0;
  for (const line of result.stdout.split('\n')) {
    if (!line.trim()) continue;
    let parsed: {
      type?: string;
      data?: {
        path?: { text?: string };
        line_number?: number;
        absolute_offset?: number;
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
    matches.push({
      path: parsed.data.path?.text ?? root,
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
}) {
  const from = assertSafeAbsolutePath(input.from);
  const to = assertSafeAbsolutePath(input.to);
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

export async function filesDelete(input: { path: string; recursive?: boolean | undefined }) {
  const path = assertSafeAbsolutePath(input.path);
  await rm(path, { recursive: Boolean(input.recursive), force: false });
  return { path, operation: 'deleted' };
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

function globMatch(pattern: string, value: string): boolean {
  // Minimal glob: * ** and ?
  const escaped = pattern
    .replace(/[.+^${}()|[\]\\]/g, '\\$&')
    .replace(/\*\*/g, '::DOUBLE::')
    .replace(/\*/g, '[^/]*')
    .replace(/::DOUBLE::/g, '.*')
    .replace(/\?/g, '.');
  return new RegExp(`^${escaped}$`).test(value);
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
