import { z } from 'zod';

import {
  argumentsSchema,
  backendIdSchema,
  environmentSchema,
  idempotencyKeySchema,
  programSchema,
  stderrMaxBytesSchema,
  stdoutMaxBytesSchema,
  timeoutMsSchema,
  workingDirectorySchema,
  yieldTimeMsSchema,
} from './defs.js';

export const commandExecInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  program: programSchema,
  arguments: argumentsSchema.optional(),
  working_directory: workingDirectorySchema.optional(),
  environment: environmentSchema.optional(),
  timeout_ms: timeoutMsSchema.optional(),
  yield_time_ms: yieldTimeMsSchema.optional(),
  stdout_max_bytes: stdoutMaxBytesSchema.optional(),
  stderr_max_bytes: stderrMaxBytesSchema.optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});
