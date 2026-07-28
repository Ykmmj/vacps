import { readFile, statfs } from 'node:fs/promises';
import os from 'node:os';

import type { BackendStatus, BackendSystem } from '@vacps/contracts';

import type { AgentConfig } from '../config.js';
import type { PiRuntime } from '../pi/pi-runtime.js';
import type { TaskQueue } from '../queue/task-queue.js';

interface CpuSample {
  idle: number;
  total: number;
}

interface NetworkSample {
  receivedBytes: number;
  transmittedBytes: number;
  capturedAt: number;
}

export class NodeTelemetryCollector {
  private previousCpuSample = cpuSample();
  private previousNetworkSample: NetworkSample | undefined;
  private collecting: Promise<BackendStatus> | undefined;
  private system: Promise<BackendSystem> | undefined;

  constructor(
    private readonly config: AgentConfig,
    private readonly queue: TaskQueue,
    private readonly piRuntime: PiRuntime,
  ) {}

  async collect(): Promise<BackendStatus> {
    if (!this.collecting) {
      this.collecting = this.collectFresh().finally(() => {
        this.collecting = undefined;
      });
    }
    return this.collecting;
  }

  private async collectFresh(): Promise<BackendStatus> {
    const [queue, disk, network, system, pi] = await Promise.all([
      this.queue.metrics(),
      diskMetrics(),
      this.networkMetrics(),
      this.systemInfo(),
      this.piRuntime.availability(),
    ]);
    const currentCpuSample = cpuSample();
    const totalDelta = currentCpuSample.total - this.previousCpuSample.total;
    const idleDelta = currentCpuSample.idle - this.previousCpuSample.idle;
    this.previousCpuSample = currentCpuSample;
    const usagePercent =
      totalDelta > 0
        ? Math.max(0, Math.min(100, ((totalDelta - idleDelta) / totalDelta) * 100))
        : 0;

    return {
      health: {
        ok: true,
        backendId: this.config.BACKEND_ID,
        version: '0.1.0',
        uptimeSeconds: Math.floor(process.uptime()),
        worker: {
          running: this.queue.isWorkerRunning(),
          concurrency: this.config.WORKER_CONCURRENCY,
        },
        redis: { connected: this.queue.isRedisConnected() },
        pi,
      },
      metrics: {
        cpu: {
          usagePercent: Number(usagePercent.toFixed(1)),
          load1: Number((os.loadavg()[0] ?? 0).toFixed(2)),
          cores: os.cpus().length,
        },
        memory: { totalBytes: os.totalmem(), usedBytes: os.totalmem() - os.freemem() },
        disk,
        ...(network ? { network } : {}),
        queue,
      },
      system,
    };
  }

  private async networkMetrics() {
    const current = await networkSample();
    if (!current) return undefined;
    const previous = this.previousNetworkSample;
    this.previousNetworkSample = current;
    if (!previous) return { receivedBytesPerSecond: 0, transmittedBytesPerSecond: 0 };
    const elapsedSeconds = (current.capturedAt - previous.capturedAt) / 1000;
    if (elapsedSeconds <= 0) return { receivedBytesPerSecond: 0, transmittedBytesPerSecond: 0 };
    return {
      receivedBytesPerSecond: Math.max(
        0,
        Math.round((current.receivedBytes - previous.receivedBytes) / elapsedSeconds),
      ),
      transmittedBytesPerSecond: Math.max(
        0,
        Math.round((current.transmittedBytes - previous.transmittedBytes) / elapsedSeconds),
      ),
    };
  }

  private systemInfo(): Promise<BackendSystem> {
    this.system ??= readSystemInfo();
    return this.system;
  }
}

function cpuSample(): CpuSample {
  return os.cpus().reduce(
    (sample, cpu) => {
      const total = Object.values(cpu.times).reduce((sum, value) => sum + value, 0);
      return { idle: sample.idle + cpu.times.idle, total: sample.total + total };
    },
    { idle: 0, total: 0 },
  );
}

async function diskMetrics(): Promise<{ totalBytes: number; usedBytes: number }> {
  try {
    const filesystem = await statfs('/');
    const totalBytes = Number(filesystem.blocks) * Number(filesystem.bsize);
    const availableBytes = Number(filesystem.bavail) * Number(filesystem.bsize);
    return {
      totalBytes: Math.max(0, totalBytes),
      usedBytes: Math.max(0, totalBytes - availableBytes),
    };
  } catch {
    return { totalBytes: 0, usedBytes: 0 };
  }
}

async function networkSample(): Promise<NetworkSample | undefined> {
  try {
    const source = await readFile('/proc/net/dev', 'utf8');
    const counters = source
      .split('\n')
      .slice(2)
      .map((line) => line.trim())
      .filter(Boolean)
      .reduce(
        (total, line) => {
          const [interfaceName, values] = line.split(':', 2);
          if (!interfaceName || !values || interfaceName.trim() === 'lo') return total;
          const columns = values.trim().split(/\s+/).map(Number);
          return {
            receivedBytes: total.receivedBytes + (columns[0] ?? 0),
            transmittedBytes: total.transmittedBytes + (columns[8] ?? 0),
          };
        },
        { receivedBytes: 0, transmittedBytes: 0 },
      );
    return { ...counters, capturedAt: Date.now() };
  } catch {
    return undefined;
  }
}

async function readSystemInfo(): Promise<BackendSystem> {
  const values = await readOsRelease();
  return {
    platform: os.platform(),
    ...(values.get('PRETTY_NAME') || values.get('NAME')
      ? { distribution: values.get('PRETTY_NAME') ?? values.get('NAME') }
      : {}),
    ...(values.get('VERSION_ID') ? { version: values.get('VERSION_ID') } : {}),
    kernel: os.release(),
    architecture: os.arch(),
  };
}

async function readOsRelease(): Promise<Map<string, string>> {
  try {
    const source = await readFile('/etc/os-release', 'utf8');
    return new Map(
      source
        .split('\n')
        .map((line) => line.match(/^([A-Z_]+)=(.*)$/))
        .filter((match): match is RegExpMatchArray => Boolean(match))
        .map((match) => [match[1]!, match[2]!.replace(/^"|"$/g, '')]),
    );
  } catch {
    return new Map();
  }
}
