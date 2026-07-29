import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { describe, expect, it } from 'vitest';

import type { Env } from '../src/env.js';
import { createMcpServer } from '../src/mcp/server.js';

function connect(env: Env) {
  const server = createMcpServer(env);
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  const client = new Client({ name: 'test-client', version: '0.0.0' });
  return Promise.all([server.connect(serverTransport), client.connect(clientTransport)]).then(
    () => client,
  );
}

const baseEnv = { DB: {}, CONTROL_PLANE_SIGNING_PRIVATE_KEY: 'test-key' } as unknown as Env;

/** Schema v3 final tool set (must match server registry). */
const EXPECTED_TOOLS = [
  'vacps.backends.list',
  'vacps.backends.get_status',
  'vacps.capabilities.get',
  'vacps.command.exec',
  'vacps.shell.exec',
  'vacps.process.start_command',
  'vacps.process.start_shell',
  'vacps.process.read',
  'vacps.process.write',
  'vacps.process.terminate',
  'vacps.files.stat',
  'vacps.files.read',
  'vacps.files.list',
  'vacps.files.glob',
  'vacps.files.grep',
  'vacps.files.mkdir',
  'vacps.files.write',
  'vacps.files.edit',
  'vacps.files.move',
  'vacps.files.delete',
  'vacps.files.apply_patch',
  'vacps.git.status',
  'vacps.git.diff',
  'vacps.git.apply',
  'vacps.tasks.create_command',
  'vacps.tasks.create_shell',
  'vacps.tasks.create_agent',
  'vacps.tasks.get',
  'vacps.tasks.list',
  'vacps.tasks.output.read',
  'vacps.tasks.cancel',
  'vacps.tasks.retry',
  'vacps.tasks.delete',
  'vacps.tasks.pin',
  'vacps.tasks.unpin',
  'vacps.tasks.legal_hold.set',
  'vacps.tasks.legal_hold.clear',
  'vacps.tasks.cleanup.preview',
  'vacps.tasks.cleanup.run',
  'vacps.schedules.create',
  'vacps.schedules.get',
  'vacps.schedules.list',
  'vacps.schedules.update',
  'vacps.schedules.delete',
  'vacps.schedules.run_now',
] as const;

const REMOVED_TOOLS = ['vacps.tasks.create', 'vacps.process.start'] as const;

const READ_ONLY_TOOLS = [
  'vacps.backends.list',
  'vacps.backends.get_status',
  'vacps.capabilities.get',
  'vacps.process.read',
  'vacps.files.stat',
  'vacps.files.read',
  'vacps.files.list',
  'vacps.files.glob',
  'vacps.files.grep',
  'vacps.git.status',
  'vacps.git.diff',
  'vacps.tasks.get',
  'vacps.tasks.list',
  'vacps.tasks.output.read',
  'vacps.tasks.cleanup.preview',
  'vacps.schedules.get',
  'vacps.schedules.list',
] as const;

