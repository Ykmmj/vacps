import type {
  CreateScheduleInput,
  CreateTaskInput,
  PatchScheduleInput,
  Schedule,
  ScheduleOccurrenceAck,
  ScheduleOccurrenceAckResult,
  SchedulePolicy,
  UpdateScheduleInput,
} from '@vacps/contracts';
import {
  authoritativeNextAfterOccurrence,
  canonicalUtcIso,
  createTaskSchema,
  nextCronRunAtIso,
  schedulePolicySchema,
  withBackendId,
} from '@vacps/contracts';

import { AppError } from '../lib/http.js';
import type { BackendClient } from '../registry/backend-client.js';
import type { BackendRepository } from '../registry/repository.js';
import type { TaskService } from '../tasks/task-service.js';

interface ScheduleRow {
  id: string;
  backend_id: string;
  name: string;
  cron: string;
  timezone: string;
  task_json: string;
  enabled: number;
  revision?: number | null;
  policy_json?: string | null;
  idempotency_key?: string | null;
  request_hash?: string | null;
  last_run_at: string | null;
  next_run_at: string | null;
  created_at: string;
  updated_at: string;
}

const DEFAULT_POLICY: SchedulePolicy = {
  concurrency: 'forbid',
  misfire: 'run_once',
  max_catchup_runs: 1,
};

type ScheduleRecord = Schedule & { requestHash?: string };

export class ScheduleService {
  constructor(
    private readonly db: D1Database,
    private readonly backends: BackendRepository,
    private readonly client: BackendClient,
    private readonly tasks: TaskService,
  ) {}

  async list(
    query: {
      backendId?: string;
      enabled?: boolean;
      limit?: number;
      offset?: number;
    } = {},
  ): Promise<{
    schedules: ScheduleRecord[];
    returned_count: number;
    next_offset: number | null;
  }> {
    const limit = Math.min(Math.max(query.limit ?? 50, 1), 200);
    const offset = Math.max(query.offset ?? 0, 0);
    const clauses: string[] = [];
    const binds: unknown[] = [];
    if (query.backendId) {
      clauses.push('backend_id = ?');
      binds.push(query.backendId);
    }
    if (typeof query.enabled === 'boolean') {
      clauses.push('enabled = ?');
      binds.push(Number(query.enabled));
    }
    const where = clauses.length ? `WHERE ${clauses.join(' AND ')}` : '';
    const rows = await this.db
      .prepare(`SELECT * FROM schedules ${where} ORDER BY name ASC LIMIT ? OFFSET ?`)
      .bind(...binds, limit + 1, offset)
      .all<ScheduleRow>();
    const page = rows.results.slice(0, limit);
    const hasMore = rows.results.length > limit;
    return {
      schedules: page.map(toSchedule),
      returned_count: page.length,
      next_offset: hasMore ? offset + page.length : null,
    };
  }

  async get(id: string): Promise<ScheduleRecord> {
    const row = await this.db
      .prepare('SELECT * FROM schedules WHERE id = ?')
      .bind(id)
      .first<ScheduleRow>();
    if (!row) throw new AppError('schedule_not_found', `Schedule '${id}' was not found.`, 404);
    return toSchedule(row);
  }

