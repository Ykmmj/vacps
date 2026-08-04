import type { BackendMetrics, BackendStatus, BackendSystem } from '@vacps/contracts';
import { File } from 'vacps:fs';
import * as host from 'vacps:host';
import * as process from 'vacps:process';

import type { AgentConfig } from '../config';
import type { TaskStore } from '../storage/task-store';

interface CpuSample {
  idle: number;
  total: number;
}

interface NetSample {
  rx: number;
  tx: number;
  at: number;
}

const textDecoder = new TextDecoder();

async function readTextFile(path: string): Promise<string> {
  const f = await File.open(path, { mode: 'read' });
  try {
    return textDecoder.decode(await f.read());
  } finally {
    await f.close();
  }
}

/**
 * Linux /proc-based telemetry (apps/vacps NodeTelemetryCollector counterpart).
 * No Node.js os module — reads /proc and optional `df`.
 */
export class NativeTelemetryCollector {
  private prevCpu: CpuSample | undefined;
  private prevNet: NetSample | undefined;
  private readonly startedMs = host.nowMs();

  constructor(
    private readonly config: AgentConfig,
    private readonly store: TaskStore,
  ) {}

  uptimeSeconds(): number {
    return Math.max(0, Math.floor((host.nowMs() - this.startedMs) / 1000));
  }

  async collect(): Promise<BackendStatus> {
    const [cpu, memory, disk, network, system, queue] = await Promise.all([
      this.cpuMetrics(),
      this.memoryMetrics(),
      this.diskMetrics(),
      this.networkMetrics(),
      this.systemInfo(),
      Promise.resolve(await this.queueMetrics()),
    ]);

    const metrics: BackendMetrics = {
      cpu,
      memory,
      disk,
      queue,
      ...(network ? { network } : {}),
    };

    return {
      health: {
        ok: true,
        backendId: this.config.BACKEND_ID,
        version: host.version().slice(0, 48) || '0.1.0',
        uptimeSeconds: this.uptimeSeconds(),
        worker: { running: true, concurrency: 1 },
        redis: { connected: false },
        pi: { available: false },
      },
      metrics,
      system,
    };
  }

  private async queueMetrics(): Promise<BackendMetrics['queue']> {
    return this.store.queueCounts();
  }

  private async cpuMetrics(): Promise<BackendMetrics['cpu']> {
    let load1 = 0;
    let cores = 1;
    try {
      const loadavg = await readTextFile('/proc/loadavg');
      load1 = Number(loadavg.trim().split(/\s+/)[0] ?? 0) || 0;
    } catch {
      /* ignore */
    }
    try {
      const stat = await readTextFile('/proc/stat');
      const line = stat.split('\n').find((l) => l.startsWith('cpu '));
      if (line) {
        const parts = line.trim().split(/\s+/).slice(1).map(Number);
        const idle = (parts[3] ?? 0) + (parts[4] ?? 0);
        const total = parts.reduce((a, b) => a + (Number.isFinite(b) ? b : 0), 0);
        const sample = { idle, total };
        let usagePercent = 0;
        if (this.prevCpu && total > this.prevCpu.total) {
          const td = total - this.prevCpu.total;
          const id = idle - this.prevCpu.idle;
          usagePercent = Math.max(0, Math.min(100, ((td - id) / td) * 100));
        }
        this.prevCpu = sample;
        // count cores from cpuN lines
        cores = Math.max(1, stat.split('\n').filter((l) => /^cpu\d+/.test(l)).length);
        return {
          usagePercent: Number(usagePercent.toFixed(1)),
          load1: Number(load1.toFixed(2)),
          cores,
        };
      }
    } catch {
      /* ignore */
    }
    return { usagePercent: 0, load1: Number(load1.toFixed(2)), cores };
  }

  private async memoryMetrics(): Promise<BackendMetrics['memory']> {
    try {
      const text = await readTextFile('/proc/meminfo');
      const get = (key: string) => {
        const m = text.match(new RegExp(`^${key}:\\s+(\\d+)`, 'm'));
        return m ? Number(m[1]) * 1024 : 0;
      };
      const total = get('MemTotal');
      const available = get('MemAvailable') || get('MemFree');
      return {
        totalBytes: total,
        usedBytes: Math.max(0, total - available),
      };
    } catch {
      return { totalBytes: 0, usedBytes: 0 };
    }
  }

