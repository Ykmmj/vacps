import { describe, expect, it } from 'vitest';

import {
  parseScheduleCreate,
  parseSchedulePatch,
  toCreateAgentTask,
  toCreateCommandTask,
  toCreateShellTask,
  tasksCreateCommandInputSchema,
  tasksCreateShellInputSchema,
  tasksCreateAgentInputSchema,
  schedulesCreateInputSchema,
  schedulesUpdateInputSchema,
  schedulesGetInputSchema,
} from '../src/mcp/task-schedule-adapters.js';

describe('task create adapters (Schema v3 kind passthrough)', () => {
  it('maps create_command to kind=command', () => {
    const parsed = tasksCreateCommandInputSchema.parse({
      backend_id: 'backend-01',
      program: 'npm',
      arguments: ['test'],
      timeout_seconds: 600,
      working_directory: '/srv/app',
    });
    const task = toCreateCommandTask(parsed);
    expect(task.kind).toBe('command');
    if (task.kind === 'command') {
      expect(task.program).toBe('npm');
      expect(task.arguments).toEqual(['test']);
    }
    expect(task.backend_id).toBe('backend-01');
    expect(task.working_directory).toBe('/srv/app');
  });

  it('maps create_shell to kind=shell', () => {
    const parsed = tasksCreateShellInputSchema.parse({
      backend_id: 'backend-01',
      command: 'npm ci && npm run build',
      timeout_seconds: 1800,
    });
    const task = toCreateShellTask(parsed);
    expect(task.kind).toBe('shell');
    if (task.kind === 'shell') {
      expect(task.shell).toBe('/bin/bash');
      expect(task.load_user_environment).toBe(true);
      expect(task.command).toContain('npm ci');
    }
  });

  it('maps create_agent with permissions snake_case', () => {
    const parsed = tasksCreateAgentInputSchema.parse({
      backend_id: 'backend-01',
      prompt: 'Diagnose deployment',
      profile: 'diagnostic',
      max_steps: 50,
      timeout_seconds: 1800,
      permissions: { shell: true, network: true, file_write: false },
    });
    const task = toCreateAgentTask(parsed);
    expect(task.kind).toBe('agent');
    if (task.kind === 'agent') {
      expect(task.permissions).toMatchObject({
        shell: true,
        network: true,
        file_write: false,
      });
      expect(task.profile).toBe('diagnostic');
      expect(task.max_steps).toBe(50);
    }
  });
});

describe('schedule adapters', () => {
  it('builds create input with trigger/policy/task (no cron/taskTemplate)', () => {
    const parsed = schedulesCreateInputSchema.parse({
      backend_id: 'backend-01',
      name: 'Nightly backup',
      trigger: { type: 'cron', expression: '0 2 * * *', timezone: 'UTC' },
      policy: { concurrency: 'forbid', misfire: 'run_once', max_catchup_runs: 1 },
      task: {
        kind: 'shell',
        command: './backup.sh',
        timeout_seconds: 3600,
      },
      idempotency_key: 'schedule-001',
    });
    const created = parseScheduleCreate(parsed);
    expect(created.backend_id).toBe('backend-01');
    expect(created.trigger.expression).toBe('0 2 * * *');
    expect(created.task.kind).toBe('shell');
    expect(created.policy.concurrency).toBe('forbid');
    expect(created.idempotency_key).toBe('schedule-001');
  });

  it('builds patch with expected_revision and trigger expression change', () => {
    const parsed = schedulesUpdateInputSchema.parse({
      schedule_id: '00000000-0000-4000-8000-000000000001',
      expected_revision: 3,
      changes: {
        enabled: false,
        trigger: { expression: '0 3 * * *' },
      },
    });
    const patch = parseSchedulePatch(parsed, 'backend-01');
    expect(patch.expected_revision).toBe(3);
    expect(patch.changes.enabled).toBe(false);
    expect(patch.changes.trigger?.expression).toBe('0 3 * * *');
  });

  it('rejects legacy schedule create fields', () => {
    expect(() =>
      schedulesCreateInputSchema.parse({
        backend_id: 'backend-01',
        name: 'legacy',
        cron: '0 2 * * *',
        task_template: { type: 'shell', timeout_seconds: 60 },
      }),
    ).toThrow();
  });

  it('rejects legacy schedule update change fields', () => {
    expect(() =>
      schedulesUpdateInputSchema.parse({
        schedule_id: '00000000-0000-4000-8000-000000000001',
        changes: {
          cron: '0 3 * * *',
          task_template: { type: 'shell' },
        },
      }),
    ).toThrow();
  });

  it('schedules.get has no idempotency_key', () => {
    const parsed = schedulesGetInputSchema.parse({
      schedule_id: '00000000-0000-4000-8000-000000000001',
    });
    expect(parsed).toEqual({
      schedule_id: '00000000-0000-4000-8000-000000000001',
    });
    expect(() =>
      schedulesGetInputSchema.parse({
        schedule_id: '00000000-0000-4000-8000-000000000001',
        idempotency_key: 'should-fail',
      }),
    ).toThrow();
  });
});
