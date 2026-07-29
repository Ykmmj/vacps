import type { TaskStatus } from './task.js';

/** Terminal statuses — cleanup and cancel rules key off this set. */
export const TERMINAL_TASK_STATUSES = [
  'succeeded',
  'failed',
  'cancelled',
  'timed_out',
  'dispatch_failed',
] as const satisfies readonly TaskStatus[];

export type TerminalTaskStatus = (typeof TERMINAL_TASK_STATUSES)[number];

export function isTerminalTaskStatus(status: string): status is TerminalTaskStatus {
  return (TERMINAL_TASK_STATUSES as readonly string[]).includes(status);
}
