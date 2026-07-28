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
});
