import { randomUUID } from 'node:crypto';
import { join } from 'node:path';

import { Annotation, END, START, StateGraph } from '@langchain/langgraph';
import {
  taskToCommand,
  taskWorkingDirectory,
  type TaskDispatch,
  type TaskError,
  type TaskStatus,
} from '@vacps/contracts';

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
    this.abortControllers.set(task.task_id, controller);
    this.store.createTask(task, 'queued');
    try {
      const output = await this.buildGraph(controller.signal).invoke(
        {
          task,
          status: 'queued',
          graphNode: 'queued',
          commandCount: 0,
        },
        { configurable: { thread_id: task.task_id } },
      );
      return {
        taskId: task.task_id,
        status: output.status,
        ...(output.result !== undefined ? { result: output.result } : {}),
        ...(output.error !== undefined ? { error: output.error } : {}),
      };
    } catch (cause: unknown) {
      const error = toTaskError(cause);
      this.store.updateTask(task.task_id, {
        status: 'failed',
        graphNode: 'finalize',
        error,
        finishedAt: new Date().toISOString(),
      });
      return { taskId: task.task_id, status: 'failed', error };
    } finally {
      this.abortControllers.delete(task.task_id);
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
    this.store.updateTask(state.task.task_id, {
      status: 'running',
      graphNode: 'prepare',
      startedAt,
    });
    this.store.saveCheckpoint(state.task.task_id, 'prepare', state);
    return { status: 'running', graphNode: 'prepare' };
  }

  private async authorize(state: TaskGraphState): Promise<Partial<TaskGraphState>> {
    const cwd = taskWorkingDirectory(state.task);
    const command = taskToCommand(state.task);
    const decision = await this.policy.authorize({
      taskId: state.task.task_id,
      profile: state.task.profile,
      command,
      cwd,
    });
    this.store.updateTask(state.task.task_id, { graphNode: 'authorize' });
    this.store.saveCheckpoint(state.task.task_id, 'authorize', state);
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
    this.store.updateTask(state.task.task_id, { graphNode: 'execute' });
    this.store.saveCheckpoint(state.task.task_id, 'execute', state);
    try {
      if (state.task.kind === 'command' || state.task.kind === 'shell') {
        const command = await this.executeProcess(state.task, state.commandCount + 1, signal);
        if (command.status !== 'succeeded') {
          return {
            graphNode: 'execute',
            status: toTaskStatus(command.status),
            error: executionError(command),
            result: {
              kind: 'process',
              exit_code: command.exitCode,
              signal: null,
              timed_out: command.status === 'timed_out',
              failure: {
                category: 'runtime',
                message: executionError(command).message,
                retryable: false,
              },
            },
          };
        }
        return {
          graphNode: 'execute',
          output: command.stdout,
          result: {
            kind: 'process',
            exit_code: command.exitCode,
            signal: null,
            timed_out: false,
            failure: null,
            stdout: command.stdout,
            stderr: command.stderr,
          },
          commandCount: state.commandCount + 1,
        };
      }

      // kind === 'agent'
      let piSequence = state.commandCount;
      const cwd = taskWorkingDirectory(state.task);
      const result = await this.piRuntime.run({
        taskId: state.task.task_id,
        prompt: state.task.prompt,
        cwd,
        timeoutSeconds: state.task.timeout_seconds,
        signal,
        execute: (command) =>
          this.executeShellString(state.task, ++piSequence, command, signal),
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
    this.store.updateTask(state.task.task_id, { graphNode: 'verify' });
    this.store.saveCheckpoint(state.task.task_id, 'verify', state);
    const verify = state.task.verify ?? {
      mode: state.task.kind === 'command' || state.task.kind === 'shell' ? 'exit_code' : 'none',
    };
    if (verify.mode !== 'command') return { graphNode: 'verify' };
    const outcome = await this.executeShellString(
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
    this.store.updateTask(state.task.task_id, {
      status,
      graphNode: 'finalize',
      ...(state.result !== undefined ? { result: state.result } : {}),
      ...(state.error !== undefined ? { error: state.error } : {}),
      finishedAt,
    });
    this.store.saveCheckpoint(state.task.task_id, 'finalize', state);
    return { status, graphNode: 'finalize' };
  }

  /** Run the task's primary process (kind command | shell). */
  private async executeProcess(
    task: TaskDispatch,
    sequence: number,
    signal: AbortSignal,
  ): Promise<ShellExecutionResult> {
    if (task.kind === 'command') {
      return this.runLogged(task, sequence, signal, {
        program: task.program,
        arguments: task.arguments ?? [],
        displayCommand: taskToCommand(task),
      });
    }
    if (task.kind === 'shell') {
      return this.runLogged(task, sequence, signal, {
        shell: task.shell ?? '/bin/bash',
        shellCommand: task.command,
        loadUserEnvironment: task.load_user_environment !== false,
        displayCommand: task.command,
      });
    }
    throw new Error('executeProcess only supports kind command|shell');
  }

  /** Run an arbitrary shell string (agent tools / verify). */
  private async executeShellString(
    task: TaskDispatch,
    sequence: number,
    command: string,
    signal: AbortSignal,
  ): Promise<ShellExecutionResult> {
    return this.runLogged(task, sequence, signal, {
      shell: '/bin/bash',
      shellCommand: command,
      loadUserEnvironment: true,
      displayCommand: command,
    });
  }

  private async runLogged(
    task: TaskDispatch,
    sequence: number,
    signal: AbortSignal,
    spec: {
      program?: string;
      arguments?: string[];
      shell?: string;
      shellCommand?: string;
      loadUserEnvironment?: boolean;
      displayCommand: string;
    },
  ): Promise<ShellExecutionResult> {
    const cwd = taskWorkingDirectory(task);
    const decision = await this.policy.authorize({
      taskId: task.task_id,
      profile: task.profile,
      command: spec.displayCommand,
      cwd,
    });
    if (decision.decision !== 'allow')
      throw new Error(decision.reason ?? 'Command denied by policy.');
    const commandId = randomUUID();
    const commandDirectory = join(this.logDirectory, task.task_id);
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
      taskId: task.task_id,
      sequence,
      command: spec.displayCommand,
      cwd,
      status: 'running',
      stdoutPath,
      stderrPath,
      startedAt,
    });
    const outcome = await this.executor.execute({
      ...(spec.program
        ? { program: spec.program, arguments: spec.arguments ?? [] }
        : {
            shell: spec.shell ?? '/bin/bash',
            command: spec.shellCommand ?? '',
            loadUserEnvironment: spec.loadUserEnvironment !== false,
          }),
      cwd,
      timeoutSeconds: task.timeout_seconds,
      stdoutPath,
      stderrPath,
      signal,
      hardMaxBytes: task.output.hard_max_bytes,
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