  async create(
    input: CreateScheduleInput,
  ): Promise<ScheduleRecord & { reused?: boolean; requestHash?: string }> {
    const backend = await this.backends.get(input.backend_id);
    const requestHash = await hashScheduleRequest(input);
    const task = withBackendId(input.task, input.backend_id);

    if (input.idempotency_key) {
      const existing = await this.findByIdempotency(input.backend_id, input.idempotency_key);
      if (existing) {
        if (existing.requestHash && existing.requestHash !== requestHash) {
          throw new AppError(
            'idempotency_conflict',
            'The idempotency key was previously used with different arguments.',
            409,
          );
        }
        return {
          ...existing,
          reused: true,
          requestHash: existing.requestHash ?? requestHash,
        };
      }
    }

    const id = crypto.randomUUID();
    const now = new Date().toISOString();
    const policy = schedulePolicySchema.parse(input.policy ?? DEFAULT_POLICY);
    const trigger = {
      type: 'cron' as const,
      expression: input.trigger.expression,
      timezone: input.trigger.timezone ?? 'UTC',
    };
    const nextRunAtRaw =
      input.enabled === false
        ? null
        : (nextCronRunAtIso(trigger.expression, trigger.timezone, new Date(now)) ?? null);
    const nextRunAt = nextRunAtRaw ? (canonicalUtcIso(nextRunAtRaw) ?? nextRunAtRaw) : null;
    try {
      await this.db
        .prepare(
          `INSERT INTO schedules
            (id, backend_id, name, cron, timezone, task_json, enabled, revision, policy_json, idempotency_key, request_hash, next_run_at, created_at, updated_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?, ?, ?)`,
        )
        .bind(
          id,
          input.backend_id,
          input.name,
          trigger.expression,
          trigger.timezone,
          JSON.stringify(task),
          Number(input.enabled ?? true),
          JSON.stringify(policy),
          input.idempotency_key ?? null,
          requestHash,
          nextRunAt,
          now,
          now,
        )
        .run();
    } catch {
      if (input.idempotency_key) {
        const raced = await this.findByIdempotency(input.backend_id, input.idempotency_key);
        if (raced) {
          if (raced.requestHash && raced.requestHash !== requestHash) {
            throw new AppError(
              'idempotency_conflict',
              'The idempotency key was previously used with different arguments.',
              409,
            );
          }
          return {
            ...raced,
            reused: true,
            requestHash: raced.requestHash ?? requestHash,
          };
        }
      }
      throw new AppError('internal_error', 'Could not create schedule.', 500);
    }
    const schedule = await this.get(id);
    try {
      await this.sync(schedule, backend);
      return { ...schedule, requestHash };
    } catch (error) {
      await this.db.prepare('DELETE FROM schedules WHERE id = ?').bind(id).run();
      throw error;
    }
  }

  async update(id: string, input: UpdateScheduleInput): Promise<ScheduleRecord> {
    const current = await this.get(id);
    return this.applyUpdate(current, {
      ...(input.name !== undefined ? { name: input.name } : {}),
      ...(input.trigger !== undefined ? { trigger: input.trigger } : {}),
      ...(input.enabled !== undefined ? { enabled: input.enabled } : {}),
      ...(input.task !== undefined ? { task: withBackendId(input.task, current.backend_id) } : {}),
      ...(input.policy !== undefined ? { policy: schedulePolicySchema.parse(input.policy) } : {}),
    });
  }

  /**
   * Schema v3 patch with expected_revision + optional idempotency_key.
   * Order: request_hash → idempotency lookup → revision check → apply → store result.
   */
  async patch(
    id: string,
    input: PatchScheduleInput,
  ): Promise<ScheduleRecord & { reused?: boolean; requestHash?: string }> {
    const requestHash = await hashOpaque({
      operation: 'schedules.update',
      schedule_id: id,
      expected_revision: input.expected_revision ?? null,
      changes: input.changes,
    });
    const scope = 'schedules.update';

    if (input.idempotency_key) {
      const cached = await this.loadIdempotency(scope, input.idempotency_key);
      if (cached) {
        if (cached.requestHash !== requestHash) {
          throw new AppError(
            'idempotency_conflict',
            'The idempotency key was previously used with different arguments.',
            409,
          );
        }
        // Replay stored schedule snapshot (do not re-check revision).
        return {
          ...(cached.result as ScheduleRecord),
          reused: true,
          requestHash,
        };
      }
    }

    const current = await this.get(id);
    if (input.expected_revision !== undefined && input.expected_revision !== current.revision) {
      throw new AppError(
        'schedule_revision_conflict',
        `Schedule revision mismatch: expected ${input.expected_revision}, current ${current.revision}.`,
        409,
      );
    }
    const changes = input.changes;
    const nextPolicy =
      changes.policy !== undefined
        ? schedulePolicySchema.parse({ ...current.policy, ...changes.policy })
        : undefined;
    const nextTrigger =
      changes.trigger !== undefined
        ? {
            type: 'cron' as const,
            expression: changes.trigger.expression ?? current.trigger.expression,
            timezone: changes.trigger.timezone ?? current.trigger.timezone,
          }
        : undefined;
    const updated = await this.applyUpdate(current, {
      ...(changes.name !== undefined ? { name: changes.name } : {}),
      ...(nextTrigger ? { trigger: nextTrigger } : {}),
      ...(changes.enabled !== undefined ? { enabled: changes.enabled } : {}),
      ...(changes.task !== undefined
        ? { task: withBackendId(changes.task, current.backend_id) }
        : {}),
      ...(nextPolicy ? { policy: nextPolicy } : {}),
    });

    if (input.idempotency_key) {
      await this.storeIdempotency(scope, input.idempotency_key, requestHash, updated);
    }
    return { ...updated, requestHash };
  }

