import { z } from 'zod';

import {
  argumentsSchema,
  backendIdSchema,
  programSchema,
  workingDirectorySchema,
} from './defs.js';

const terminalIdSchema = z.string().regex(/^term_[a-f0-9]{32}$/, 'invalid terminal_id');
const terminalCursorSchema = z
  .string()
  .regex(/^(0|[1-9][0-9]{0,15})$/, 'cursor must be a decimal byte offset')
  .refine((value) => Number.isSafeInteger(Number(value)), {
    message: 'cursor exceeds the safe integer range',
  });

const terminalOpenShared = {
  backend_id: backendIdSchema,
  working_directory: workingDirectorySchema.optional(),
  columns: z.number().int().min(2).max(500).optional(),
  rows: z.number().int().min(1).max(200).optional(),
  timeout_ms: z.number().int().min(0).max(3_600_000).optional(),
  idle_timeout_ms: z.number().int().min(30_000).max(86_400_000).optional(),
  max_buffer_bytes: z.number().int().min(65_536).max(16_777_216).optional(),
};

export const terminalOpenCommandInputSchema = z.strictObject({
  ...terminalOpenShared,
  program: programSchema,
  arguments: argumentsSchema.optional(),
});

export const terminalOpenShellInputSchema = z.strictObject({
  ...terminalOpenShared,
  shell: z.enum(['/bin/bash', '/bin/sh']).optional(),
  login: z.boolean().optional(),
});

export const terminalListInputSchema = z.strictObject({
  backend_id: backendIdSchema,
});

export const terminalGetInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  terminal_id: terminalIdSchema,
});

export const terminalReadInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  terminal_id: terminalIdSchema,
  cursor: terminalCursorSchema.optional(),
  max_bytes: z.number().int().min(4).max(1_048_576).optional(),
  wait_ms: z.number().int().min(0).max(60_000).optional(),
});

export const terminalWriteInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  terminal_id: terminalIdSchema,
  data: z.string().refine((value) => new TextEncoder().encode(value).byteLength <= 1_048_576, {
    message: 'data must be at most 1 MiB of UTF-8 text',
  }),
});

export const terminalResizeInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  terminal_id: terminalIdSchema,
  columns: z.number().int().min(2).max(500),
  rows: z.number().int().min(1).max(200),
});

export const terminalSignalInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  terminal_id: terminalIdSchema,
  signal: z.enum(['SIGINT', 'SIGTERM', 'SIGHUP', 'SIGKILL', 'SIGTSTP', 'SIGCONT']),
});

export const terminalCloseInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  terminal_id: terminalIdSchema,
  grace_period_ms: z.number().int().min(0).max(60_000).optional(),
});
