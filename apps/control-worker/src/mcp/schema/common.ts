/**
 * Shared Schema v3 constants and $defs re-exports.
 */
export {
  argumentsSchema,
  backendIdSchema,
  commandSchema,
  contextLinesSchema,
  cursorSchema,
  environmentSchema,
  fileMaxBytesSchema,
  idempotencyKeySchema,
  labelsSchema,
  listLimitSchema,
  maxMatchesSchema,
  pageLimitSchema,
  pathSchema,
  programSchema,
  publicDefsJson,
  requestIdSchema,
  scheduleIdSchema,
  sha256Schema,
  stderrMaxBytesSchema,
  stdoutMaxBytesSchema,
  taskIdSchema,
  timeoutMsSchema,
  workingDirectorySchema,
} from './defs.js';

export const MCP_PROTOCOL_VERSION = '0.6.0';
export const TOOL_SCHEMA_REVISION = '2026-08-12-schema-v3-r10-terminal-interaction';
