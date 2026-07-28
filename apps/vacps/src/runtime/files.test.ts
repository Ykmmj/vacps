import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

import { applyPatch, filesEdit, filesGlob, filesRead, filesWrite } from './files.js';

describe('files runtime', () => {
  it('reads a line range without marking truncated when the range is complete', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'a.txt');
    await writeFile(path, 'alpha\nbeta\ngamma\n');
    const result = await filesRead({ path, startLine: 2, endLine: 3 });
    expect(result.content).toBe('beta\ngamma');
    expect(result.start_line).toBe(2);
    expect(result.truncated).toBe(false);
    expect(result.next_start_line).toBeNull();
    expect(result.range?.range_complete).toBe(true);
    // Trailing newline yields an empty final line from split('\n').
    expect(result.file?.total_lines).toBe(4);
  });

  it('marks truncated only when max_bytes cuts a requested range short', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'big.txt');
    await writeFile(path, `${'a'.repeat(100)}\n${'b'.repeat(100)}\n`);
    const result = await filesRead({ path, startLine: 1, endLine: 2, maxBytes: 50 });
    expect(result.truncated).toBe(true);
    expect(result.truncation_reason).toBe('max_bytes');
    expect(result.next_start_line).not.toBeNull();
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
});
