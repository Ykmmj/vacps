import type { CreateTaskInput, SchedulePolicy, TaskDispatch } from '@vacps/contracts';
import { canonicalUtcIso, nextCronRunAtIso } from '@vacps/contracts';
import * as host from 'vacps:host';
import * as log from 'vacps:log';

import type { ShellExecutor } from '../executor/shell-executor';
import type { TaskStore } from '../storage/task-store';
import { randomUuidV4 } from '../util/uuid';
import { DEFAULT_SCHEDULE_POLICY, parseSchedulePolicy } from './schedule-logic';
import { cronMatchesUtc, SchedulerStore, utcMinuteKey } from './scheduler-store';

/**
 * Local inbox + single-flight worker (apps/vacps TaskQueue without BullMQ/Redis).
 */
export class TaskQueue {
  private pumpBusy = false;
  readonly schedulers: SchedulerStore;

  constructor(
    private readonly store: TaskStore,
    private readonly executor: ShellExecutor,
    schedulerStore: SchedulerStore,
  ) {
    this.schedulers = schedulerStore;
  }

  async enqueue(task: TaskDispatch): Promise<{ created: boolean }> {
    const created = await this.store.createTask(task, 'queued');
    return { created };
  }

  async getTask(taskId: string) {
    return await this.store.getTask(taskId);
  }

  async listLogs(taskId: string, opts?: { stream?: string; offset?: number; limit?: number }) {
    return await this.store.listLogs(taskId, opts);
  }

  async findByIdempotencyKey(key: string) {
    return await this.store.findByIdempotencyKey(key);
  }

  async cancel(taskId: string): Promise<{
    cancelled: boolean;
    status: string;
    already_terminal?: boolean;
    state?: string;
  }> {
    const before = await this.store.getTask(taskId);
    if (!before) {
      return { cancelled: false, status: 'not_found', already_terminal: true };
    }
    const ok = await this.store.requestCancel(taskId);
    if (!ok) {
      return {
        cancelled: false,
        status: before.status,
        already_terminal: true,
        state: before.status,
      };
    }
    void (await this.executor.cancelRunning(taskId));
    const after = await this.store.getTask(taskId);
    return {
      cancelled: true,
      status: after?.status ?? 'cancelled',
      state: after?.status === 'cancelled' ? 'cancelled' : 'cancelling',
    };
  }

  async retry(
    taskId: string,
  ): Promise<{ task_id: string; status: 'queued'; retry_of_task_id: string }> {
    const newId = randomUuidV4();
    const enqueued = await this.store.enqueueRetryOf(taskId, newId);
    if (!enqueued) {
      throw new Error('Task not found.');
    }
    return { task_id: newId, status: 'queued', retry_of_task_id: taskId };
  }

  /** Claim and run at most one queued task (single-flight). */
  async pumpOnce(): Promise<boolean> {
    if (this.pumpBusy || (await this.store.hasRunning())) {
      return false;
    }
    this.pumpBusy = true;
    try {
      const claimed = await this.store.claimNextQueued();
      if (!claimed) return false;
      log.info(`queue claim task=${claimed.task.task_id} kind=${claimed.task.kind}`);
      await this.executor.execute(claimed.task);
      const done = await this.store.getTask(claimed.task.task_id);
      log.info(`queue done task=${claimed.task.task_id} status=${done?.status ?? '?'}`);
      return true;
    } finally {
      this.pumpBusy = false;
    }
  }

  async recoverInterruptedOnBoot(): Promise<number> {
    return await this.store.recoverInterruptedOnBoot();
  }

  async claimNonce(nonce: string): Promise<boolean> {
    return await this.store.claimNonce(nonce);
  }

  async queueCounts() {
    return await this.store.queueCounts();
  }

  async upsertScheduler(input: {
    id: string;
    cron: string;
    timezone: string;
    enabled: boolean;
    task: CreateTaskInput;
    nextRunAt?: string | null;
    revision?: number;
    policy?: SchedulePolicy;
  }): Promise<void> {
    const wire: {
      id: string;
      cron: string;
      timezone: string;
      enabled: boolean;
      task: CreateTaskInput;
      revision?: number;
      policy: SchedulePolicy;
      nextRunAt?: string | null;
      computeNextIfMissing: () => string | undefined;
    } = {
      id: input.id,
      cron: input.cron,
      timezone: input.timezone,
      enabled: input.enabled,
      task: input.task,
      policy: input.policy ?? DEFAULT_SCHEDULE_POLICY,
      computeNextIfMissing: () =>
        nextCronRunAtIso(input.cron, input.timezone, new Date(host.nowMs())),
    };
    if (input.revision !== undefined) wire.revision = input.revision;
    if (input.nextRunAt !== undefined) wire.nextRunAt = input.nextRunAt;
    await this.schedulers.upsertFromWire(wire);
  }

