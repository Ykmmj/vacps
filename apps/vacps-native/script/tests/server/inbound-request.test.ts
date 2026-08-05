import { describe, expect, it, beforeEach } from 'vitest';

import {
  createInboundRequestAdapter,
  type InboundRequestAdapter,
  type InboundServerRequest,
} from '../../src/server/inbound-request';

function abFrom(text: string): ArrayBuffer {
  return new TextEncoder().encode(text).buffer;
}

function inbound(
  partial: Partial<InboundServerRequest> & Pick<InboundServerRequest, 'url'>,
): InboundServerRequest {
  return {
    method: partial.method ?? 'GET',
    url: partial.url,
    httpVersion: partial.httpVersion ?? '1.1',
    headers: partial.headers ?? {},
    body: partial.body ?? new ArrayBuffer(0),
    remoteAddress: partial.remoteAddress ?? '127.0.0.1:12345',
  };
}

describe('createInboundRequestAdapter', () => {
  let adapt: InboundRequestAdapter;

  beforeEach(() => {
    adapt = createInboundRequestAdapter();
  });

  it('splits raw target on the first ? into path and query', () => {
    const host = adapt(inbound({ url: '/fs/read?path=%2Ftmp%2Fa&limit=10' }));
    expect(host.path).toBe('/fs/read');
    expect(host.query).toBe('path=%2Ftmp%2Fa&limit=10');
  });

  it('uses empty query when target has no ?', () => {
    const host = adapt(inbound({ url: '/health' }));
    expect(host.path).toBe('/health');
    expect(host.query).toBe('');
  });

  it('uses empty query when target ends with bare ?', () => {
    const host = adapt(inbound({ url: '/ready?' }));
    expect(host.path).toBe('/ready');
    expect(host.query).toBe('');
  });

  it('UTF-8 decodes ArrayBuffer body at the product boundary', () => {
    const host = adapt(
      inbound({
        method: 'POST',
        url: '/tasks',
        body: abFrom('{"你好":1}'),
      }),
    );
    expect(host.body).toBe('{"你好":1}');
  });

  it('prefers non-empty x-request-id (case-insensitive)', () => {
    const host = adapt(
      inbound({
        url: '/',
        headers: { 'X-Request-Id': 'client-42' },
      }),
    );
    expect(host.requestId).toBe('client-42');
  });

  it('falls back to deterministic per-adapter sequence when x-request-id absent', () => {
    const a = adapt(inbound({ url: '/a' }));
    const b = adapt(inbound({ url: '/b' }));
    expect(a.requestId).toBe('req-1');
    expect(b.requestId).toBe('req-2');
  });

  it('falls back when x-request-id is empty or whitespace-only', () => {
    expect(adapt(inbound({ url: '/', headers: { 'x-request-id': '' } })).requestId).toBe('req-1');
    expect(adapt(inbound({ url: '/', headers: { 'x-request-id': '   ' } })).requestId).toBe(
      'req-2',
    );
  });

  it('isolates sequence across adapter instances', () => {
    const other = createInboundRequestAdapter();
    expect(adapt(inbound({ url: '/a' })).requestId).toBe('req-1');
    expect(other(inbound({ url: '/b' })).requestId).toBe('req-1');
  });

  it('passes headers and method through unchanged', () => {
    const headers = { 'Content-Type': 'application/json', Accept: '*/*' };
    const host = adapt(inbound({ method: 'PuT', url: '/x', headers }));
    expect(host.method).toBe('PuT');
    expect(host.headers).toBe(headers);
  });
});
