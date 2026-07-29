import { z } from 'zod';

import { backendIdSchema, idempotencyKeySchema, workingDirectorySchema } from './defs.js';

export const gitStatusInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  working_directory: workingDirectorySchema.optional(),
});

export const gitDiffInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  working_directory: workingDirectorySchema.optional(),
  staged: z.boolean().optional(),
});

export const gitApplyInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  patch: z
    .string()
    .min(1)
    .max(8 * 1024 * 1024),
  working_directory: workingDirectorySchema.optional(),
  check: z.boolean().optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});
