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

  enqueue(task: TaskDispatch): { created: boolean } {
    const created = this.store.createTask(task, 'queued');
    return { created };
  }

  getTask(taskId: string) {
    return this.store.getTask(taskId);
  }

  listLogs(taskId: string, opts?: { stream?: string; offset?: number; limit?: number }) {
    return this.store.listLogs(taskId, opts);
  }

  findByIdempotencyKey(key: string) {
    return this.store.findByIdempotencyKey(key);
  }

  cancel(taskId: string): {
    cancelled: boolean;
    status: string;
    already_terminal?: boolean;
    state?: string;
  } {
    const before = this.store.getTask(taskId);
    if (!before) {
      return { cancelled: false, status: 'not_found', already_terminal: true };
    }
    const ok = this.store.requestCancel(taskId);
    if (!ok) {
      return {
        cancelled: false,
        status: before.status,
        already_terminal: true,
        state: before.status,
      };
    }
    void this.executor.cancelRunning(taskId);
    const after = this.store.getTask(taskId);
    return {
      cancelled: true,
      status: after?.status ?? 'cancelled',
      state: after?.status === 'cancelled' ? 'cancelled' : 'cancelling',
    };
  }

  retry(taskId: string): { task_id: string; status: 'queued'; retry_of_task_id: string } {
    const newId = randomUuidV4();
    const enqueued = this.store.enqueueRetryOf(taskId, newId);
    if (!enqueued) {
      throw new Error('Task not found.');
    }
    return { task_id: newId, status: 'queued', retry_of_task_id: taskId };
  }

  /** Claim and run at most one queued task (single-flight). */
  async pumpOnce(): Promise<boolean> {
    if (this.pumpBusy || this.store.hasRunning()) {
      return false;
    }
    this.pumpBusy = true;
    try {
      const claimed = this.store.claimNextQueued();
      if (!claimed) return false;
      log.info(`queue claim task=${claimed.task.task_id} kind=${claimed.task.kind}`);
      await this.executor.execute(claimed.task);
      const done = this.store.getTask(claimed.task.task_id);
      log.info(`queue done task=${claimed.task.task_id} status=${done?.status ?? '?'}`);
      return true;
    } finally {
      this.pumpBusy = false;
    }
  }

  recoverInterruptedOnBoot(): number {
    return this.store.recoverInterruptedOnBoot();
  }

  claimNonce(nonce: string): boolean {
    return this.store.claimNonce(nonce);
  }

  queueCounts() {
    return this.store.queueCounts();
  }

  upsertScheduler(input: {
    id: string;
    cron: string;
    timezone: string;
    enabled: boolean;
    task: CreateTaskInput;
    nextRunAt?: string | null;
    revision?: number;
    policy?: SchedulePolicy;
  }): void {
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
    this.schedulers.upsertFromWire(wire);
  }

  listSchedulers() {
    return this.schedulers.list().map((s) => ({
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

  deleteScheduler(id: string): void {
    this.schedulers.remove(id);
  }

  /** Manual one-shot (not an occurrence claim); random task_id. */
  runScheduleNow(input: { id: string; task: CreateTaskInput }): string {
    const taskId = randomUuidV4();
    const dispatch = {
      ...input.task,
      task_id: taskId,
      source: 'schedule' as const,
      schedule_id: input.id,
    } as TaskDispatch;
    this.store.createTask(dispatch, 'queued');
    return taskId;
  }

  /**
   * Successful claim payload for control-plane occurrence ack.
   */
  fireDueSchedulers(nowMs: number = host.nowMs()): {
    tasksEnqueued: number;
    acks: ScheduleFireAck[];
  } {
    let tasksEnqueued = 0;
    const acks: ScheduleFireAck[] = [];
    for (const s of this.schedulers.listEnabled()) {
      // Legacy rows without absolute cursor: seed next from UTC minute match once.
      if (!s.nextRunAt) {
        const now = new Date(nowMs);
        if (!cronMatchesUtc(s.cron, now)) continue;
        const seeded = nextCronRunAtIso(s.cron, s.timezone, new Date(nowMs - 60_000));
        if (!seeded || Date.parse(seeded) > nowMs) continue;
        this.schedulers.writeRow({
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

      const current = this.schedulers.get(s.id);
      if (!current?.nextRunAt || !current.enabled) continue;

      const result = this.schedulers.claimAndEnqueue(current, nowMs, (slot, schedule) => {
        const dispatch = {
          ...schedule.task,
          task_id: slot.occurrenceId,
          source: 'schedule' as const,
          schedule_id: schedule.id,
          idempotency_key: slot.occurrenceId,
        } as TaskDispatch;
        this.store.insertOccurrenceTask(dispatch, {
          scheduleId: schedule.id,
          scheduleRevision: slot.revision,
          scheduledForMs: slot.scheduledForMs,
        });
      });

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
