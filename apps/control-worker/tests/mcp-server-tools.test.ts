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

  it('returns structured content that validates against the output schema', async () => {
    const env = {
      DB: { prepare: () => ({ all: async () => ({ results: [] }) }) },
      CONTROL_PLANE_SIGNING_PRIVATE_KEY: 'test-key',
    } as unknown as Env;
    const client = await connect(env);
    const result = await client.callTool({ name: 'vacps.backends.list', arguments: {} });
    expect(result.isError).toBeFalsy();
    const body = result.structuredContent as { ok?: boolean; backends?: unknown[] };
    expect(body.ok).toBe(true);
    expect(body.backends).toEqual([]);
    await client.close();
  });

  it('exposes JSON Schema that matches runtime constraints for write/process.start', async () => {
    const client = await connect(baseEnv);
    const { tools } = await client.listTools();

    const write = tools.find((tool) => tool.name === 'vacps.files.write');
    expect(write).toBeTruthy();
    const writeSchema = write?.inputSchema as {
      required?: string[];
      properties?: Record<string, { default?: unknown; minimum?: number; enum?: string[] }>;
    };
    expect(writeSchema.required).toEqual(
      expect.arrayContaining(['backend_id', 'path', 'content', 'mode']),
    );
    expect(writeSchema.properties?.mode?.default).toBeUndefined();
    expect(writeSchema.properties?.mode?.enum).toEqual([
      'create',
      'overwrite',
      'create_or_overwrite',
    ]);

    const start = tools.find((tool) => tool.name === 'vacps.process.start');
    expect(start).toBeTruthy();
    const startSchema = start?.inputSchema as {
      properties?: Record<string, { minimum?: number; maximum?: number; default?: unknown }>;
    };
    const hard = startSchema.properties?.stdout_hard_max_bytes;
    expect(hard?.minimum).toBe(0);
    expect(hard?.maximum).toBe(1_073_741_824);
    // Defaults must not advertise a fake "optional overwrite everything" path.
    expect(hard?.minimum).not.toBeLessThan(0);
    // No bogus MIN_SAFE_INTEGER from raw-shape conversion.
    expect(hard?.minimum).not.toBe(Number.MIN_SAFE_INTEGER);

    const caps = tools.find((tool) => tool.name === 'vacps.capabilities.get');
    expect(caps, 'vacps.capabilities.get must be advertised in tools/list').toBeTruthy();

    // Server version must bump when contracts change (forces client cache refresh).
    const { publicToolJsonSchemas } = await import('../src/mcp/tool-schemas.js');
    const published = publicToolJsonSchemas();
    expect(published.mcp_server_version).toBe('0.3.0');
    const writePublished = (published.tools as Record<string, { required?: string[] }>)[
      'vacps.files.write'
    ];
    expect(writePublished.required).toEqual(
      expect.arrayContaining(['backend_id', 'path', 'content', 'mode']),
    );

    await client.close();
  });
});
