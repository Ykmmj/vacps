import { z } from 'zod';

import {
  argumentsSchema,
  backendIdSchema,
  environmentSchema,
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
  environment: environmentSchema.optional(),
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
  status: z.enum(['running', 'exited', 'signaled', 'timed_out']).optional(),
  created_after: z.string().min(1).max(64).optional(),
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
  sensitive: z.boolean().optional(),
});

const terminalNamedKeySchema = z.enum([
  'ENTER',
  'TAB',
  'ESC',
  'BACKSPACE',
  'DELETE',
  'INSERT',
  'UP',
  'DOWN',
  'LEFT',
  'RIGHT',
  'HOME',
  'END',
  'PAGE_UP',
  'PAGE_DOWN',
  'F1',
  'F2',
  'F3',
  'F4',
  'F5',
  'F6',
  'F7',
  'F8',
  'F9',
  'F10',
  'F11',
  'F12',
]);

const terminalCharacterSchema = z.string().refine(
  (value) => {
    const scalars = [...value];
    if (scalars.length !== 1) return false;
    const code = scalars[0]!.codePointAt(0)!;
    return code >= 0x20 && code !== 0x7f && !(code >= 0xd800 && code <= 0xdfff);
  },
  { message: 'key must be a named terminal key or one printable Unicode scalar' },
);

export const terminalSendKeysInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  terminal_id: terminalIdSchema,
  keys: z
    .array(
      z.strictObject({
        key: z.union([terminalNamedKeySchema, terminalCharacterSchema]),
        ctrl: z.boolean().optional(),
        alt: z.boolean().optional(),
        shift: z.boolean().optional(),
      }),
    )
    .min(1)
    .max(256),
  sensitive: z.boolean().optional(),
});

export const terminalExpectInputSchema = z
  .strictObject({
    backend_id: backendIdSchema,
    terminal_id: terminalIdSchema,
    cursor: terminalCursorSchema.optional(),
    pattern: z.string().min(1).max(1_048_576),
    mode: z.enum(['literal', 'regex']).optional(),
    regex_flags: z
      .string()
      .regex(/^[imsu]*$/)
      .max(4)
      .optional(),
    timeout_ms: z.number().int().min(0).max(60_000).optional(),
  })
  .superRefine((value, context) => {
    const mode = value.mode ?? 'literal';
    const flags = value.regex_flags ?? '';
    if (new Set(flags).size !== flags.length) {
      context.addIssue({ code: 'custom', path: ['regex_flags'], message: 'flags must be unique' });
    }
    if (mode === 'literal' && flags.length > 0) {
      context.addIssue({
        code: 'custom',
        path: ['regex_flags'],
        message: 'regex_flags requires mode=regex',
      });
    }
    const patternBytes = new TextEncoder().encode(value.pattern).byteLength;
    const patternLimit = mode === 'regex' ? 4096 : 1_048_576;
    if (patternBytes > patternLimit) {
      context.addIssue({
        code: 'custom',
        path: ['pattern'],
        message: `pattern exceeds ${patternLimit} UTF-8 bytes for ${mode} mode`,
      });
    }
    if (mode === 'regex') {
      try {
        new RegExp(value.pattern, flags);
      } catch {
        context.addIssue({
          code: 'custom',
          path: ['pattern'],
          message: 'invalid regular expression',
        });
      }
    }
  });

export const terminalScreenInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  terminal_id: terminalIdSchema,
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
