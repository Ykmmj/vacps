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

  it('returns structured content that validates against the Schema v2 envelope', async () => {
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
    expect(body.schema_version).toBe('2.0');
    expect(body.request_id).toBeTruthy();
    expect(body.trace_id).toBeTruthy();
    expect(body.warnings).toEqual([]);
    expect(body.backends).toEqual([]);
    expect(body.returned_count).toBe(0);
    expect(body.next_cursor).toBeNull();
    // content text mirrors structuredContent as compact JSON
    const text = (result.content as Array<{ type: string; text?: string }>)[0]?.text;
    expect(text).toBeTruthy();
    expect(JSON.parse(text!)).toMatchObject({ ok: true, schema_version: '2.0' });
    await client.close();
  });

  it('exposes Schema v2 constraints, annotations, and process.start oneOf', async () => {
    const client = await connect(baseEnv);
    const { tools } = await client.listTools();

    for (const tool of tools) {
      expect(tool.annotations, `${tool.name} missing annotations`).toBeTruthy();
      expect(typeof tool.annotations?.readOnlyHint).toBe('boolean');
      expect(typeof tool.annotations?.destructiveHint).toBe('boolean');
    }

    const list = tools.find((tool) => tool.name === 'vacps.backends.list');
    expect(list?.annotations?.readOnlyHint).toBe(true);
    expect(list?.annotations?.destructiveHint).toBe(false);

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

    const start = tools.find((tool) => tool.name === 'vacps.process.start');
    expect(start).toBeTruthy();
    const startSchema = start?.inputSchema as {
      properties?: Record<string, { minimum?: number; maximum?: number; default?: unknown }>;
      required?: string[];
      oneOf?: Array<{ required?: string[]; not?: { required?: string[] } }>;
      additionalProperties?: boolean;
    };
    const hard = startSchema.properties?.stdout_hard_max_bytes;
    expect(hard?.minimum).toBe(0);
    expect(hard?.maximum).toBe(1_073_741_824);
    expect(startSchema.additionalProperties).toBe(false);
    expect(startSchema.oneOf).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          required: ['program'],
          not: { required: ['command'] },
        }),
        expect.objectContaining({
          required: ['command'],
          not: { required: ['program'] },
        }),
      ]),
    );
    expect(start?.annotations?.openWorldHint).toBe(true);

    const grep = tools.find((tool) => tool.name === 'vacps.files.grep');
    const grepSchema = grep?.inputSchema as {
      properties?: Record<string, unknown>;
      additionalProperties?: boolean;
    };
    expect(grepSchema.properties?.cursor).toBeTruthy();
    expect(grepSchema.additionalProperties).toBe(false);

    const caps = tools.find((tool) => tool.name === 'vacps.capabilities.get');
    expect(caps, 'vacps.capabilities.get must be advertised in tools/list').toBeTruthy();
    expect(caps?.annotations?.readOnlyHint).toBe(true);

    // P2 split task create tools
    for (const name of [
      'vacps.tasks.create_command',
      'vacps.tasks.create_shell',
      'vacps.tasks.create_agent',
    ]) {
      const tool = tools.find((item) => item.name === name);
      expect(tool, `${name} must be advertised`).toBeTruthy();
      expect(tool?.annotations?.destructiveHint).toBe(true);
    }
    const legacyCreate = tools.find((tool) => tool.name === 'vacps.tasks.create');
    expect(legacyCreate?.description?.toLowerCase()).toContain('deprecated');

    const scheduleCreate = tools.find((tool) => tool.name === 'vacps.schedules.create');
    const scheduleSchema = scheduleCreate?.inputSchema as {
      properties?: Record<string, unknown>;
      required?: string[];
    };
    expect(scheduleSchema.properties?.trigger).toBeTruthy();
    expect(scheduleSchema.properties?.task).toBeTruthy();
    expect(scheduleSchema.required).toEqual(
      expect.arrayContaining(['backend_id', 'name', 'trigger', 'task']),
    );

    const { publicToolJsonSchemas, MCP_PROTOCOL_VERSION } = await import(
      '../src/mcp/tool-schemas.js'
    );
    const published = publicToolJsonSchemas();
    expect(published.mcp_server_version).toBe(MCP_PROTOCOL_VERSION);
    expect(published.schema_version).toBe('2.0');
    expect(published.$defs).toBeTruthy();
    const writePublished = (published.tools as Record<string, { required?: string[] }>)[
      'vacps.files.write'
    ];
    expect(writePublished.required).toEqual(
      expect.arrayContaining(['backend_id', 'path', 'content', 'mode']),
    );
    const startPublished = (published.tools as Record<string, { oneOf?: unknown[] }>)[
      'vacps.process.start'
    ];
    expect(startPublished.oneOf).toHaveLength(2);

    await client.close();
  });
});
