import { z } from 'zod';

import {
  argumentsSchema,
  backendIdSchema,
  commandSchema,
  cursorSchema,
  environmentSchema,
  hardMaxBytesSchema,
  idempotencyKeySchema,
  processIdSchema,
  processReadMaxBytesSchema,
  programSchema,
  timeoutMsSchema,
  waitMsSchema,
  workingDirectorySchema,
} from './defs.js';

const processStartShared = {
  backend_id: backendIdSchema,
  working_directory: workingDirectorySchema.optional(),
  environment: environmentSchema.optional(),
  tty: z.boolean().optional(),
  timeout_ms: timeoutMsSchema.optional(),
  stdout_hard_max_bytes: hardMaxBytesSchema.optional(),
  stderr_hard_max_bytes: hardMaxBytesSchema.optional(),
  idempotency_key: idempotencyKeySchema.optional(),
};

/** Flat argv process start (Schema v3 — no mode oneOf). */
export const processStartCommandInputSchema = z.strictObject({
  ...processStartShared,
  program: programSchema,
  arguments: argumentsSchema.optional(),
});

/** Shell-string process start (Schema v3 — no mode oneOf). */
export const processStartShellInputSchema = z
  .object({
    ...processStartShared,
    command: commandSchema,
    shell: z.enum(['/bin/bash', '/bin/sh']).optional(),
    load_user_environment: z.boolean().optional(),
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

export const processReadInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  process_id: processIdSchema,
  cursor: cursorSchema.optional(),
  max_bytes: processReadMaxBytesSchema.optional(),
  wait_ms: waitMsSchema.optional(),
});

export const processWriteInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  process_id: processIdSchema,
  data: z.string().max(1_048_576),
  close_stdin: z.boolean().optional(),
});

export const processTerminateInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  process_id: processIdSchema,
  signal: z.enum(['sigterm', 'sigint', 'sigkill']).optional(),
  grace_period_ms: z.number().int().min(0).max(60_000).optional(),
});
