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
 * Canonical tool input schemas (MCP Schema v2).
 * Used for MCP registerTool (tools/list) and runtime parse.
 */
export const MCP_PROTOCOL_VERSION = '0.4.0';
export const TOOL_SCHEMA_REVISION = '2026-07-28-schema-v2-p0';

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

export const shellExecInputSchema = z.strictObject({
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

/** Runtime + CallTool validation (XOR program/command). */
export const processStartInputSchema = z
  .object({
    ...processStartShared,
    program: programSchema.optional(),
    arguments: argumentsSchema.optional(),
    command: commandSchema.optional(),
  })
  .strict()
  .superRefine((value, context) => {
    const hasProgram = value.program !== undefined;
    const hasCommand = value.command !== undefined;
    if (hasProgram === hasCommand) {
      context.addIssue({
        code: 'custom',
        message: 'Provide exactly one of program or command.',
      });
    }
  });

/**
 * Advertised JSON Schema for tools/list — oneOf expresses program XOR command.
 * MCP SDK only auto-converts plain object Zod schemas (superRefine is invisible).
 */
export const processStartListJsonSchema: Record<string, unknown> = {
  type: 'object',
  properties: {
    backend_id: z.toJSONSchema(backendIdSchema),
    program: z.toJSONSchema(programSchema),
    arguments: z.toJSONSchema(argumentsSchema),
    command: z.toJSONSchema(commandSchema),
    working_directory: z.toJSONSchema(workingDirectorySchema),
    environment: z.toJSONSchema(environmentSchema),
    tty: { type: 'boolean' },
    timeout_ms: z.toJSONSchema(timeoutMsSchema),
    stdout_hard_max_bytes: z.toJSONSchema(hardMaxBytesSchema),
    stderr_hard_max_bytes: z.toJSONSchema(hardMaxBytesSchema),
    idempotency_key: z.toJSONSchema(idempotencyKeySchema),
  },
  required: ['backend_id'],
  oneOf: [
    {
      required: ['program'],
      not: { required: ['command'] },
    },
    {
      required: ['command'],
      not: { required: ['program'] },
    },
  ],
  additionalProperties: false,
};

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

export const filesReadInputSchema = z.strictObject({
  backend_id: backendIdSchema,
  path: pathSchema,
  start_line: z.number().int().min(1).optional(),
  end_line: z.number().int().min(1).optional(),
  max_bytes: fileMaxBytesSchema.optional(),
  encoding: z.enum(['utf-8', 'base64']).optional(),
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
    mcp_server_version: MCP_PROTOCOL_VERSION,
    schema_version: '2.0',
    $defs: publicDefsJson(),
    tools: {
      'vacps.backends.list': z.toJSONSchema(backendsListInputSchema),
      'vacps.command.exec': z.toJSONSchema(commandExecInputSchema),
      'vacps.shell.exec': z.toJSONSchema(shellExecInputSchema),
      'vacps.process.start': processStartListJsonSchema,
      'vacps.files.write': z.toJSONSchema(filesWriteInputSchema),
      'vacps.files.grep': z.toJSONSchema(filesGrepInputSchema),
      'vacps.capabilities.get': z.toJSONSchema(capabilitiesGetInputSchema),
    },
  };
}