  private async applyUpdate(
    current: ScheduleRecord,
    patch: {
      name?: string;
      trigger?: Schedule['trigger'];
      enabled?: boolean;
      task?: CreateTaskInput;
      policy?: SchedulePolicy;
    },
  ): Promise<ScheduleRecord & { changed?: boolean }> {
    const task = createTaskSchema.parse({
      ...(patch.task ?? current.task),
      backend_id: current.backend_id,
    });
    const nextBase: Schedule = {
      ...current,
      ...(patch.name !== undefined ? { name: patch.name } : {}),
      ...(patch.trigger !== undefined ? { trigger: patch.trigger } : {}),
      ...(patch.enabled !== undefined ? { enabled: patch.enabled } : {}),
      task,
      ...(patch.policy !== undefined ? { policy: patch.policy } : {}),
      revision: current.revision,
      updated_at: current.updated_at,
    };

    // No-op patch: return current revision without bumping.
    if (
      nextBase.name === current.name &&
      nextBase.enabled === current.enabled &&
      nextBase.trigger.expression === current.trigger.expression &&
      nextBase.trigger.timezone === current.trigger.timezone &&
      JSON.stringify(nextBase.policy) === JSON.stringify(current.policy) &&
      JSON.stringify(publicTaskForCompare(nextBase.task)) ===
        JSON.stringify(publicTaskForCompare(current.task))
    ) {
      return { ...current, changed: false };
    }

    const updatedAt = new Date().toISOString();
    const nextRunAtRaw = nextBase.enabled
      ? (nextCronRunAtIso(
          nextBase.trigger.expression,
          nextBase.trigger.timezone,
          new Date(updatedAt),
        ) ?? null)
      : null;
    const nextRunAt = nextRunAtRaw ? (canonicalUtcIso(nextRunAtRaw) ?? nextRunAtRaw) : null;
    const next: Schedule = {
      ...nextBase,
      revision: current.revision + 1,
      updated_at: updatedAt,
      ...(nextRunAt ? { next_run_at: nextRunAt } : { next_run_at: undefined }),
    };

    const result = await this.db
      .prepare(
        `UPDATE schedules SET name = ?, cron = ?, timezone = ?, task_json = ?, enabled = ?,
           revision = ?, policy_json = ?, next_run_at = ?, updated_at = ?
         WHERE id = ? AND revision = ?`,
      )
      .bind(
        next.name,
        next.trigger.expression,
        next.trigger.timezone,
        JSON.stringify(next.task),
        Number(next.enabled),
        next.revision,
        JSON.stringify(next.policy),
        nextRunAt,
        next.updated_at,
        current.id,
        current.revision,
      )
      .run();

    if ((result.meta?.changes ?? 0) === 0) {
      throw new AppError(
        'schedule_revision_conflict',
        `Schedule '${current.id}' was modified concurrently.`,
        409,
      );
    }

    await this.sync(next, await this.backends.get(next.backend_id));
    return { ...(await this.get(current.id)), changed: true };
  }

