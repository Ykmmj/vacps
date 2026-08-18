import { describe, expect, it } from 'vitest';

import { buildWranglerDeployArgs, resolveDomains } from '../scripts/deploy-worker.mjs';

describe('control-worker deployment domains', () => {
  it('leaves existing Custom Domain bindings unchanged by default', () => {
    expect(buildWranglerDeployArgs([], {})).toEqual(['deploy']);
  });

  it('builds a hostname from a zone and the default vacps prefix', () => {
    expect(
      buildWranglerDeployArgs([], {
        VACPS_CUSTOM_DOMAIN_ZONE: '803800.xyz',
      }),
    ).toEqual(['deploy', '--domain', 'vacps.803800.xyz']);
  });

  it('accepts a custom prefix from CLI arguments', () => {
    expect(
      buildWranglerDeployArgs(
        ['--custom-domain-zone', 'Example.COM.', '--custom-domain-prefix=edge-01', '--dry-run'],
        {},
      ),
    ).toEqual(['deploy', '--domain', 'edge-01.example.com', '--dry-run']);
  });

  it('accepts and deduplicates complete hostnames', () => {
    expect(
      buildWranglerDeployArgs([], {
        VACPS_CUSTOM_DOMAINS: 'vacps.example.com, api.example.com, vacps.example.com',
      }),
    ).toEqual(['deploy', '--domain', 'vacps.example.com', '--domain', 'api.example.com']);
  });

  it('lets explicit CLI configuration override deployment environment defaults', () => {
    expect(
      buildWranglerDeployArgs(['--custom-domain=vacps.cli.example'], {
        VACPS_CUSTOM_DOMAIN_ZONE: 'environment.example',
        VACPS_CUSTOM_DOMAIN: 'vacps.environment.example',
        VACPS_CUSTOM_DOMAINS: 'api.environment.example',
      }),
    ).toEqual(['deploy', '--domain', 'vacps.cli.example']);
  });

  it('passes native Wrangler domain arguments through when no VACPS domain is supplied', () => {
    expect(buildWranglerDeployArgs(['--domain', 'vacps.example.com'], {})).toEqual([
      'deploy',
      '--domain',
      'vacps.example.com',
    ]);
  });

  it('rejects ambiguous domain sources', () => {
    expect(() =>
      buildWranglerDeployArgs(['--domain', 'wrangler.example.com'], {
        VACPS_CUSTOM_DOMAIN: 'vacps.example.com',
      }),
    ).toThrow('not both');
    expect(() =>
      buildWranglerDeployArgs([], {
        VACPS_CUSTOM_DOMAIN: 'vacps.example.com',
        VACPS_CUSTOM_DOMAINS: 'api.example.com',
      }),
    ).toThrow('not both');
    expect(() => resolveDomains(['vacps.example.com'], 'example.com', undefined)).toThrow(
      'not both',
    );
  });

  it('rejects malformed zones and prefixes before running Wrangler', () => {
    expect(() => resolveDomains([], 'https://example.com', undefined)).toThrow(
      'fully qualified hostname',
    );
    expect(() => resolveDomains([], 'example.com', '-vacps')).toThrow('custom-domain prefix');
  });
});
