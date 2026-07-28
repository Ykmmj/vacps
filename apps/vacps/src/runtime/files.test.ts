import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

import {
  applyPatch,
  filesEdit,
  filesGlob,
  filesGrep,
  filesList,
  filesRead,
  filesWrite,
} from './files.js';

describe('files runtime', () => {
  it('reads a line range without marking truncated when the range is complete', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'a.txt');
    await writeFile(path, 'alpha\nbeta\ngamma\n');
    const result = await filesRead({ path, startLine: 2, endLine: 3 });
    expect(result.content).toBe('beta\ngamma');
    expect(result.range?.returned_start_line).toBe(2);
    expect(result.range?.truncated).toBe(false);
    expect(result.range?.next_start_line).toBeNull();
    expect(result.range?.complete).toBe(true);
    expect(result.range?.range_complete).toBe(true);
    // Schema v2: no duplicated top-level total_lines / total_bytes.
    expect((result as { total_lines?: unknown }).total_lines).toBeUndefined();
    // Trailing newline yields an empty final line from split('\n').
    expect(result.file?.total_lines).toBe(4);
    expect(result.file?.size_bytes).toBeGreaterThan(0);
    expect(result.file?.sha256).toBeTruthy();
  });

  it('marks truncated only when max_bytes cuts a requested range short', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'big.txt');
    await writeFile(path, `${'a'.repeat(100)}\n${'b'.repeat(100)}\n`);
    const result = await filesRead({ path, startLine: 1, endLine: 2, maxBytes: 50 });
    expect(result.range?.truncated).toBe(true);
    expect(result.range?.truncation_reason).toBe('max_bytes');
    expect(result.range?.next_start_line).not.toBeNull();
  });

  it('rejects end_line < start_line with invalid_line_range', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'a.txt');
    await writeFile(path, 'a\nb\n');
    await expect(filesRead({ path, startLine: 3, endLine: 1 })).rejects.toMatchObject({
      code: 'invalid_line_range',
      statusCode: 400,
    });
  });

  it('matches root files with **/*.txt globstar dialect', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const { mkdir } = await import('node:fs/promises');
    await writeFile(join(dir, 'example.txt'), 'x\n');
    await mkdir(join(dir, 'nested'), { recursive: true });
    await writeFile(join(dir, 'nested', 'deep.txt'), 'y\n');
    const result = await filesGlob({ pattern: '**/*.txt', path: dir, respectGitignore: false });
    const names = result.matches.map((item) => String((item as { path: string }).path));
    expect(names.some((name) => name.endsWith('example.txt'))).toBe(true);
    expect(names.some((name) => name.endsWith('deep.txt'))).toBe(true);
  });

  it('pages glob results with next_cursor when truncated', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    for (const name of ['a.txt', 'b.txt', 'c.txt']) {
      await writeFile(join(dir, name), 'x\n');
    }
    const first = await filesGlob({
      pattern: '**/*',
      path: dir,
      limit: 1,
      respectGitignore: false,
    });
    expect(first.returned_count).toBe(1);
    expect(first.truncated).toBe(true);
    expect(first.next_cursor).toBeTruthy();
    const second = await filesGlob({
      pattern: '**/*',
      path: dir,
      limit: 10,
      cursor: first.next_cursor!,
      respectGitignore: false,
    });
    expect(second.returned_count).toBeGreaterThanOrEqual(1);
    expect(second.matches[0]).not.toEqual(first.matches[0]);
  });

  it('lists entries without matches alias', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    await writeFile(join(dir, 'only.txt'), 'x\n');
    const result = await filesList({ path: dir, limit: 10 });
    expect(result.entries.length).toBe(1);
    expect((result as { matches?: unknown }).matches).toBeUndefined();
  });

  it('edits unique text and rejects non-unique without replace_all', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'b.txt');
    await writeFile(path, 'alpha beta alpha\n');
    await expect(filesEdit({ path, oldText: 'alpha', newText: 'A' })).rejects.toMatchObject({
      code: 'old_text_not_unique',
    });
    const edited = await filesEdit({ path, oldText: 'alpha', newText: 'A', replaceAll: true });
    expect(edited.replacement_count).toBe(2);
    expect(await readFile(path, 'utf8')).toBe('A beta A\n');
  });

  it('writes atomically and applies a simple patch', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'c.txt');
    await filesWrite({ path, content: 'hello\n', mode: 'create' });
    const patched = await applyPatch({
      workspacePath: dir,
      patch: `*** Begin Patch\n*** Update File: c.txt\n@@\n-hello\n+world\n*** End Patch\n`,
    });
    expect(patched.applied).toBe(true);
    expect(await readFile(path, 'utf8')).toBe('world\n');
  });

  it('greps a file path directly (not only directories)', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'os-release-like.txt');
    await writeFile(path, 'PRETTY_NAME="Ubuntu 24.04.4 LTS"\nNAME="Ubuntu"\n');
    const result = await filesGrep({ pattern: 'Ubuntu', path, caseSensitive: true });
    expect(result.match_count).toBeGreaterThanOrEqual(1);
    expect(result.matches.some((m) => String((m as { line: string }).line).includes('Ubuntu'))).toBe(
      true,
    );
  });

  it('rejects missing grep paths with path_not_found (not internal)', async () => {
    await expect(
      filesGrep({ pattern: 'x', path: '/tmp/vacps-definitely-missing-' + Date.now() }),
    ).rejects.toMatchObject({ code: 'path_not_found', statusCode: 400 });
  });

  it('pages directory listing with opaque cursor', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const { mkdir } = await import('node:fs/promises');
    for (const name of ['a.txt', 'b.txt', 'c.txt']) {
      await writeFile(join(dir, name), 'x\n');
    }
    await mkdir(join(dir, 'sub'), { recursive: true });
    const first = await filesList({ path: dir, limit: 2 });
    expect(first.returned_count).toBe(2);
    expect(first.next_cursor).toBeTruthy();
    const second = await filesList({ path: dir, limit: 2, cursor: first.next_cursor! });
    expect(second.returned_count).toBeGreaterThanOrEqual(1);
    const allNames = [
      ...first.entries.map((m) => String((m as { name: string }).name)),
      ...second.entries.map((m) => String((m as { name: string }).name)),
    ];
    expect(new Set(allNames).size).toBe(allNames.length);
  });
});