  /**
   * Delete a schedule.
   * - Without idempotency_key: natural idempotent (missing → already_absent).
   * - With key: request-hash replay like create (same key+hash → replayed result).
   */
  async delete(
    id: string,
    options: { idempotencyKey?: string } = {},
  ): Promise<{
    deleted: boolean;
    already_absent?: boolean;
    schedule_id: string;
    idempotency?: {
      key: string;
      replayed: boolean;
      request_hash: string;
    };
  }> {
    const requestHash = await hashOpaque({
      operation: 'schedules.delete',
      schedule_id: id,
    });
    const scope = 'schedules.delete';

    if (options.idempotencyKey) {
      const cached = await this.loadIdempotency(scope, options.idempotencyKey);
      if (cached) {
        if (cached.requestHash !== requestHash) {
          throw new AppError(
            'idempotency_conflict',
            'The idempotency key was previously used with different arguments.',
            409,
          );
        }
        return {
          ...(cached.result as {
            deleted: boolean;
            already_absent?: boolean;
            schedule_id: string;
          }),
          idempotency: {
            key: options.idempotencyKey,
            replayed: true,
            request_hash: requestHash,
          },
        };
      }
    }

    const existing = await this.db
      .prepare('SELECT id, backend_id FROM schedules WHERE id = ?')
      .bind(id)
      .first<{ id: string; backend_id: string }>();

    let result: { deleted: boolean; already_absent?: boolean; schedule_id: string };
    if (!existing) {
      result = { deleted: false, already_absent: true, schedule_id: id };
    } else {
      try {
        await this.client.deleteScheduler(await this.backends.get(existing.backend_id), id);
      } catch {
        // Control plane is source of truth for schedule inventory.
      }
      await this.db.prepare('DELETE FROM schedules WHERE id = ?').bind(id).run();
      result = { deleted: true, schedule_id: id };
    }

    if (options.idempotencyKey) {
      await this.storeIdempotency(scope, options.idempotencyKey, requestHash, result);
      return {
        ...result,
        idempotency: {
          key: options.idempotencyKey,
          replayed: false,
          request_hash: requestHash,
        },
      };
    }
    return result;
  }

  private async loadIdempotency(
    scope: string,
    key: string,
  ): Promise<{ requestHash: string; result: unknown } | undefined> {
    const row = await this.db
      .prepare(
        'SELECT request_hash, result_json FROM operation_idempotency WHERE scope = ? AND idempotency_key = ?',
      )
      .bind(scope, key)
      .first<{ request_hash: string; result_json: string }>();
    if (!row) return undefined;
    try {
      return { requestHash: row.request_hash, result: JSON.parse(row.result_json) };
    } catch {
      return undefined;
    }
  }

  private async storeIdempotency(
    scope: string,
    key: string,
    requestHash: string,
    result: unknown,
  ): Promise<void> {
    await this.db
      .prepare(
        `INSERT OR REPLACE INTO operation_idempotency
          (scope, idempotency_key, request_hash, result_json, created_at)
         VALUES (?, ?, ?, ?, ?)`,
      )
      .bind(scope, key, requestHash, JSON.stringify(result), new Date().toISOString())
      .run();
  }

  async runNow(
    id: string,
    options: { idempotencyKey?: string } = {},
  ): Promise<{
    scheduleId: string;
    task: Awaited<ReturnType<TaskService['create']>>;
    queued: true;
  }> {
    const schedule = await this.get(id);
    const template = createTaskSchema.parse({
      ...schedule.task,
      backend_id: schedule.backend_id,
      ...(options.idempotencyKey ? { idempotency_key: options.idempotencyKey } : {}),
    });

    const task = await this.tasks.create(template, 'schedule', schedule.id);
    if (!task.reusedExistingTask) {
      const now = new Date();
      // Advance from claimed cursor when present (scheduled_for), else from now.
      // CAS on (revision, next_run_at) so concurrent fire/ack cannot clobber.
      const scheduledForRaw = schedule.next_run_at ?? null;
      const advanceFrom =
        scheduledForRaw && Number.isFinite(Date.parse(scheduledForRaw))
          ? new Date(Date.parse(scheduledForRaw))
          : now;
      const nextRunAt = schedule.enabled
        ? (nextCronRunAtIso(schedule.trigger.expression, schedule.trigger.timezone, advanceFrom) ??
          null)
        : null;
      const nextCanonical = nextRunAt ? (canonicalUtcIso(nextRunAt) ?? nextRunAt) : null;
      const nowIso = now.toISOString();
      if (scheduledForRaw) {
        await this.db
          .prepare(
            `UPDATE schedules
             SET last_run_at = ?, next_run_at = ?, updated_at = ?
             WHERE id = ? AND revision = ? AND next_run_at = ?`,
          )
          .bind(nowIso, nextCanonical, nowIso, id, schedule.revision, scheduledForRaw)
          .run();
      } else {
        await this.db
          .prepare(
            `UPDATE schedules
             SET last_run_at = ?, next_run_at = ?, updated_at = ?
             WHERE id = ? AND revision = ?`,
          )
          .bind(nowIso, nextCanonical, nowIso, id, schedule.revision)
          .run();
      }
      // Push advanced next_run_at to backend so absolute-time agents stay in sync.
      try {
        await this.sync(await this.get(id), await this.backends.get(schedule.backend_id));
      } catch {
        /* best-effort; reconcile will repair */
      }
    }
    return { scheduleId: id, task, queued: true };
  }

