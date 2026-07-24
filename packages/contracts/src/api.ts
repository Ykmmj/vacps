import type { BackendHealth } from './backend.js';
import type { CommandExecution, TaskError, TaskStatus } from './task.js';

export interface ApiError {
  error: { code: string; message: string; requestId?: string };
}

export interface TaskDetail {
  taskId: string;
  status: TaskStatus;
  graphNode?: string;
  result?: unknown;
  error?: TaskError;
  commands: CommandExecution[];
}

export interface BackendStatusResponse {
  health: BackendHealth;
  metrics?: {
    queue: { waiting: number; active: number; failed: number };
  };
}