  private async diskMetrics(): Promise<BackendMetrics['disk']> {
    // Prefer portable `df -Pk` (POSIX 1024-blocks); fall back to GNU --output.
    try {
      const r = await process.run('df', ['-Pk', '/'], { timeoutMs: 3_000 });
      const line = r.stdout
        .trim()
        .split('\n')
        .map((l) => l.trim())
        .filter(Boolean)
        .pop();
      if (line && !line.toLowerCase().startsWith('filesystem')) {
        const cols = line.split(/\s+/);
        // Filesystem 1024-blocks Used Available Capacity Mounted
        const totalKb = Number(cols[1]);
        const usedKb = Number(cols[2]);
        if (Number.isFinite(totalKb) && totalKb > 0 && Number.isFinite(usedKb)) {
          return { totalBytes: totalKb * 1024, usedBytes: usedKb * 1024 };
        }
      }
    } catch {
      /* ignore */
    }
    try {
      const r = await process.run('df', ['-B1', '--output=size,used', '/'], {
        timeoutMs: 3_000,
      });
      const line = r.stdout
        .trim()
        .split('\n')
        .map((l) => l.trim())
        .filter(Boolean)
        .pop();
      if (line) {
        const cols = line.split(/\s+/).map(Number);
        const size = cols[0] ?? 0;
        const used = cols[1] ?? 0;
        if (Number.isFinite(size) && Number.isFinite(used) && size > 0) {
          return { totalBytes: size, usedBytes: used };
        }
      }
    } catch {
      /* ignore */
    }
    return { totalBytes: 0, usedBytes: 0 };
  }

  private async networkMetrics(): Promise<BackendMetrics['network'] | undefined> {
    try {
      const text = await readTextFile('/proc/net/dev');
      let rx = 0;
      let tx = 0;
      for (const line of text.split('\n')) {
        if (!line.includes(':')) continue;
        if (line.includes('lo:')) continue;
        const body = line.split(':')[1];
        if (!body) continue;
        const cols = body.trim().split(/\s+/).map(Number);
        rx += cols[0] ?? 0;
        tx += cols[8] ?? 0;
      }
      const now = host.nowMs();
      const prev = this.prevNet;
      this.prevNet = { rx, tx, at: now };
      if (!prev || now <= prev.at) {
        return { receivedBytesPerSecond: 0, transmittedBytesPerSecond: 0 };
      }
      const sec = (now - prev.at) / 1000;
      return {
        receivedBytesPerSecond: Math.max(0, Math.round((rx - prev.rx) / sec)),
        transmittedBytesPerSecond: Math.max(0, Math.round((tx - prev.tx) / sec)),
      };
    } catch {
      return undefined;
    }
  }

  private async systemInfo(): Promise<BackendSystem> {
    let kernel = 'unknown';
    try {
      kernel = (await readTextFile('/proc/version')).trim().slice(0, 120) || 'unknown';
      // shorten: first token after "Linux version "
      const m = kernel.match(/Linux version ([^\s]+)/);
      if (m?.[1]) kernel = m[1];
    } catch {
      /* ignore */
    }
    let arch = 'x86_64';
    try {
      const r = await process.run('uname', ['-m'], { timeoutMs: 2_000 });
      if (r.exitCode === 0 && r.stdout.trim()) arch = r.stdout.trim().slice(0, 32);
    } catch {
      /* ignore */
    }
    // /etc is host metadata (not MCP tools); pure vacps:fs may read it.
    let distribution: string | undefined;
    let version: string | undefined;
    try {
      const release = await readTextFile('/etc/os-release');
      const map = new Map<string, string>();
      for (const line of release.split('\n')) {
        const m = line.match(/^([A-Z_]+)=(.*)$/);
        if (!m?.[1]) continue;
        map.set(m[1], (m[2] ?? '').replace(/^"|"$/g, ''));
      }
      const pretty = map.get('PRETTY_NAME') || map.get('NAME');
      if (pretty) distribution = pretty.slice(0, 120);
      const ver = map.get('VERSION_ID');
      if (ver) version = ver.slice(0, 120);
    } catch {
      /* ignore */
    }
    return {
      platform: 'linux',
      ...(distribution ? { distribution } : {}),
      ...(version ? { version } : {}),
      kernel,
      architecture: arch,
    };
  }
}
