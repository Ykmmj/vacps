/**
 * @deprecated Import from `./schema/tasks.js` / `./schema/schedules.js`.
 * Kept as a stable re-export path for existing imports.
 */
export {
  createTaskSchema,
  parseScheduleCreate,
  parseSchedulePatch,
  schedulesCreateInputSchema,
  schedulesGetInputSchema,
  schedulesIdInputSchema,
  schedulesListInputSchema,
  schedulesUpdateInputSchema,
  taskCreateResult,
  tasksCleanupPreviewInputSchema,
  tasksCleanupRunInputSchema,
  tasksCreateAgentInputSchema,
  tasksCreateCommandInputSchema,
  tasksCreateShellInputSchema,
  tasksDeleteInputSchema,
  tasksGetInputSchema,
  tasksIdInputSchema,
  tasksListInputSchema,
  tasksOutputReadInputSchema,
  toCreateAgentTask,
  toCreateCommandTask,
  toCreateShellTask,
  withBackendId,
} from './schema/index.js';
