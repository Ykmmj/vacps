import * as fs from "vacps:fs";
import * as host from "vacps:host";
import * as process from "vacps:process";

/** Process user home/shell probe (not Pi / task.kind). */
export interface ShellEnvironment {
  uid: number;
  gid: number;
  user: string;
  home: string;
  shell: string;
  home_accessible: boolean;
  home_writable: boolean;
  bashrc_readable: boolean;
  shell_smoke_ok: boolean;
  bashrc_path: string;
  notes: string[];
}

/**
 * Probe HOME / bash login environment for shell.exec health.
 */
export async function probeShellEnvironment(): Promise<ShellEnvironment> {
  const user = host.getenv("USER") ?? host.getenv("LOGNAME") ?? "vacps";
  const home = host.getenv("HOME") ?? `/home/${user}`;
  const shell = host.getenv("SHELL") ?? "/bin/bash";
  const bashrc = `${home}/.bashrc`;
  const notes: string[] = [];

  let home_accessible = false;
  let home_writable = false;
  let bashrc_readable = false;

  try {
    const st = await fs.stat(home);
    home_accessible = st.type === "directory" && st.readable;
    home_writable = st.writable;
    if (!home_accessible) {
      notes.push(`HOME ${home} is not accessible.`);
    }
    if (!home_writable) {
      notes.push(`HOME ${home} is not writable.`);
    }
  } catch {
    notes.push(
      `HOME ${home} is not accessible (check /home mode and systemd ProtectHome/BindPaths).`,
    );
  }

  try {
    const st = await fs.stat(bashrc);
    bashrc_readable = st.readable;
    if (!bashrc_readable) {
      notes.push(`${bashrc} is not readable; shell login env may emit Permission denied.`);
    }
  } catch {
    notes.push(`${bashrc} is not readable; shell login env may emit Permission denied.`);
  }

  let shell_smoke_ok = false;
  let uid = 0;
  let gid = 0;
  try {
    const r = await process.run(
      ["/bin/bash", "-lc", 'test -n "$HOME" && test -x "$HOME" && id -un && id -u && id -g'],
      { timeoutMs: 3_000 },
    );
    if (r.stderr.includes("Permission denied")) {
      notes.push(`bash -lc reported: ${r.stderr.trim()}`);
    } else if (r.exitCode === 0 && r.stdout.trim()) {
      shell_smoke_ok = true;
      const lines = r.stdout.trim().split("\n");
      if (lines.length >= 3) {
        uid = Number(lines[1]) || 0;
        gid = Number(lines[2]) || 0;
      }
    }
  } catch (e) {
    notes.push(`shell smoke failed: ${e instanceof Error ? e.message : String(e)}`);
  }

  return {
    uid,
    gid,
    user,
    home,
    shell,
    home_accessible,
    home_writable,
    bashrc_readable,
    shell_smoke_ok,
    bashrc_path: bashrc,
    notes,
  };
}
