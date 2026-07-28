import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

import { applyPatch, filesEdit, filesRead, filesWrite } from './files.js';

describe('files runtime', () => {
  it('reads a line range with byte limits', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'vacps-files-'));
    const path = join(dir, 'a.txt');
    await writeFile(path, 'one\ntwo\nthree\nfour\n');
    const result = await filesRead({ path, startLine: 2, endLine: 3 });
    expect(result.content).toBe('two\nthree');
    expect(result.start_line).toBe(2);
    expect(result.total_lines).toBe(5);
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
