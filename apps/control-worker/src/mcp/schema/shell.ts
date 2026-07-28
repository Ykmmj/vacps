import { z } from 'zod';

import {
  backendIdSchema,
  commandSchema,
  environmentSchema,
  idempotencyKeySchema,
  stderrMaxBytesSchema,
  stdoutMaxBytesSchema,
  timeoutMsSchema,
  workingDirectorySchema,
  yieldTimeMsSchema,
} from './defs.js';

export const shellExecInputSchema = z
  .object({
    backend_id: backendIdSchema,
    command: commandSchema,
    shell: z.enum(['/bin/bash', '/bin/sh']).optional(),
    working_directory: workingDirectorySchema.optional(),
    environment: environmentSchema.optional(),
    timeout_ms: timeoutMsSchema.optional(),
    yield_time_ms: yieldTimeMsSchema.optional(),
    stdout_max_bytes: stdoutMaxBytesSchema.optional(),
    stderr_max_bytes: stderrMaxBytesSchema.optional(),
    load_user_environment: z.boolean().optional(),
    idempotency_key: idempotencyKeySchema.optional(),
  })
  .strict()
  .superRefine((value, context) => {
    if (value.shell === '/bin/sh' && value.load_user_environment === true) {
      context.addIssue({
        code: 'custom',
        message:
          'load_user_environment=true is not supported with shell=/bin/sh; use /bin/bash or set false.',
        path: ['load_user_environment'],
      });
    }
  });