  async listSchedulers() {
    return (await this.schedulers.list()).map((s) => ({
      id: s.id,
      cron: s.cron,
      timezone: s.timezone,
      enabled: s.enabled,
      task: s.task,
      revision: s.revision,
      policy: s.policy,
      next_run_at: s.nextRunAt ?? null,
      last_claimed_at: s.lastClaimedAt ?? null,
      last_fired_minute: s.lastFiredMinute ?? null,
      updated_at: s.updatedAt,
    }));
  }

  async deleteScheduler(id: string): Promise<void> {
    await this.schedulers.remove(id);
  }

  /** Manual one-shot (not an occurrence claim); random task_id. */
  async runScheduleNow(input: { id: string; task: CreateTaskInput }): Promise<string> {
    const taskId = randomUuidV4();
    const dispatch = {
      ...input.task,
      task_id: taskId,
      source: 'schedule' as const,
      schedule_id: input.id,
    } as TaskDispatch;
    await this.store.createTask(dispatch, 'queued');
    return taskId;
  }

  /**
   * Successful claim payload for control-plane occurrence ack.
   */
  async fireDueSchedulers(nowMs: number = host.nowMs()): Promise<{
    tasksEnqueued: number;
    acks: ScheduleFireAck[];
  }> {
    let tasksEnqueued = 0;
    const acks: ScheduleFireAck[] = [];
    for (const s of await this.schedulers.listEnabled()) {
      // Legacy rows without absolute cursor: seed next from UTC minute match once.
      if (!s.nextRunAt) {
        const now = new Date(nowMs);
        if (!cronMatchesUtc(s.cron, now)) continue;
        const seeded = nextCronRunAtIso(s.cron, s.timezone, new Date(nowMs - 60_000));
        if (!seeded || Date.parse(seeded) > nowMs) continue;
        await this.schedulers.writeRow({
          id: s.id,
          cron: s.cron,
          timezone: s.timezone,
          enabled: s.enabled,
          task: s.task,
          revision: s.revision,
          policy: s.policy,
          nextRunAt: seeded,
        });
      }

      const current = await this.schedulers.get(s.id);
      if (!current?.nextRunAt || !current.enabled) continue;

      const result = await this.schedulers.claimAndEnqueue(
        current,
        nowMs,
        async (slot, schedule) => {
          const dispatch = {
            ...schedule.task,
            task_id: slot.occurrenceId,
            source: 'schedule' as const,
            schedule_id: schedule.id,
            idempotency_key: slot.occurrenceId,
          } as TaskDispatch;
          await this.store.insertOccurrenceTask(dispatch, {
            scheduleId: schedule.id,
            scheduleRevision: slot.revision,
            scheduledForMs: slot.scheduledForMs,
          });
        },
      );

      if (!result.claimed || !result.plan) continue;
      tasksEnqueued += result.slots.length;
      log.info(
        `scheduler claim id=${current.id} rev=${current.revision} ` +
          `scheduled_for=${result.plan.scheduledForRaw} ` +
          `tasks=${result.slots.length} advanced=${result.advancedNext ?? 'none'}`,
      );

      const scheduledFor =
        canonicalUtcIso(result.plan.scheduledForMs) ?? result.plan.scheduledForRaw;
      const ack: ScheduleFireAck = {
        schedule_id: current.id,
        revision: current.revision,
        scheduled_for: scheduledFor,
        enqueued_count: result.slots.length,
      };
      if (result.advancedNext) ack.locally_advanced_to = result.advancedNext;
      if (result.slots[0]) ack.occurrence_id = result.slots[0].occurrenceId;
      acks.push(ack);
    }
    return { tasksEnqueued, acks };
  }
}

/** Payload collected after a successful local claim (for CP ack). */
export interface ScheduleFireAck {
  schedule_id: string;
  revision: number;
  scheduled_for: string;
  enqueued_count: number;
  locally_advanced_to?: string;
  occurrence_id?: string;
}

export { parseSchedulePolicy, utcMinuteKey };
