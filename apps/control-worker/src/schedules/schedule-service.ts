import type {
  CreateScheduleInput,
  PatchScheduleInput,
  Schedule,
  SchedulePolicy,
  UpdateScheduleInput,
} from '@vacps/contracts';
import { schedulePolicySchema } from '@vacps/contracts';

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
  task_template_json: string;
  enabled: number;
  revision?: number | null;
  policy_json?: string | null;
  idempotency_key?: string | null;
  last_run_at: string | null;
  next_run_at: string | null;
  created_at: string;
  updated_at: string;
}

const DEFAULT_POLICY: SchedulePolicy = {
  concurrency: 'forbid',
  misfire: 'run_once',
  maxCatchupRuns: 1,
};

export class ScheduleService {
  constructor(
    private readonly db: D1Database,
    private readonly backends: BackendRepository,
    private readonly client: BackendClient,
    private readonly tasks: TaskService,
  ) {}

  async list(query: {
    backendId?: string;
    enabled?: boolean;
    limit?: number;
    offset?: number;
  } = {}): Promise<{ schedules: Schedule[]; returned_count: number; next_offset: number | null }> {
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

  async get(id: string): Promise<Schedule> {
    const row = await this.db
      .prepare('SELECT * FROM schedules WHERE id = ?')
      .bind(id)
      .first<ScheduleRow>();
    if (!row) throw new AppError('schedule_not_found', `Schedule '${id}' was not found.`, 404);
    return toSchedule(row);
  }

  async create(input: CreateScheduleInput): Promise<Schedule & { reused?: boolean }> {
    const backend = await this.backends.get(input.backendId);

    if (input.idempotencyKey) {
      const existing = await this.findByIdempotency(input.backendId, input.idempotencyKey);
      if (existing) return { ...existing, reused: true };
    }

    const id = crypto.randomUUID();
    const now = new Date().toISOString();
    const policy = input.policy ?? DEFAULT_POLICY;
    try {
      await this.db
        .prepare(
          `INSERT INTO schedules
            (id, backend_id, name, cron, timezone, task_template_json, enabled, revision, policy_json, idempotency_key, created_at, updated_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?)`,
        )
        .bind(
          id,
          input.backendId,
          input.name,
          input.cron,
          input.timezone,
          JSON.stringify(input.taskTemplate),
          Number(input.enabled),
          JSON.stringify(policy),
          input.idempotencyKey ?? null,
          now,
          now,
        )
        .run();
    } catch {
      if (input.idempotencyKey) {
        const raced = await this.findByIdempotency(input.backendId, input.idempotencyKey);
        if (raced) return { ...raced, reused: true };
      }
      throw new AppError('internal_error', 'Could not create schedule.', 500);
    }
    const schedule = await this.get(id);
    try {
      await this.sync(schedule, backend);
      return schedule;
    } catch (error) {
      await this.db.prepare('DELETE FROM schedules WHERE id = ?').bind(id).run();
      throw error;
    }
  }

  async update(id: string, input: UpdateScheduleInput): Promise<Schedule> {
    const current = await this.get(id);
    return this.applyUpdate(current, {
      ...(input.name !== undefined ? { name: input.name } : {}),
      ...(input.cron !== undefined ? { cron: input.cron } : {}),
      ...(input.timezone !== undefined ? { timezone: input.timezone } : {}),
      ...(input.enabled !== undefined ? { enabled: input.enabled } : {}),
      ...(input.taskTemplate !== undefined ? { taskTemplate: input.taskTemplate } : {}),
      ...(input.policy !== undefined ? { policy: input.policy } : {}),
    });
  }

  /** Schema v2 patch with optional expected_revision optimistic concurrency. */
  async patch(id: string, input: PatchScheduleInput): Promise<Schedule> {
    const current = await this.get(id);
    if (
      input.expectedRevision !== undefined &&
      input.expectedRevision !== current.revision
    ) {
      throw new AppError(
        'schedule_revision_conflict',
        `Schedule revision mismatch: expected ${input.expectedRevision}, current ${current.revision}.`,
        409,
      );
    }
    const changes = input.changes;
    const nextPolicy =
      changes.policy !== undefined
        ? schedulePolicySchema.parse({ ...current.policy, ...changes.policy })
        : undefined;
    return this.applyUpdate(current, {
      ...(changes.name !== undefined ? { name: changes.name } : {}),
      ...(changes.cron !== undefined ? { cron: changes.cron } : {}),
      ...(changes.timezone !== undefined ? { timezone: changes.timezone } : {}),
      ...(changes.enabled !== undefined ? { enabled: changes.enabled } : {}),
      ...(changes.taskTemplate !== undefined ? { taskTemplate: changes.taskTemplate } : {}),
      ...(nextPolicy ? { policy: nextPolicy } : {}),
    });
  }

  private async applyUpdate(
    current: Schedule,
    patch: {
      name?: string;
      cron?: string;
      timezone?: string;
      enabled?: boolean;
      taskTemplate?: Schedule['taskTemplate'];
      policy?: SchedulePolicy;
    },
  ): Promise<Schedule> {
    // Ensure task template backend always matches schedule.
    let taskTemplate = patch.taskTemplate ?? current.taskTemplate;
    if (taskTemplate.backendId !== current.backendId) {
      taskTemplate = { ...taskTemplate, backendId: current.backendId };
    }

    const next: Schedule = {
      ...current,
      ...(patch.name !== undefined ? { name: patch.name } : {}),
      ...(patch.cron !== undefined ? { cron: patch.cron } : {}),
      ...(patch.timezone !== undefined ? { timezone: patch.timezone } : {}),
      ...(patch.enabled !== undefined ? { enabled: patch.enabled } : {}),
      taskTemplate,
      ...(patch.policy !== undefined ? { policy: patch.policy } : {}),
      revision: current.revision + 1,
      updatedAt: new Date().toISOString(),
    };

    const result = await this.db
      .prepare(
        `UPDATE schedules SET name = ?, cron = ?, timezone = ?, task_template_json = ?, enabled = ?,
           revision = ?, policy_json = ?, updated_at = ?
         WHERE id = ? AND revision = ?`,
      )
      .bind(
        next.name,
        next.cron,
        next.timezone,
        JSON.stringify(next.taskTemplate),
        Number(next.enabled),
        next.revision,
        JSON.stringify(next.policy),
        next.updatedAt,
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

    await this.sync(next, await this.backends.get(next.backendId));
    return this.get(current.id);
  }

  async delete(id: string): Promise<void> {
    const schedule = await this.get(id);
    await this.client.deleteScheduler(await this.backends.get(schedule.backendId), id);
    await this.db.prepare('DELETE FROM schedules WHERE id = ?').bind(id).run();
  }

  async runNow(
    id: string,
    options: { idempotencyKey?: string } = {},
  ): Promise<{ scheduleId: string; task: Awaited<ReturnType<TaskService['create']>>; queued: true }> {
    const schedule = await this.get(id);
    // Ensure template targets this schedule's backend.
    const template =
      schedule.taskTemplate.backendId === schedule.backendId
        ? schedule.taskTemplate
        : { ...schedule.taskTemplate, backendId: schedule.backendId };

    const taskInput =
      options.idempotencyKey && !template.idempotencyKey
        ? { ...template, idempotencyKey: options.idempotencyKey }
        : template;

    const task = await this.tasks.create(taskInput, 'schedule', schedule.id);
    await this.db
      .prepare('UPDATE schedules SET last_run_at = ?, updated_at = ? WHERE id = ?')
      .bind(new Date().toISOString(), new Date().toISOString(), id)
      .run();
    return { scheduleId: id, task, queued: true };
  }

  async reconcile(): Promise<{ reconciled: number; failed: Array<{ id: string; error: string }> }> {
    const { schedules } = await this.list({ limit: 200 });
    const result = { reconciled: 0, failed: [] as Array<{ id: string; error: string }> };
    for (const schedule of schedules) {
      try {
        await this.sync(schedule, await this.backends.get(schedule.backendId));
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

  private async findByIdempotency(
    backendId: string,
    key: string,
  ): Promise<Schedule | undefined> {
    const row = await this.db
      .prepare(
        'SELECT * FROM schedules WHERE backend_id = ? AND idempotency_key = ? LIMIT 1',
      )
      .bind(backendId, key)
      .first<ScheduleRow>();
    return row ? toSchedule(row) : undefined;
  }

  private async sync(
    schedule: Schedule,
    backend: Awaited<ReturnType<BackendRepository['get']>>,
  ): Promise<void> {
    await this.client.upsertScheduler(backend, schedule.id, {
      cron: schedule.cron,
      timezone: schedule.timezone,
      enabled: schedule.enabled,
      taskTemplate: schedule.taskTemplate,
      policy: schedule.policy,
      revision: schedule.revision,
    });
  }
}

function toSchedule(row: ScheduleRow): Schedule {
  let policy: SchedulePolicy = DEFAULT_POLICY;
  if (row.policy_json) {
    try {
      policy = schedulePolicySchema.parse(JSON.parse(row.policy_json));
    } catch {
      policy = DEFAULT_POLICY;
    }
  }
  return {
    id: row.id,
    backendId: row.backend_id,
    name: row.name,
    cron: row.cron,
    timezone: row.timezone,
    enabled: Boolean(row.enabled),
    revision: row.revision && row.revision > 0 ? row.revision : 1,
    policy,
    taskTemplate: JSON.parse(row.task_template_json) as Schedule['taskTemplate'],
    ...(row.idempotency_key ? { idempotencyKey: row.idempotency_key } : {}),
    ...(row.last_run_at ? { lastRunAt: row.last_run_at } : {}),
    ...(row.next_run_at ? { nextRunAt: row.next_run_at } : {}),
    createdAt: row.created_at,
    updatedAt: row.updated_at,
  };
}
