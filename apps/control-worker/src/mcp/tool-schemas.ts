import { z } from 'zod';

import {
  argumentsSchema,
  backendIdSchema,
  commandSchema,
  contextLinesSchema,
  cursorSchema,
  environmentSchema,
  fileMaxBytesSchema,
  hardMaxBytesSchema,
  idempotencyKeySchema,
  listLimitSchema,
  maxMatchesSchema,
  pageLimitSchema,
  pathSchema,
  processIdSchema,
  processReadMaxBytesSchema,
  programSchema,
  publicDefsJson,
  sha256Schema,
  stderrMaxBytesSchema,
  stdoutMaxBytesSchema,
  timeoutMsSchema,
  waitMsSchema,
  workingDirectorySchema,
  yieldTimeMsSchema,
} from './schema/defs.js';

/**
 * Canonical tool input schemas (MCP Schema v3).
 * Single source for MCP registerTool (tools/list) and runtime parse.
 * No dual host/runtime schemas; no legacy tool inputs.
 */
export const MCP_PROTOCOL_VERSION = '0.5.0';
export const TOOL_SCHEMA_REVISION = '2026-07-29-schema-v3';

// ── Backends ──────────────────────────────────────────────────────────

export const backendsListInputSchema = z.strictObject({
  limit: pageLimitSchema.optional(),
  cursor: cursorSchema.optional(),
  enabled: z.boolean().optional(),
  status: z.enum(['healthy', 'unhealthy', 'unknown']).optional(),
  tags: z.array(z.string().min(1).max(64)).max(20).optional(),
});

export const backendsGetStatusInputSchema = z.strictObject({
  backend_id: backendIdSchema,
});

// ── Command / shell / process ─────────────────────────────────────────

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

// ── Files ─────────────────────────────────────────────────────────────

export const filesReadInputSchema = z
  .object({
    backend_id: backendIdSchema,
    path: pathSchema,
    start_line: z.number().int().min(1).optional(),
    end_line: z.number().int().min(1).optional(),
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
  patch: z.string().min(1).max(8 * 1024 * 1024),
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

export const capabilitiesGetInputSchema = z.strictObject({
  backend_id: backendIdSchema,
});

// ── Git ───────────────────────────────────────────────────────────────

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
  patch: z.string().min(1).max(8 * 1024 * 1024),
  working_directory: workingDirectorySchema.optional(),
  check: z.boolean().optional(),
  idempotency_key: idempotencyKeySchema.optional(),
});

/** JSON Schema for operators / CI — mirrors what tools/list should advertise. */
export function publicToolJsonSchemas(): Record<string, unknown> {
  return {
    tool_schema_revision: TOOL_SCHEMA_REVISION,
    tool_schema_version: '3.0',
    mcp_server_version: MCP_PROTOCOL_VERSION,
    schema_version: '3.0',
    $schema: 'https://json-schema.org/draft/2020-12/schema',
    $defs: publicDefsJson(),
    tools: {
      'vacps.backends.list': z.toJSONSchema(backendsListInputSchema),
      'vacps.backends.get_status': z.toJSONSchema(backendsGetStatusInputSchema),
      'vacps.capabilities.get': z.toJSONSchema(capabilitiesGetInputSchema),
      'vacps.command.exec': z.toJSONSchema(commandExecInputSchema),
      'vacps.shell.exec': z.toJSONSchema(shellExecInputSchema),
      'vacps.process.start_command': z.toJSONSchema(processStartCommandInputSchema),
      'vacps.process.start_shell': z.toJSONSchema(processStartShellInputSchema),
      'vacps.process.read': z.toJSONSchema(processReadInputSchema),
      'vacps.process.write': z.toJSONSchema(processWriteInputSchema),
      'vacps.process.terminate': z.toJSONSchema(processTerminateInputSchema),
      'vacps.files.stat': z.toJSONSchema(filesStatInputSchema),
      'vacps.files.read': z.toJSONSchema(filesReadInputSchema),
      'vacps.files.list': z.toJSONSchema(filesListInputSchema),
      'vacps.files.glob': z.toJSONSchema(filesGlobInputSchema),
      'vacps.files.grep': z.toJSONSchema(filesGrepInputSchema),
      'vacps.files.mkdir': z.toJSONSchema(filesMkdirInputSchema),
      'vacps.files.write': z.toJSONSchema(filesWriteInputSchema),
      'vacps.files.edit': z.toJSONSchema(filesEditInputSchema),
      'vacps.files.move': z.toJSONSchema(filesMoveInputSchema),
      'vacps.files.delete': z.toJSONSchema(filesDeleteInputSchema),
      'vacps.files.apply_patch': z.toJSONSchema(filesApplyPatchInputSchema),
      'vacps.git.status': z.toJSONSchema(gitStatusInputSchema),
      'vacps.git.diff': z.toJSONSchema(gitDiffInputSchema),
      'vacps.git.apply': z.toJSONSchema(gitApplyInputSchema),
    },
  };
}
