import { spawn } from 'node:child_process';

import type { ShellExecutionResult } from '../executor/shell-executor.js';

export interface PiRuntimeInput {
  taskId: string;
  prompt: string;
  cwd: string;
  timeoutSeconds: number;
  execute: (command: string) => Promise<ShellExecutionResult>;
  signal?: AbortSignal;
}

export interface PiRuntimeResult {
  report: string;
  commandsExecuted: number;
}

type PiMessage =
  | { type: 'exec'; id: string; command: string }
  | { type: 'final'; report: string }
  | { type: 'error'; message: string };

/**
 * The Pi process is deliberately isolated behind an NDJSON protocol. A small
 * Pi SDK/CLI adapter emits `exec` requests and receives their outputs over
 * stdin, so every command still passes through CommandPolicy and ShellExecutor.
 */
export class PiRuntime {
  constructor(
    private readonly command: string,
    private readonly commandArgs: string[],
  ) {}

  async availability(): Promise<{ available: boolean; version?: string }> {
    try {
      const output = await this.runProcess(['--version'], undefined, 5_000);
      return { available: true, version: output.trim().slice(0, 200) };
    } catch {
      return { available: false };
    }
  }

  async run(input: PiRuntimeInput): Promise<PiRuntimeResult> {
    const child = spawn(this.command, this.commandArgs, {
      cwd: input.cwd,
      stdio: ['pipe', 'pipe', 'pipe'],
      env: process.env,
    });
    const timeout = setTimeout(() => child.kill('SIGTERM'), input.timeoutSeconds * 1000);
    input.signal?.addEventListener('abort', () => child.kill('SIGTERM'), { once: true });
    child.stdin.write(
      `${JSON.stringify({ type: 'task', taskId: input.taskId, prompt: input.prompt })}\n`,
    );

    let buffer = '';
    let report: string | undefined;
    let commandsExecuted = 0;
    let stderr = '';
    const pending: Promise<void>[] = [];
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (chunk: string) => {
      buffer += chunk;
      const lines = buffer.split('\n');
      buffer = lines.pop() ?? '';
      for (const line of lines) {
        if (!line.trim()) continue;
        pending.push(
          this.handleMessage(line, input, child.stdin).then((outcome) => {
            commandsExecuted += outcome.commandsExecuted;
            if (outcome.report) report = outcome.report;
          }),
        );
      }
    });
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk: string) => {
      stderr += chunk;
    });

    const code = await new Promise<number | null>((resolve, reject) => {
      child.once('error', reject);
      child.once('close', resolve);
    }).finally(() => clearTimeout(timeout));
    await Promise.all(pending);
    if (buffer.trim()) {
      const outcome = await this.handleMessage(buffer, input, child.stdin);
      commandsExecuted += outcome.commandsExecuted;
      report ??= outcome.report;
    }
    if (code !== 0)
      throw new Error(`Pi adapter exited with code ${code ?? 'unknown'}: ${stderr.slice(-500)}`);
    if (!report) throw new Error('Pi adapter completed without a final report.');
    return { report, commandsExecuted };
  }

  private async handleMessage(
    line: string,
    input: PiRuntimeInput,
    stdin: NodeJS.WritableStream,
  ): Promise<{ commandsExecuted: number; report?: string }> {
    const message = JSON.parse(line) as PiMessage;
    if (message.type === 'final') return { commandsExecuted: 0, report: message.report };
    if (message.type === 'error') throw new Error(`Pi adapter error: ${message.message}`);
    if (message.type !== 'exec') throw new Error('Pi adapter emitted an unknown protocol message.');
    const result = await input.execute(message.command);
    stdin.write(`${JSON.stringify({ type: 'exec_result', id: message.id, result })}\n`);
    return { commandsExecuted: 1 };
  }

  private async runProcess(
    args: string[],
    cwd: string | undefined,
    timeoutMs: number,
  ): Promise<string> {
    const child = spawn(this.command, args, { cwd, stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    let stderr = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk: string) => (output += chunk));
    child.stderr.on('data', (chunk: string) => (stderr += chunk));
    const timer = setTimeout(() => child.kill('SIGTERM'), timeoutMs);
    const code = await new Promise<number | null>((resolve, reject) => {
      child.once('error', reject);
      child.once('close', resolve);
    }).finally(() => clearTimeout(timer));
    if (code !== 0) throw new Error(stderr || `Pi exited with ${code ?? 'unknown'}`);
    return output;
  }
}
