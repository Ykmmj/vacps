import os from 'node:os';

export function publicInterfaceAddresses(
  interfaces: ReturnType<typeof os.networkInterfaces> = os.networkInterfaces(),
): string[] {
  const addresses = new Set<string>();
  for (const entries of Object.values(interfaces)) {
    for (const entry of entries ?? []) {
      if (entry.internal || !isPublicAddress(entry.address, entry.family)) continue;
      addresses.add(entry.address);
    }
  }
  return [...addresses].sort((left, right) => left.localeCompare(right));
}

function isPublicAddress(address: string, family: string): boolean {
  if (family === 'IPv4') return isPublicIpv4(address);
  if (family === 'IPv6') return isGlobalIpv6(address);
  return false;
}

function isPublicIpv4(address: string): boolean {
  const octets = address.split('.').map(Number);
  if (
    octets.length !== 4 ||
    octets.some((octet) => !Number.isInteger(octet) || octet < 0 || octet > 255)
  )
    return false;
  const [first = -1, second = -1] = octets;
  if (first === 0 || first === 10 || first === 127 || first >= 224) return false;
  if (first === 100 && second >= 64 && second <= 127) return false;
  if (first === 169 && second === 254) return false;
  if (first === 172 && second >= 16 && second <= 31) return false;
  if (first === 192 && second === 168) return false;
  return true;
}

function isGlobalIpv6(address: string): boolean {
  const firstHextet = Number.parseInt(address.split(':', 1)[0] ?? '', 16);
  return Number.isInteger(firstHextet) && firstHextet >= 0x2000 && firstHextet < 0x4000;
}
