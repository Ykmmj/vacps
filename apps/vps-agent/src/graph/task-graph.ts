import { randomUUID } from 'node:crypto';
import { join } from 'node:path';

import { Annotation, END, START, StateGraph } from '@langchain/langgraph';
import type { TaskDispatch, TaskError, TaskStatus } from '@vps-agent/contracts';

import type { ShellExecutionResult } from '../executor/shell-executor.js';
import type { ShellExecutor } from '../executor/shell-executor.js';
import type { PiRuntime } from '../pi/pi-runtime.js';
import type { CommandPolicy } from '../policy/full-access-policy.js';
import type { TaskStore } from '../storage/task-store.js';

const TaskGraphAnnotation = Annotation.Root({
  task: Annotation<TaskDispatch>,
  status: Annotation<TaskStatus>,
  graphNode: Annotation<string>,
  output: Annotation<string | undefined>,
  result: Annotation<unknown>,
  error: Annotation<TaskError | undefined>,
  commandCount: Annotation<number>,
});

type TaskGraphState = typeof TaskGraphAnnotation.State;

export interface TaskRunResult {
  taskId: string;
  status: TaskStatus;
  result?: unknown;
  error?: TaskError;
}

export class TaskGraphRunner {
  private readonly abortControllers = new Map<string, AbortController>();

  constructor(
    private readonly store: TaskStore,
    private readonly executor: ShellExecutor,
    private readonly policy: CommandPolicy,
    private readonly piRuntime: PiRuntime,
    private readonly logDirectory: string,
  ) {}

  async run(task: TaskDispatch): Promise<TaskRunResult> {
    const controller = new AbortController();
    this.abortControllers.set(task.taskId, controller);
    this.store.createTask(task, 'queued');
    try {
      const output = await this.buildGraph(controller.signal).invoke(
        {
          task,
          status: 'queued',
          graphNode: 'queued',
          commandCount: 0,
        },
        { configurable: { thread_id: task.taskId } },
      );
      return {
        taskId: task.taskId,
        status: output.status,
        ...(output.result !== undefined ? { result: output.result } : {}),
        ...(output.error !== undefined ? { error: output.error } : {}),
      };
    } catch (cause: unknown) {
      const error = toTaskError(cause);
      this.store.updateTask(task.taskId, {
        status: 'failed',
        graphNode: 'finalize',
        error,
        finishedAt: new Date().toISOString(),
      });
      return { taskId: task.taskId, status: 'failed', error };
    } finally {
      this.abortControllers.delete(task.taskId);
    }
  }

  cancel(taskId: string): boolean {
    const controller = this.abortControllers.get(taskId);
    if (!controller) return false;
    controller.abort();
    return true;
  }

  private buildGraph(signal: AbortSignal) {
    return new StateGraph(TaskGraphAnnotation)
      .addNode('prepare', async (state) => this.prepare(state))
      .addNode('authorize', async (state) => this.authorize(state))
      .addNode('execute', async (state) => this.execute(state, signal))
      .addNode('verify', async (state) => this.verify(state, signal))
      .addNode('finalize', async (state) => this.finalize(state))
      .addEdge(START, 'prepare')
      .addEdge('prepare', 'authorize')
      .addConditionalEdges('authorize', (state) => (state.error ? 'finalize' : 'execute'), {
        execute: 'execute',
        finalize: 'finalize',
      })
      .addConditionalEdges('execute', (state) => (state.error ? 'finalize' : 'verify'), {
        verify: 'verify',
        finalize: 'finalize',
      })
      .addEdge('verify', 'finalize')
      .addEdge('finalize', END)
      .compile();
  }

  private async prepare(state: TaskGraphState): Promise<Partial<TaskGraphState>> {
    const startedAt = new Date().toISOString();
    this.store.updateTask(state.task.taskId, {
      status: 'running',
      graphNode: 'prepare',
      startedAt,
    });
    this.store.saveCheckpoint(state.task.taskId, 'prepare', state);
    return { status: 'running', graphNode: 'prepare' };
  }

  private async authorize(state: TaskGraphState): Promise<Partial<TaskGraphState>> {
    const command = state.task.type === 'shell' ? state.task.command : '(Pi agent task)';
    const decision = await this.policy.authorize({
      taskId: state.task.taskId,
      profile: state.task.profile,
      command,
      cwd: state.task.cwd,
    });
    this.store.updateTask(state.task.taskId, { graphNode: 'authorize' });
    this.store.saveCheckpoint(state.task.taskId, 'authorize', state);
    if (decision.decision !== 'allow') {
      return {
        graphNode: 'authorize',
        status: decision.decision === 'approval_required' ? 'waiting_for_approval' : 'failed',
        error: { code: decision.decision, message: decision.reason ?? 'Task was not authorized.' },
      };
    }
    return { graphNode: 'authorize' };
  }

