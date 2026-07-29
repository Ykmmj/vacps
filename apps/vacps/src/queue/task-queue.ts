import { randomUUID } from 'node:crypto';

import { Queue, QueueEvents, Worker, type Job } from 'bullmq';
import type { CreateTaskInput, TaskDispatch } from '@vacps/contracts';
import { Redis } from 'ioredis';

import type { AgentConfig } from '../config.js';
import type { TaskGraphRunner } from '../graph/task-graph.js';
import type { TaskStore } from '../storage/task-store.js';

export interface ScheduleTrigger {
  /** Discriminator for BullMQ schedule jobs (not a task kind). */
  trigger_kind: 'schedule-trigger';
  schedule_id: string;
  task: CreateTaskInput;
}

type QueueData = TaskDispatch | ScheduleTrigger;

export class TaskQueue {
  readonly queueName: string;
  private readonly connection: Redis;
  private readonly queue: Queue<QueueData>;
  private readonly events: QueueEvents;
  private worker?: Worker<QueueData>;

  constructor(
    private readonly config: AgentConfig,
    private readonly store: TaskStore,
    private readonly runner: TaskGraphRunner,
  ) {
    this.queueName = `agent-${config.BACKEND_ID}`;
    this.connection = new Redis(config.REDIS_URL, {
      maxRetriesPerRequest: null,
      enableReadyCheck: true,
    });
    this.queue = new Queue<QueueData>(this.queueName, { connection: this.connection });
    this.events = new QueueEvents(this.queueName, {
      connection: new Redis(config.REDIS_URL, { maxRetriesPerRequest: null }),
    });
  }

  async startWorker(): Promise<void> {
    if (this.worker) return;
    this.worker = new Worker<QueueData>(this.queueName, async (job) => this.process(job), {
      connection: new Redis(this.config.REDIS_URL, { maxRetriesPerRequest: null }),
      concurrency: this.config.WORKER_CONCURRENCY,
    });
    await this.worker.waitUntilReady();
  }

  async enqueue(task: TaskDispatch): Promise<void> {
    this.store.createTask(task, 'queued');
    await this.queue.add('task', task, {
      jobId: task.task_id,
      attempts: task.retry?.attempts ?? 1,
      ...(task.retry
        ? { backoff: { type: 'fixed' as const, delay: task.retry.backoff_seconds * 1000 } }
        : {}),
      removeOnComplete: { age: 86_400, count: 200 },
      removeOnFail: { age: 604_800, count: 500 },
    });
  }

  async getTask(taskId: string) {
    const [stored, job] = await Promise.all([
      this.store.getTask(taskId),
      this.queue.getJob(taskId),
    ]);
    return {
      ...(stored ?? { task: undefined, status: 'dispatch_failed' as const, createdAt: undefined }),
      bullmq: job
        ? { id: job.id, state: await job.getState(), attemptsMade: job.attemptsMade }
        : undefined,
      commands: this.store.listCommands(taskId),
    };
  }

  async cancel(taskId: string): Promise<{
    cancelled: boolean;
    state: string;
    already_terminal?: boolean;
  }> {
    const stored = this.store.getTask(taskId);
    if (stored && ['succeeded', 'failed', 'cancelled', 'timed_out'].includes(stored.status)) {
      return {
        cancelled: stored.status === 'cancelled',
        state: stored.status,
        already_terminal: true,
      };
    }
    if (this.runner.cancel(taskId)) {
      this.store.updateTask(taskId, { status: 'cancelled', finishedAt: new Date().toISOString() });
      return { cancelled: true, state: 'cancelling' };
    }
    const job = await this.queue.getJob(taskId);
    if (!job) {
      // Natural idempotency when unknown: treat as already gone.
      this.store.updateTask(taskId, {
        status: 'cancelled',
        finishedAt: new Date().toISOString(),
      });
      return { cancelled: true, state: 'not_found', already_terminal: true };
    }
    const state = await job.getState();
    if (state === 'waiting' || state === 'delayed' || state === 'prioritized') {
      await job.remove();
      this.store.updateTask(taskId, { status: 'cancelled', finishedAt: new Date().toISOString() });
      return { cancelled: true, state: 'cancelled' };
    }
    if (state === 'active') {
      // Signal graph abort; worker will stop at next checkpoint.
      this.runner.cancel(taskId);
      return { cancelled: true, state: 'cancelling' };
    }
    // completed / failed jobs — already terminal
    return { cancelled: false, state, already_terminal: true };
  }

  /** Re-enqueue a new task_id copy of a previous task (Schema v3). */
  async retry(taskId: string): Promise<{ task_id: string; status: 'queued'; retry_of_task_id: string }> {
    const stored = this.store.getTask(taskId);
    if (!stored) throw new Error('Task not found.');
    const newId = randomUUID();
    const { task_id: _old, schedule_id, ...rest } = stored.task;
    const next = {
      ...rest,
      task_id: newId,
      source: stored.task.source ?? 'api',
      ...(schedule_id ? { schedule_id } : {}),
    } as typeof stored.task;
    await this.enqueue(next);
    return { task_id: newId, status: 'queued', retry_of_task_id: taskId };
  }

  async metrics(): Promise<{ waiting: number; active: number; failed: number }> {
    const counts = await this.queue.getJobCounts('waiting', 'active', 'failed');
    return { waiting: counts.waiting ?? 0, active: counts.active ?? 0, failed: counts.failed ?? 0 };
  }

  isRedisConnected(): boolean {
    return this.connection.status === 'ready';
  }

  isWorkerRunning(): boolean {
    return this.worker !== undefined;
  }

  async upsertScheduler(input: {
    id: string;
    cron: string;
    timezone: string;
    enabled: boolean;
    task: CreateTaskInput;
  }): Promise<void> {
    if (!input.enabled) {
      await this.queue.removeJobScheduler(input.id);
      return;
    }
    await this.queue.upsertJobScheduler(
      input.id,
      { pattern: input.cron, tz: input.timezone },
      {
        name: 'schedule-trigger',
        data: {
          trigger_kind: 'schedule-trigger',
          schedule_id: input.id,
          task: input.task,
        },
        opts: {
          removeOnComplete: { age: 86_400, count: 200 },
          removeOnFail: { age: 604_800, count: 500 },
        },
      },
    );
  }

  async listSchedulers() {
    return this.queue.getJobSchedulers();
  }

  async runScheduleNow(input: { id: string; task: CreateTaskInput }): Promise<string> {
    const taskId = randomUUID();
    await this.enqueue({
      ...input.task,
      task_id: taskId,
      source: 'schedule',
      schedule_id: input.id,
    });
    return taskId;
  }

  async close(): Promise<void> {
    await Promise.all([this.worker?.close(), this.events.close(), this.queue.close()]);
    await this.connection.quit();
  }

  private async process(job: Job<QueueData>): Promise<unknown> {
    const task = isScheduleTrigger(job.data)
      ? {
          ...job.data.task,
          task_id: randomUUID(),
          source: 'schedule' as const,
          schedule_id: job.data.schedule_id,
        }
      : job.data;
    this.store.createTask(task, 'queued');
    return this.runner.run(task);
  }
}

function isScheduleTrigger(data: QueueData): data is ScheduleTrigger {
  return (
    typeof data === 'object' &&
    data !== null &&
    'trigger_kind' in data &&
    (data as ScheduleTrigger).trigger_kind === 'schedule-trigger'
  );
}
