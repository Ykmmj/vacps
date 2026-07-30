export const TaskState = {
  queued: "queued",
  starting: "starting",
  running: "running",
  succeeded: "succeeded",
  failed: "failed",
  cancelled: "cancelled",
  interrupted: "interrupted",
} as const;

export type TaskState = (typeof TaskState)[keyof typeof TaskState];

export interface TaskRequest {
  readonly id: string;
  readonly kind: string;
  readonly payload: string;
}

export interface TaskResult {
  readonly id: string;
  readonly state: TaskState;
  readonly message: string;
}