  private async execute(
    state: TaskGraphState,
    signal: AbortSignal,
  ): Promise<Partial<TaskGraphState>> {
    this.store.updateTask(state.task.taskId, { graphNode: 'execute' });
    this.store.saveCheckpoint(state.task.taskId, 'execute', state);
    try {
      if (state.task.type === 'shell') {
        const command = await this.executeCommand(
          state.task,
          state.commandCount + 1,
          state.task.command,
          signal,
        );
        if (command.status !== 'succeeded') {
          return {
            graphNode: 'execute',
            status: toTaskStatus(command.status),
            error: executionError(command),
          };
        }
        return {
          graphNode: 'execute',
          output: command.stdout,
          result: { exitCode: command.exitCode, stdout: command.stdout, stderr: command.stderr },
          commandCount: state.commandCount + 1,
        };
      }
      let piSequence = state.commandCount;
      const result = await this.piRuntime.run({
        taskId: state.task.taskId,
        prompt: state.task.prompt,
        cwd: state.task.cwd,
        timeoutSeconds: state.task.timeoutSeconds,
        signal,
        execute: (command) => this.executeCommand(state.task, ++piSequence, command, signal),
      });
      return {
        graphNode: 'execute',
        output: result.report,
        result: result,
        commandCount: piSequence,
      };
    } catch (cause: unknown) {
      return {
        graphNode: 'execute',
        status: signal.aborted ? 'cancelled' : 'failed',
        error: toTaskError(cause),
      };
    }
  }

  private async verify(
    state: TaskGraphState,
    signal: AbortSignal,
  ): Promise<Partial<TaskGraphState>> {
    this.store.updateTask(state.task.taskId, { graphNode: 'verify' });
    this.store.saveCheckpoint(state.task.taskId, 'verify', state);
    const verify = state.task.verify ?? {
      mode: state.task.type === 'shell' ? 'exit_code' : 'none',
    };
    if (verify.mode !== 'command') return { graphNode: 'verify' };
    const outcome = await this.executeCommand(
      state.task,
      state.commandCount + 1,
      verify.command,
      signal,
    );
    if (outcome.status !== 'succeeded') {
      return {
        graphNode: 'verify',
        status: toTaskStatus(outcome.status),
        error: executionError(outcome),
      };
    }
    return { graphNode: 'verify', commandCount: state.commandCount + 1 };
  }

  private async finalize(state: TaskGraphState): Promise<Partial<TaskGraphState>> {
    const status = state.error
      ? state.status === 'running'
        ? 'failed'
        : state.status
      : 'succeeded';
    const finishedAt = new Date().toISOString();
    this.store.updateTask(state.task.taskId, {
      status,
      graphNode: 'finalize',
      ...(state.result !== undefined ? { result: state.result } : {}),
      ...(state.error !== undefined ? { error: state.error } : {}),
      finishedAt,
    });
    this.store.saveCheckpoint(state.task.taskId, 'finalize', state);
    return { status, graphNode: 'finalize' };
  }

  private async executeCommand(
    task: TaskDispatch,
    sequence: number,
    command: string,
    signal: AbortSignal,
  ): Promise<ShellExecutionResult> {
    const decision = await this.policy.authorize({
      taskId: task.taskId,
      profile: task.profile,
      command,
      cwd: task.cwd,
    });
    if (decision.decision !== 'allow')
      throw new Error(decision.reason ?? 'Command denied by policy.');
    const commandId = randomUUID();
    const commandDirectory = join(this.logDirectory, task.taskId);
    const stdoutPath = join(
      commandDirectory,
      `${String(sequence).padStart(3, '0')}-${commandId}.stdout.log`,
    );
    const stderrPath = join(
      commandDirectory,
      `${String(sequence).padStart(3, '0')}-${commandId}.stderr.log`,
    );
    const startedAt = new Date().toISOString();
    this.store.startCommand({
      id: commandId,
      taskId: task.taskId,
      sequence,
      command,
      cwd: task.cwd,
      status: 'running',
      stdoutPath,
      stderrPath,
      startedAt,
    });
    const outcome = await this.executor.execute({
      command,
      cwd: task.cwd,
      timeoutSeconds: task.timeoutSeconds,
      stdoutPath,
      stderrPath,
      signal,
    });
    this.store.finishCommand({
      id: commandId,
      status: outcome.status,
      exitCode: outcome.exitCode,
      finishedAt: new Date().toISOString(),
    });
    return outcome;
  }
}

function toTaskStatus(status: ShellExecutionResult['status']): TaskStatus {
  if (status === 'cancelled') return 'cancelled';
  if (status === 'timed_out') return 'timed_out';
  return 'failed';
}

function executionError(outcome: ShellExecutionResult): TaskError {
  return {
    code: outcome.status,
    message: `Command ended with status ${outcome.status} (exit ${outcome.exitCode ?? 'signal'}).`,
  };
}

function toTaskError(cause: unknown): TaskError {
  return {
    code: 'execution_failed',
    message: cause instanceof Error ? cause.message : String(cause),
  };
}
