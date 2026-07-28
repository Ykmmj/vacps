import { describe, expect, it } from 'vitest';
import type { NetworkInterfaceInfo } from 'node:os';

import { publicInterfaceAddresses } from './public-interface-addresses.js';

describe('publicInterfaceAddresses', () => {
  it('keeps global IPv4 and IPv6 addresses while excluding private, loopback, and link-local interfaces', () => {
    const interfaces = {
      eth0: [
        { address: '203.0.113.8', family: 'IPv4', internal: false },
        { address: '2606:4700:4700::1111', family: 'IPv6', internal: false },
        { address: '2606:4700:4700::1111', family: 'IPv6', internal: false },
        { address: '10.0.0.4', family: 'IPv4', internal: false },
        { address: 'fe80::1', family: 'IPv6', internal: false },
      ],
      lo: [{ address: '127.0.0.1', family: 'IPv4', internal: true }],
    } as Record<string, NetworkInterfaceInfo[]>;

    expect(publicInterfaceAddresses(interfaces)).toEqual(['203.0.113.8', '2606:4700:4700::1111']);
  });
});
