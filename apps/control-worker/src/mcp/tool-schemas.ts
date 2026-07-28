import { z } from 'zod';

/**
 * Canonical tool input schemas. Used both for MCP registerTool (tools/list)
 * and runtime parse — keep a single source of truth.
 */
export const MCP_PROTOCOL_VERSION = '0.3.1';
export const TOOL_SCHEMA_REVISION = '2026-07-28-sync-v2';

const processStartSharedFields = {
  backend_id: z.string().min(1),
  working_directory: z.string().optional(),
  environment: z.record(z.string(), z.string()).optional(),
  tty: z.boolean().optional(),
  timeout_ms: z.number().int().min(1).max(3_600_000).optional(),
  stdout_hard_max_bytes: z.number().int().min(0).max(1_073_741_824).optional(),
  stderr_hard_max_bytes: z.number().int().min(0).max(1_073_741_824).optional(),
  idempotency_key: z.string().optional(),
};

/** Runtime + CallTool validation (XOR program/command). */
export const processStartInputSchema = z
  .object({
    ...processStartSharedFields,
    program: z.string().min(1).optional(),
    arguments: z.array(z.string()).max(1000).optional(),
    command: z.string().min(1).optional(),
  })
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
 * MCP SDK only auto-converts plain object Zod schemas (superRefine is invisible),
 * so tools/list is patched to publish this schema explicitly.
 */
export const processStartListJsonSchema: Record<string, unknown> = {
  type: 'object',
  properties: {
    backend_id: { type: 'string', minLength: 1 },
    program: { type: 'string', minLength: 1 },
    arguments: {
      type: 'array',
      maxItems: 1000,
      items: { type: 'string' },
    },
    command: { type: 'string', minLength: 1 },
    working_directory: { type: 'string' },
    environment: {
      type: 'object',
      additionalProperties: { type: 'string' },
    },
    tty: { type: 'boolean' },
    timeout_ms: { type: 'integer', minimum: 1, maximum: 3_600_000 },
    stdout_hard_max_bytes: { type: 'integer', minimum: 0, maximum: 1_073_741_824 },
    stderr_hard_max_bytes: { type: 'integer', minimum: 0, maximum: 1_073_741_824 },
    idempotency_key: { type: 'string' },
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

export const commandExecInputSchema = z.object({
  backend_id: z.string().min(1),
  program: z.string().min(1),
  arguments: z.array(z.string()).max(1000).optional(),
  working_directory: z.string().optional(),
  environment: z.record(z.string(), z.string()).optional(),
  timeout_ms: z.number().int().min(1).max(3_600_000).optional(),
  yield_time_ms: z.number().int().min(1).max(120_000).optional(),
  stdout_max_bytes: z.number().int().min(0).max(1_048_576).optional(),
  stderr_max_bytes: z.number().int().min(0).max(1_048_576).optional(),
  idempotency_key: z.string().optional(),
});

export const shellExecInputSchema = z.object({
  backend_id: z.string().min(1),
  command: z.string().min(1),
  shell: z.enum(['/bin/bash', '/bin/sh']).optional(),
  working_directory: z.string().optional(),
  environment: z.record(z.string(), z.string()).optional(),
  timeout_ms: z.number().int().min(1).max(3_600_000).optional(),
  yield_time_ms: z.number().int().min(1).max(120_000).optional(),
  stdout_max_bytes: z.number().int().min(0).max(1_048_576).optional(),
  stderr_max_bytes: z.number().int().min(0).max(1_048_576).optional(),
  load_user_environment: z.boolean().optional(),
  idempotency_key: z.string().optional(),
});

export const filesWriteInputSchema = z.object({
  backend_id: z.string().min(1),
  path: z.string().min(1),
  content: z.string(),
  // Required — no default. Callers must choose create | overwrite | create_or_overwrite.
  mode: z.enum(['create', 'overwrite', 'create_or_overwrite']),
  expected_sha256: z.string().optional(),
  create_parent_directories: z.boolean().optional(),
  idempotency_key: z.string().optional(),
});

export const capabilitiesGetInputSchema = z.object({
  backend_id: z.string().min(1),
});

/** JSON Schema for operators / CI — mirrors what tools/list should advertise. */
export function publicToolJsonSchemas(): Record<string, unknown> {
  return {
    tool_schema_revision: TOOL_SCHEMA_REVISION,
    mcp_server_version: MCP_PROTOCOL_VERSION,
    tools: {
      'vacps.command.exec': z.toJSONSchema(commandExecInputSchema),
      'vacps.shell.exec': z.toJSONSchema(shellExecInputSchema),
      'vacps.process.start': processStartListJsonSchema,
      'vacps.files.write': z.toJSONSchema(filesWriteInputSchema),
      'vacps.capabilities.get': z.toJSONSchema(capabilitiesGetInputSchema),
    },
  };
}
