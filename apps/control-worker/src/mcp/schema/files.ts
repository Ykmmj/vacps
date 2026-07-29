import { z } from 'zod';

import {
  backendIdSchema,
  contextLinesSchema,
  cursorSchema,
  fileMaxBytesSchema,
  idempotencyKeySchema,
  listLimitSchema,
  maxMatchesSchema,
  pathSchema,
  sha256Schema,
} from './defs.js';

const lineNumberSchema = z.number().int().min(1).max(2_147_483_647);

export const filesReadInputSchema = z
  .object({
    backend_id: backendIdSchema,
    path: pathSchema,
    start_line: lineNumberSchema.optional(),
    end_line: lineNumberSchema.optional(),
    max_bytes: fileMaxBytesSchema.optional(),
    encoding: z.enum(['utf-8', 'base64']).optional(),
  })
  .strict()
  .superRefine((value, context) => {
    if (
      value.start_line !== undefined &&
      value.end_line !== undefined &&
      value.end_line < value.start_line
    ) {
      context.addIssue({
        code: 'custom',
        message: 'end_line must be >= start_line.',
        path: ['end_line'],
      });
    }
  });

export const filesStatInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  path: pathSchema,
});

export const filesListInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  path: pathSchema,
  limit: listLimitSchema.optional(),
  cursor: cursorSchema.optional(),
  include_hidden: z.boolean().optional(),
});

export const filesGlobInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  pattern: z.string().min(1).max(1024),
  path: pathSchema.optional(),
  include_hidden: z.boolean().optional(),
  respect_gitignore: z.boolean().optional(),
  limit: listLimitSchema.optional(),
  cursor: cursorSchema.optional(),
});

export const filesGrepInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  pattern: z.string().min(1).max(10_000),
  path: pathSchema.optional(),
  file_pattern: z.string().min(1).max(1024).optional(),
  case_sensitive: z.boolean().optional(),
  fixed_string: z.boolean().optional(),
  context_before: contextLinesSchema.optional(),
  context_after: contextLinesSchema.optional(),
  max_matches: maxMatchesSchema.optional(),
  max_bytes: fileMaxBytesSchema.optional(),
  cursor: cursorSchema.optional(),
});

export const filesWriteInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  path: pathSchema,
  content: z.string().max(8 * 1024 * 1024),
  // Required — no default. Callers must choose create | overwrite | create_or_overwrite.
  mode: z.enum(['create', 'overwrite', 'create_or_overwrite']),
  expected_sha256: sha256Schema.optional(),
  create_parent_directories: z.boolean().optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});

export const filesEditInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  path: pathSchema,
  old_text: z.string().max(1_048_576),
  new_text: z.string().max(1_048_576),
  replace_all: z.boolean().optional(),
  expected_sha256: sha256Schema.optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});

export const filesApplyPatchInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  patch: z
    .string()
    .min(1)
    .max(8 * 1024 * 1024),
  workspace_path: pathSchema.optional(),
  dry_run: z.boolean().optional(),
  atomic: z.boolean().optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});

export const filesMoveInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  from: pathSchema,
  to: pathSchema,
  overwrite: z.boolean().optional(),
  expected_sha256: sha256Schema.optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});

export const filesDeleteInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  path: pathSchema,
  recursive: z.boolean().optional(),
  expected_sha256: sha256Schema.optional(),
  expected_type: z.enum(['file', 'directory']).optional(),
  dry_run: z.boolean().optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});

export const filesMkdirInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  path: pathSchema,
  recursive: z.boolean().optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});