describe('MCP server tools', () => {
  it('advertises an object output schema for every tool', async () => {
    const client = await connect(baseEnv);
    const { tools } = await client.listTools();
    expect(tools.length).toBeGreaterThan(0);
    for (const tool of tools) {
      expect(tool.outputSchema, `${tool.name} is missing an outputSchema`).toBeTruthy();
      expect(tool.outputSchema?.type, `${tool.name} outputSchema is not an object`).toBe('object');
    }
    await client.close();
  });

  it('returns structured content that validates against the Schema v3 envelope', async () => {
    const env = {
      DB: { prepare: () => ({ all: async () => ({ results: [] }) }) },
      CONTROL_PLANE_SIGNING_PRIVATE_KEY: 'test-key',
    } as unknown as Env;
    const client = await connect(env);
    const result = await client.callTool({ name: 'vacps.backends.list', arguments: {} });
    expect(result.isError).toBeFalsy();
    const body = result.structuredContent as {
      ok?: boolean;
      schema_version?: string;
      request_id?: string;
      trace_id?: string;
      warnings?: string[];
      backends?: unknown[];
      returned_count?: number;
      next_cursor?: string | null;
    };
    expect(body.ok).toBe(true);
    expect(body.schema_version).toBe('3.0');
    expect(body.request_id).toBeTruthy();
    expect(body.trace_id).toBeTruthy();
    expect(body.warnings).toEqual([]);
    expect(body.backends).toEqual([]);
    expect(body.returned_count).toBe(0);
    expect(body.next_cursor).toBeNull();
    // content text mirrors structuredContent as compact JSON
    const text = (result.content as Array<{ type: string; text?: string }>)[0]?.text;
    expect(text).toBeTruthy();
    expect(JSON.parse(text!)).toMatchObject({ ok: true, schema_version: '3.0' });
    await client.close();
  });

  it('exposes Schema v3 tool set, annotations, and strict constraints', async () => {
    const client = await connect(baseEnv);
    const { tools } = await client.listTools();
    const names = tools.map((tool) => tool.name);

    for (const name of EXPECTED_TOOLS) {
      expect(names, `missing tool ${name}`).toContain(name);
    }
    for (const name of REMOVED_TOOLS) {
      expect(names, `legacy tool ${name} must not be advertised`).not.toContain(name);
    }

    for (const tool of tools) {
      expect(tool.annotations, `${tool.name} missing annotations`).toBeTruthy();
      expect(typeof tool.annotations?.readOnlyHint).toBe('boolean');
      expect(typeof tool.annotations?.destructiveHint).toBe('boolean');
      expect(typeof tool.annotations?.idempotentHint).toBe('boolean');
      expect(typeof tool.annotations?.openWorldHint).toBe('boolean');
      expect(tool._meta?.tool_schema_version).toBe('3.0');
    }

    const list = tools.find((tool) => tool.name === 'vacps.backends.list');
    expect(list?.annotations?.readOnlyHint).toBe(true);
    expect(list?.annotations?.destructiveHint).toBe(false);
    expect(list?.annotations?.idempotentHint).toBe(true);

    const write = tools.find((tool) => tool.name === 'vacps.files.write');
    expect(write).toBeTruthy();
    const writeSchema = write?.inputSchema as {
      required?: string[];
      additionalProperties?: boolean;
      properties?: Record<string, { default?: unknown; minimum?: number; enum?: string[] }>;
    };
    expect(writeSchema.required).toEqual(
      expect.arrayContaining(['backend_id', 'path', 'content', 'mode']),
    );
    expect(writeSchema.additionalProperties).toBe(false);
    expect(writeSchema.properties?.mode?.default).toBeUndefined();
    expect(writeSchema.properties?.mode?.enum).toEqual([
      'create',
      'overwrite',
      'create_or_overwrite',
    ]);
    expect(write?.annotations?.destructiveHint).toBe(true);
    expect(write?.annotations?.idempotentHint).toBe(false);

    // process.start_command / start_shell — flat schemas, no mode/oneOf
    const startCommand = tools.find((tool) => tool.name === 'vacps.process.start_command');
    expect(startCommand).toBeTruthy();
    const startCommandSchema = startCommand?.inputSchema as {
      properties?: Record<string, { minimum?: number; maximum?: number }>;
      required?: string[];
      oneOf?: unknown[];
      additionalProperties?: boolean;
    };
    expect(startCommandSchema.oneOf).toBeUndefined();
    expect(startCommandSchema.properties?.mode).toBeUndefined();
    expect(startCommandSchema.required).toEqual(expect.arrayContaining(['backend_id', 'program']));
    expect(startCommandSchema.additionalProperties).toBe(false);
    expect(startCommandSchema.properties?.stdout_hard_max_bytes?.minimum).toBe(0);
    expect(startCommandSchema.properties?.stdout_hard_max_bytes?.maximum).toBe(1_073_741_824);
    expect(startCommand?.annotations?.openWorldHint).toBe(true);

    const startShell = tools.find((tool) => tool.name === 'vacps.process.start_shell');
    expect(startShell).toBeTruthy();
    const startShellSchema = startShell?.inputSchema as {
      properties?: Record<string, unknown>;
      required?: string[];
      oneOf?: unknown[];
    };
    expect(startShellSchema.oneOf).toBeUndefined();
    expect(startShellSchema.properties?.mode).toBeUndefined();
    expect(startShellSchema.required).toEqual(expect.arrayContaining(['backend_id', 'command']));

    // Read-only tools must not advertise idempotency_key
    for (const name of READ_ONLY_TOOLS) {
      const tool = tools.find((item) => item.name === name);
      expect(tool, `${name} must be advertised`).toBeTruthy();
      const schema = tool?.inputSchema as { properties?: Record<string, unknown> };
      expect(
        schema.properties?.idempotency_key,
        `${name} must not have idempotency_key`,
      ).toBeUndefined();
      expect(tool?.annotations?.readOnlyHint).toBe(true);
    }

    const grep = tools.find((tool) => tool.name === 'vacps.files.grep');
    const grepSchema = grep?.inputSchema as {
      properties?: Record<string, unknown>;
      additionalProperties?: boolean;
    };
    expect(grepSchema.properties?.cursor).toBeTruthy();
    expect(grepSchema.additionalProperties).toBe(false);

    const filesList = tools.find((tool) => tool.name === 'vacps.files.list');
    const filesListOut = filesList?.outputSchema as {
      properties?: Record<string, unknown>;
    };
    expect(filesListOut?.properties?.entries).toBeTruthy();
    expect(filesListOut?.properties?.matches).toBeUndefined();

    const caps = tools.find((tool) => tool.name === 'vacps.capabilities.get');
    expect(caps, 'vacps.capabilities.get must be advertised in tools/list').toBeTruthy();
    expect(caps?.annotations?.readOnlyHint).toBe(true);

    for (const name of [
      'vacps.tasks.create_command',
      'vacps.tasks.create_shell',
      'vacps.tasks.create_agent',
    ]) {
      const tool = tools.find((item) => item.name === name);
      expect(tool, `${name} must be advertised`).toBeTruthy();
      expect(tool?.annotations?.destructiveHint).toBe(true);
      const schema = tool?.inputSchema as { properties?: Record<string, unknown> };
      expect(schema.properties?.type, `${name} must not use task.type`).toBeUndefined();
    }

    const scheduleCreate = tools.find((tool) => tool.name === 'vacps.schedules.create');
    const scheduleSchema = scheduleCreate?.inputSchema as {
      properties?: Record<string, unknown>;
      required?: string[];
    };
    expect(scheduleSchema.properties?.trigger).toBeTruthy();
    expect(scheduleSchema.properties?.task).toBeTruthy();
    expect(scheduleSchema.properties?.cron).toBeUndefined();
    expect(scheduleSchema.properties?.task_template).toBeUndefined();
    expect(scheduleSchema.properties?.timezone).toBeUndefined();
    expect(scheduleSchema.required).toEqual(
      expect.arrayContaining(['backend_id', 'name', 'trigger', 'task']),
    );

    const scheduleUpdate = tools.find((tool) => tool.name === 'vacps.schedules.update');
    const updateSchema = scheduleUpdate?.inputSchema as {
      properties?: {
        changes?: {
          properties?: Record<string, unknown>;
        };
      };
    };
    const changeProps = updateSchema.properties?.changes?.properties ?? {};
    expect(changeProps.trigger).toBeTruthy();
    expect(changeProps.task).toBeTruthy();
    expect(changeProps.cron).toBeUndefined();
    expect(changeProps.timezone).toBeUndefined();
    expect(changeProps.task_template).toBeUndefined();

    const scheduleGet = tools.find((tool) => tool.name === 'vacps.schedules.get');
    const getSchema = scheduleGet?.inputSchema as { properties?: Record<string, unknown> };
    expect(getSchema.properties?.schedule_id).toBeTruthy();
    expect(getSchema.properties?.idempotency_key).toBeUndefined();

    const { publicToolJsonSchemas, MCP_PROTOCOL_VERSION } =
      await import('../src/mcp/tool-schemas.js');
    const published = publicToolJsonSchemas();
    expect(published.mcp_server_version).toBe(MCP_PROTOCOL_VERSION);
    expect(published.schema_version).toBe('3.0');
    expect(published.tool_schema_version).toBe('3.0');
    expect(published.$defs).toBeTruthy();
    const shaDef = (published.$defs as Record<string, { pattern?: string }>).sha256;
    expect(shaDef?.pattern).toBe('^sha256:[a-f0-9]{64}$');

    const writePublished = (published.tools as Record<string, { required?: string[] }>)[
      'vacps.files.write'
    ];
    expect(writePublished.required).toEqual(
      expect.arrayContaining(['backend_id', 'path', 'content', 'mode']),
    );
    const startPublished = published.tools as Record<
      string,
      { oneOf?: unknown[]; properties?: Record<string, unknown> }
    >;
    expect(startPublished['vacps.process.start']).toBeUndefined();
    expect(startPublished['vacps.process.start_command']?.properties?.program).toBeTruthy();
    expect(startPublished['vacps.process.start_shell']?.properties?.command).toBeTruthy();
    expect(published.mcp_server_version).toBe(MCP_PROTOCOL_VERSION);

    await client.close();
  });

  it('rejects unknown tools that were removed in Schema v3', async () => {
    const client = await connect(baseEnv);
    for (const name of REMOVED_TOOLS) {
      const result = await client.callTool({ name, arguments: {} });
      expect(result.isError).toBeTruthy();
    }
    await client.close();
  });
});
