import type { CreateScheduleInput, Schedule, UpdateScheduleInput } from '@vacps/contracts';

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
  last_run_at: string | null;
  next_run_at: string | null;
  created_at: string;
  updated_at: string;
}

export class ScheduleService {
  constructor(
    private readonly db: D1Database,
    private readonly backends: BackendRepository,
    private readonly client: BackendClient,
    private readonly tasks: TaskService,
  ) {}

  async list(): Promise<Schedule[]> {
    const rows = await this.db
      .prepare('SELECT * FROM schedules ORDER BY name ASC')
      .all<ScheduleRow>();
    return rows.results.map(toSchedule);
  }

  async get(id: string): Promise<Schedule> {
    const row = await this.db
      .prepare('SELECT * FROM schedules WHERE id = ?')
      .bind(id)
      .first<ScheduleRow>();
    if (!row) throw new AppError('schedule_not_found', `Schedule '${id}' was not found.`, 404);
    return toSchedule(row);
  }

  async create(input: CreateScheduleInput): Promise<Schedule> {
    const backend = await this.backends.get(input.backendId);
    const id = crypto.randomUUID();
    const now = new Date().toISOString();
    await this.db
      .prepare(
        `INSERT INTO schedules
          (id, backend_id, name, cron, timezone, task_template_json, enabled, created_at, updated_at)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      )
      .bind(
        id,
        input.backendId,
        input.name,
        input.cron,
        input.timezone,
        JSON.stringify(input.taskTemplate),
        Number(input.enabled),
        now,
        now,
      )
      .run();
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
    const next: Schedule = {
      ...current,
      ...(input.name !== undefined ? { name: input.name } : {}),
      ...(input.cron !== undefined ? { cron: input.cron } : {}),
      ...(input.timezone !== undefined ? { timezone: input.timezone } : {}),
      ...(input.enabled !== undefined ? { enabled: input.enabled } : {}),
      ...(input.taskTemplate !== undefined ? { taskTemplate: input.taskTemplate } : {}),
      updatedAt: new Date().toISOString(),
    };
    await this.db
      .prepare(
        `UPDATE schedules SET name = ?, cron = ?, timezone = ?, task_template_json = ?, enabled = ?, updated_at = ?
         WHERE id = ?`,
      )
      .bind(
        next.name,
        next.cron,
        next.timezone,
        JSON.stringify(next.taskTemplate),
        Number(next.enabled),
        next.updatedAt,
        id,
      )
      .run();
    await this.sync(next, await this.backends.get(next.backendId));
    return next;
  }

  async delete(id: string): Promise<void> {
    const schedule = await this.get(id);
    await this.client.deleteScheduler(await this.backends.get(schedule.backendId), id);
    await this.db.prepare('DELETE FROM schedules WHERE id = ?').bind(id).run();
  }

  async runNow(id: string): Promise<unknown> {
    const schedule = await this.get(id);
    const task = await this.tasks.create(schedule.taskTemplate, 'schedule', schedule.id);
    await this.db
      .prepare('UPDATE schedules SET last_run_at = ?, updated_at = ? WHERE id = ?')
      .bind(new Date().toISOString(), new Date().toISOString(), id)
      .run();
    return task;
  }

  async reconcile(): Promise<{ reconciled: number; failed: Array<{ id: string; error: string }> }> {
    const schedules = await this.list();
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

  private async sync(
    schedule: Schedule,
    backend: Awaited<ReturnType<BackendRepository['get']>>,
  ): Promise<void> {
    await this.client.upsertScheduler(backend, schedule.id, {
      cron: schedule.cron,
      timezone: schedule.timezone,
      enabled: schedule.enabled,
      taskTemplate: schedule.taskTemplate,
    });
  }
}

function toSchedule(row: ScheduleRow): Schedule {
  return {
    id: row.id,
    backendId: row.backend_id,
    name: row.name,
    cron: row.cron,
    timezone: row.timezone,
    enabled: Boolean(row.enabled),
    taskTemplate: JSON.parse(row.task_template_json) as Schedule['taskTemplate'],
    ...(row.last_run_at ? { lastRunAt: row.last_run_at } : {}),
    ...(row.next_run_at ? { nextRunAt: row.next_run_at } : {}),
    createdAt: row.created_at,
    updatedAt: row.updated_at,
  };
}