  async reconcile(): Promise<{ reconciled: number; failed: Array<{ id: string; error: string }> }> {
    const { schedules } = await this.list({ limit: 200 });
    const result = { reconciled: 0, failed: [] as Array<{ id: string; error: string }> };
    for (const schedule of schedules) {
      try {
        await this.sync(schedule, await this.backends.get(schedule.backend_id));
        result.reconciled += 1;
      } catch (error) {
        result.failed.push({
          id: schedule.id,
          error: error instanceof Error ? error.message : String(error),
        });
      }
    }
    return result;
  }

  /**
   * Backend occurrence ack: CP recomputes authoritative next_run_at and CAS-advances D1.
   * `locally_advanced_to` is never written as truth — diagnostic drift only.
   */
  async ackOccurrence(input: ScheduleOccurrenceAck): Promise<ScheduleOccurrenceAckResult> {
    let schedule: ScheduleRecord;
    try {
      schedule = await this.get(input.schedule_id);
    } catch {
      throw new AppError(
        'schedule_not_found',
        `Schedule '${input.schedule_id}' was not found.`,
        404,
      );
    }

    if (schedule.backend_id !== input.backend_id) {
      throw new AppError(
        'backend_identity_mismatch',
        'Schedule does not belong to this backend.',
        403,
      );
    }

    if (input.revision !== schedule.revision) {
      return {
        accepted: false,
        status: 'revision_mismatch',
        schedule_id: schedule.id,
        revision: schedule.revision,
        ...(schedule.next_run_at ? { next_run_at: schedule.next_run_at } : {}),
        ...(schedule.last_run_at ? { last_run_at: schedule.last_run_at } : {}),
      };
    }

    if (!schedule.enabled) {
      return {
        accepted: false,
        status: 'schedule_disabled',
        schedule_id: schedule.id,
        revision: schedule.revision,
        ...(schedule.next_run_at ? { next_run_at: schedule.next_run_at } : {}),
        ...(schedule.last_run_at ? { last_run_at: schedule.last_run_at } : {}),
      };
    }

    const scheduledFor = canonicalUtcIso(input.scheduled_for);
    if (!scheduledFor) {
      throw new AppError('invalid_request', 'scheduled_for is not a valid timestamp.', 400);
    }

    const currentNext = schedule.next_run_at ? canonicalUtcIso(schedule.next_run_at) : undefined;
    const currentNextMs = currentNext ? Date.parse(currentNext) : Number.NaN;
    const scheduledMs = Date.parse(scheduledFor);

    // Already moved past this occurrence (idempotent stale ack).
    if (
      Number.isFinite(currentNextMs) &&
      Number.isFinite(scheduledMs) &&
      currentNextMs > scheduledMs
    ) {
      const drift = localAdvanceDrift(input.locally_advanced_to, currentNext);
      return {
        accepted: true,
        status: 'already_advanced',
        schedule_id: schedule.id,
        revision: schedule.revision,
        ...(currentNext ? { next_run_at: currentNext } : {}),
        ...(schedule.last_run_at ? { last_run_at: schedule.last_run_at } : {}),
        ...(drift !== undefined ? { local_advance_drift: drift } : {}),
      };
    }

    // Cursor must still be at scheduled_for for CAS (or equal after canonicalize).
    if (
      currentNext &&
      currentNext !== scheduledFor &&
      schedule.next_run_at !== input.scheduled_for
    ) {
      // raw string may differ from canonical; compare epochs
      const rawMs = schedule.next_run_at ? Date.parse(schedule.next_run_at) : Number.NaN;
      if (!(Number.isFinite(rawMs) && rawMs === scheduledMs)) {
        return {
          accepted: false,
          status: 'cursor_mismatch',
          schedule_id: schedule.id,
          revision: schedule.revision,
          ...(currentNext ? { next_run_at: currentNext } : {}),
          ...(schedule.last_run_at ? { last_run_at: schedule.last_run_at } : {}),
        };
      }
    }

    const now = new Date();
    const enqueued = input.enqueued_count ?? (input.occurrence_id ? 1 : 0);
    // skip path: 0 enqueued still advances past backlog (same as run_once on CP).
    const misfire =
      schedule.policy.misfire === 'catch_up' && enqueued === 0 ? 'skip' : schedule.policy.misfire;

    const computed = authoritativeNextAfterOccurrence(
      schedule.trigger.expression,
      schedule.trigger.timezone,
      scheduledFor,
      now,
      misfire,
      Math.max(1, enqueued || 1),
    );
    const nextCanonical = computed ? (canonicalUtcIso(computed) ?? computed) : null;
    const nowIso = now.toISOString();

    // CAS on raw D1 value first; fall back to canonical equality.
    const casToken = schedule.next_run_at ?? scheduledFor;
    const result = await this.db
      .prepare(
        `UPDATE schedules
         SET last_run_at = ?, next_run_at = ?, updated_at = ?
         WHERE id = ? AND revision = ? AND next_run_at = ?`,
      )
      .bind(nowIso, nextCanonical, nowIso, schedule.id, schedule.revision, casToken)
      .run();

    let applied = (result.meta?.changes ?? 0) > 0;
    if (!applied && casToken !== scheduledFor) {
      const retry = await this.db
        .prepare(
          `UPDATE schedules
           SET last_run_at = ?, next_run_at = ?, updated_at = ?
           WHERE id = ? AND revision = ? AND next_run_at = ?`,
        )
        .bind(nowIso, nextCanonical, nowIso, schedule.id, schedule.revision, scheduledFor)
        .run();
      applied = (retry.meta?.changes ?? 0) > 0;
    }

    if (!applied) {
      // Concurrent advance — re-read and treat as idempotent if past scheduled_for.
      const latest = await this.get(schedule.id);
      const latestNext = latest.next_run_at ? canonicalUtcIso(latest.next_run_at) : undefined;
      const latestMs = latestNext ? Date.parse(latestNext) : Number.NaN;
      if (Number.isFinite(latestMs) && latestMs > scheduledMs) {
        return {
          accepted: true,
          status: 'already_advanced',
          schedule_id: latest.id,
          revision: latest.revision,
          ...(latestNext ? { next_run_at: latestNext } : {}),
          ...(latest.last_run_at ? { last_run_at: latest.last_run_at } : {}),
        };
      }
      return {
        accepted: false,
        status: 'cursor_mismatch',
        schedule_id: schedule.id,
        revision: schedule.revision,
        ...(schedule.next_run_at ? { next_run_at: schedule.next_run_at } : {}),
      };
    }

    const updated = await this.get(schedule.id);
    try {
      await this.sync(updated, await this.backends.get(updated.backend_id));
    } catch {
      /* best-effort */
    }

    const drift = localAdvanceDrift(input.locally_advanced_to, updated.next_run_at);
    return {
      accepted: true,
      status: 'cas_applied',
      schedule_id: updated.id,
      revision: updated.revision,
      ...(updated.next_run_at ? { next_run_at: updated.next_run_at } : {}),
      ...(updated.last_run_at ? { last_run_at: updated.last_run_at } : {}),
      ...(drift !== undefined ? { local_advance_drift: drift } : {}),
    };
  }

