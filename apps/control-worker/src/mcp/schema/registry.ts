/**
 * Single registry of Schema v3 tool input JSON Schemas for CI / operators.
 * tools/list and runtime parse use the same Zod modules re-exported here.
 */
import { z } from 'zod';

import {
  backendsGetStatusInputSchema,
  backendsListInputSchema,
  capabilitiesGetInputSchema,
} from './backends.js';
import { commandExecInputSchema } from './command.js';
import { MCP_PROTOCOL_VERSION, TOOL_SCHEMA_REVISION, publicDefsJson } from './common.js';
import {
  filesApplyPatchInputSchema,
  filesDeleteInputSchema,
  filesEditInputSchema,
  filesGlobInputSchema,
  filesGrepInputSchema,
  filesListInputSchema,
  filesMkdirInputSchema,
  filesMoveInputSchema,
  filesReadInputSchema,
  filesStatInputSchema,
  filesWriteInputSchema,
} from './files.js';
import { gitApplyInputSchema, gitDiffInputSchema, gitStatusInputSchema } from './git.js';
import {
  schedulesCreateInputSchema,
  schedulesGetInputSchema,
  schedulesIdInputSchema,
  schedulesListInputSchema,
  schedulesUpdateInputSchema,
} from './schedules.js';
import { shellExecInputSchema } from './shell.js';
import {
  terminalCloseInputSchema,
  terminalExpectInputSchema,
  terminalGetInputSchema,
  terminalListInputSchema,
  terminalOpenCommandInputSchema,
  terminalOpenShellInputSchema,
  terminalReadInputSchema,
  terminalResizeInputSchema,
  terminalScreenInputSchema,
  terminalSendKeysInputSchema,
  terminalSignalInputSchema,
  terminalWriteInputSchema,
} from './terminal.js';
import {
  tasksCleanupPreviewInputSchema,
  tasksCleanupRunInputSchema,
  tasksCreateAgentInputSchema,
  tasksCreateCommandInputSchema,
  tasksCreateShellInputSchema,
  tasksDeleteInputSchema,
  tasksGetInputSchema,
  tasksIdInputSchema,
  tasksLegalHoldClearInputSchema,
  tasksLegalHoldSetInputSchema,
  tasksListInputSchema,
  tasksOutputReadInputSchema,
  tasksPinInputSchema,
  tasksUnpinInputSchema,
} from './tasks.js';

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
      'vacps.terminal.open_command': z.toJSONSchema(terminalOpenCommandInputSchema),
      'vacps.terminal.open_shell': z.toJSONSchema(terminalOpenShellInputSchema),
      'vacps.terminal.list': z.toJSONSchema(terminalListInputSchema),
      'vacps.terminal.get': z.toJSONSchema(terminalGetInputSchema),
      'vacps.terminal.read': z.toJSONSchema(terminalReadInputSchema),
      'vacps.terminal.expect': z.toJSONSchema(terminalExpectInputSchema),
      'vacps.terminal.screen': z.toJSONSchema(terminalScreenInputSchema),
      'vacps.terminal.write': z.toJSONSchema(terminalWriteInputSchema),
      'vacps.terminal.send_keys': z.toJSONSchema(terminalSendKeysInputSchema),
      'vacps.terminal.resize': z.toJSONSchema(terminalResizeInputSchema),
      'vacps.terminal.signal': z.toJSONSchema(terminalSignalInputSchema),
      'vacps.terminal.close': z.toJSONSchema(terminalCloseInputSchema),
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
      'vacps.tasks.create_command': z.toJSONSchema(tasksCreateCommandInputSchema),
      'vacps.tasks.create_shell': z.toJSONSchema(tasksCreateShellInputSchema),
      'vacps.tasks.create_agent': z.toJSONSchema(tasksCreateAgentInputSchema),
      'vacps.tasks.get': z.toJSONSchema(tasksGetInputSchema),
      'vacps.tasks.list': z.toJSONSchema(tasksListInputSchema),
      'vacps.tasks.output.read': z.toJSONSchema(tasksOutputReadInputSchema),
      'vacps.tasks.cancel': z.toJSONSchema(tasksIdInputSchema),
      'vacps.tasks.retry': z.toJSONSchema(tasksIdInputSchema),
      'vacps.tasks.delete': z.toJSONSchema(tasksDeleteInputSchema),
      'vacps.tasks.pin': z.toJSONSchema(tasksPinInputSchema),
      'vacps.tasks.unpin': z.toJSONSchema(tasksUnpinInputSchema),
      'vacps.tasks.legal_hold.set': z.toJSONSchema(tasksLegalHoldSetInputSchema),
      'vacps.tasks.legal_hold.clear': z.toJSONSchema(tasksLegalHoldClearInputSchema),
      'vacps.tasks.cleanup.preview': z.toJSONSchema(tasksCleanupPreviewInputSchema),
      'vacps.tasks.cleanup.run': z.toJSONSchema(tasksCleanupRunInputSchema),
      'vacps.schedules.create': z.toJSONSchema(schedulesCreateInputSchema),
      'vacps.schedules.get': z.toJSONSchema(schedulesGetInputSchema),
      'vacps.schedules.list': z.toJSONSchema(schedulesListInputSchema),
      'vacps.schedules.update': z.toJSONSchema(schedulesUpdateInputSchema),
      'vacps.schedules.delete': z.toJSONSchema(schedulesIdInputSchema),
      'vacps.schedules.run_now': z.toJSONSchema(schedulesIdInputSchema),
    },
  };
}
