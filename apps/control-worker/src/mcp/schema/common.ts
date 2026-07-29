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
  hardMaxBytesSchema,
  idempotencyKeySchema,
  labelsSchema,
  listLimitSchema,
  maxMatchesSchema,
  pageLimitSchema,
  pathSchema,
  processIdSchema,
  processReadMaxBytesSchema,
  programSchema,
  publicDefsJson,
  requestIdSchema,
  scheduleIdSchema,
  sha256Schema,
  stderrMaxBytesSchema,
  stdoutMaxBytesSchema,
  taskIdSchema,
  timeoutMsSchema,
  waitMsSchema,
  workingDirectorySchema,
  yieldTimeMsSchema,
} from './defs.js';

export const MCP_PROTOCOL_VERSION = '0.5.3';
export const TOOL_SCHEMA_REVISION = '2026-07-29-schema-v3-r7-pin-hold';