  private async findByIdempotency(
    backendId: string,
    key: string,
  ): Promise<ScheduleRecord | undefined> {
    const row = await this.db
      .prepare('SELECT * FROM schedules WHERE backend_id = ? AND idempotency_key = ? LIMIT 1')
      .bind(backendId, key)
      .first<ScheduleRow>();
    return row ? toSchedule(row) : undefined;
  }

  private async sync(
    schedule: ScheduleRecord,
    backend: Awaited<ReturnType<BackendRepository['get']>>,
  ): Promise<void> {
    // Backend wire: cron/timezone for display + absolute next_run_at for firing.
    // Always send canonical ISO so backend CAS tokens stay stable.
    const computed =
      schedule.next_run_at ??
      (schedule.enabled
        ? nextCronRunAtIso(schedule.trigger.expression, schedule.trigger.timezone, new Date())
        : undefined);
    const nextRunAt = computed ? canonicalUtcIso(computed) : undefined;
    await this.client.upsertScheduler(backend, schedule.id, {
      cron: schedule.trigger.expression,
      timezone: schedule.trigger.timezone,
      enabled: schedule.enabled,
      task: schedule.task,
      policy: schedule.policy,
      revision: schedule.revision,
      ...(nextRunAt ? { next_run_at: nextRunAt } : {}),
    });
  }
}

