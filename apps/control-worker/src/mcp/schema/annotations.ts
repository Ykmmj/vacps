/**
 * Tool annotation matrix (MCP hints only — not a security boundary).
 * Schema v3: every tool has explicit four-hint config.
 */

export type ToolAnnotationHints = {
  readOnlyHint: boolean;
  destructiveHint: boolean;
  idempotentHint: boolean;
  openWorldHint: boolean;
};

const READ_ONLY: ToolAnnotationHints = {
  readOnlyHint: true,
  destructiveHint: false,
  idempotentHint: true,
  openWorldHint: false,
};

const MUTATING_LOCAL: ToolAnnotationHints = {
  readOnlyHint: false,
  destructiveHint: true,
  idempotentHint: false,
  openWorldHint: false,
};

const MUTATING_OPEN: ToolAnnotationHints = {
  readOnlyHint: false,
  destructiveHint: true,
  idempotentHint: false,
  openWorldHint: true,
};

const CREATE_DIR: ToolAnnotationHints = {
  readOnlyHint: false,
  destructiveHint: false,
  idempotentHint: true,
  openWorldHint: false,
};

const IDEMPOTENT_DESTRUCTIVE: ToolAnnotationHints = {
  readOnlyHint: false,
  destructiveHint: true,
  idempotentHint: true,
  openWorldHint: false,
};

/** Per-tool annotation lookup for Schema v3. */
export const TOOL_ANNOTATIONS: Record<string, ToolAnnotationHints> = {
  'vacps.backends.list': READ_ONLY,
  'vacps.backends.get_status': READ_ONLY,
  'vacps.capabilities.get': READ_ONLY,

  'vacps.command.exec': MUTATING_OPEN,
  'vacps.shell.exec': MUTATING_OPEN,
  'vacps.process.start_command': MUTATING_OPEN,
  'vacps.process.start_shell': MUTATING_OPEN,
  'vacps.process.read': READ_ONLY,
  'vacps.process.write': MUTATING_LOCAL,
  'vacps.process.terminate': IDEMPOTENT_DESTRUCTIVE,

  'vacps.files.stat': READ_ONLY,
  'vacps.files.read': READ_ONLY,
  'vacps.files.list': READ_ONLY,
  'vacps.files.glob': READ_ONLY,
  'vacps.files.grep': READ_ONLY,
  'vacps.files.mkdir': CREATE_DIR,
  'vacps.files.write': MUTATING_LOCAL,
  'vacps.files.edit': MUTATING_LOCAL,
  'vacps.files.move': MUTATING_LOCAL,
  'vacps.files.delete': MUTATING_LOCAL,
  'vacps.files.apply_patch': MUTATING_LOCAL,

  'vacps.git.status': READ_ONLY,
  'vacps.git.diff': READ_ONLY,
  'vacps.git.apply': MUTATING_LOCAL,

  'vacps.tasks.create_command': MUTATING_OPEN,
  'vacps.tasks.create_shell': MUTATING_OPEN,
  'vacps.tasks.create_agent': MUTATING_OPEN,
  'vacps.tasks.get': READ_ONLY,
  'vacps.tasks.list': READ_ONLY,
  'vacps.tasks.output.read': READ_ONLY,
  'vacps.tasks.cancel': IDEMPOTENT_DESTRUCTIVE,
  'vacps.tasks.retry': MUTATING_OPEN,
  'vacps.tasks.delete': IDEMPOTENT_DESTRUCTIVE,
  'vacps.tasks.cleanup.preview': READ_ONLY,
  'vacps.tasks.cleanup.run': IDEMPOTENT_DESTRUCTIVE,

  'vacps.schedules.create': MUTATING_LOCAL,
  'vacps.schedules.get': READ_ONLY,
  'vacps.schedules.list': READ_ONLY,
  'vacps.schedules.update': MUTATING_LOCAL,
  'vacps.schedules.delete': IDEMPOTENT_DESTRUCTIVE,
  'vacps.schedules.run_now': MUTATING_OPEN,
};

export function annotationsFor(toolName: string): ToolAnnotationHints {
  return (
    TOOL_ANNOTATIONS[toolName] ?? {
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: true,
    }
  );
}
