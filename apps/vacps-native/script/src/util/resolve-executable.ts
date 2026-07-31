import * as fs from 'vacps:fs';
import * as host from 'vacps:host';

/** Resolve bare program names via common absolute paths (static agent often has empty PATH). */
export async function resolveExecutable(program: string): Promise<string> {
  if (!program || program.includes('/')) return program;
  const candidates: string[] = [
    `/usr/bin/${program}`,
    `/bin/${program}`,
    `/usr/local/bin/${program}`,
  ];
  try {
    const pathEnv = host.getenv('PATH') ?? '';
    if (pathEnv) {
      for (const dir of pathEnv.split(':')) {
        if (!dir) continue;
        const candidate = `${dir.replace(/\/$/, '')}/${program}`;
        if (!candidates.includes(candidate)) candidates.push(candidate);
      }
    }
  } catch {
    /* ignore */
  }
  for (const c of candidates) {
    try {
      if (await fs.exists(c)) {
        const st = await fs.stat(c);
        if (st.type === 'file' || st.type === 'symlink') return c;
      }
    } catch {
      /* try next */
    }
  }
  // Prefer absolute path so Boost.Process does not depend on PATH.
  return `/usr/bin/${program}`;
}