/** True when local advance epoch differs from CP next (diagnostic). */
function localAdvanceDrift(
  localAdvancedTo: string | undefined,
  cpNext: string | null | undefined,
): boolean | undefined {
  if (!localAdvancedTo || !cpNext) return undefined;
  const a = Date.parse(localAdvancedTo);
  const b = Date.parse(cpNext);
  if (!Number.isFinite(a) || !Number.isFinite(b)) return undefined;
  return a !== b;
}

function toSchedule(row: ScheduleRow): ScheduleRecord {
  let policy: SchedulePolicy = DEFAULT_POLICY;
  if (row.policy_json) {
    try {
      const parsed = JSON.parse(row.policy_json) as Record<string, unknown>;
      // Accept both V3 snake_case and legacy camelCase policy JSON.
      policy = schedulePolicySchema.parse({
        concurrency: parsed.concurrency,
        misfire: parsed.misfire,
        max_catchup_runs: parsed.max_catchup_runs ?? parsed.maxCatchupRuns ?? 1,
      });
    } catch {
      policy = DEFAULT_POLICY;
    }
  }
  const rawTask = JSON.parse(row.task_json) as unknown;
  const task = createTaskSchema.parse(
    withBackendId(
      // stored task may already include backend_id
      (rawTask && typeof rawTask === 'object' ? rawTask : {}) as CreateTaskInput,
      row.backend_id,
    ),
  );
  return {
    id: row.id,
    backend_id: row.backend_id,
    name: row.name,
    trigger: {
      type: 'cron',
      expression: row.cron,
      timezone: row.timezone,
    },
    enabled: Boolean(row.enabled),
    revision: row.revision && row.revision > 0 ? row.revision : 1,
    policy,
    task,
    ...(row.idempotency_key ? { idempotency_key: row.idempotency_key } : {}),
    ...(row.request_hash ? { requestHash: row.request_hash } : {}),
    ...(row.last_run_at ? { last_run_at: row.last_run_at } : {}),
    ...(row.next_run_at ? { next_run_at: row.next_run_at } : {}),
    created_at: row.created_at,
    updated_at: row.updated_at,
  };
}

/** Compare schedule tasks without storage-only fields. */
function publicTaskForCompare(task: CreateTaskInput | Schedule['task']): unknown {
  const {
    backend_id: _b,
    idempotency_key: _i,
    ...rest
  } = task as CreateTaskInput & { backend_id?: string; idempotency_key?: string };
  return rest;
}

/** Canonical hash of schedule create payload for idempotency_conflict. */
export async function hashScheduleRequest(input: CreateScheduleInput): Promise<string> {
  const { idempotency_key: _drop, ...rest } = input;
  const data = new TextEncoder().encode(stableStringify(rest));
  const digest = await crypto.subtle.digest('SHA-256', data);
  const hex = [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
  return `sha256:${hex}`;
}

function stableStringify(value: unknown): string {
  if (value === null || typeof value !== 'object') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map((item) => stableStringify(item)).join(',')}]`;
  const record = value as Record<string, unknown>;
  const keys = Object.keys(record).sort();
  return `{${keys.map((key) => `${JSON.stringify(key)}:${stableStringify(record[key])}`).join(',')}}`;
}

async function hashOpaque(value: unknown): Promise<string> {
  const data = new TextEncoder().encode(stableStringify(value));
  const digest = await crypto.subtle.digest('SHA-256', data);
  const hex = [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
  return `sha256:${hex}`;
}
