#!/usr/bin/env node

import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';

const DEFAULT_CUSTOM_DOMAIN_PREFIX = 'vacps';

export function buildWranglerDeployArgs(argv, env = process.env) {
  const parsed = parseCustomDomainArgs(argv);
  const hasCliConfiguration =
    parsed.domains.length > 0 || parsed.zone !== undefined || parsed.prefix !== undefined;

  let domains;
  if (hasCliConfiguration) {
    domains = resolveDomains(parsed.domains, parsed.zone, parsed.prefix);
  } else {
    const environment = customDomainEnvironment(env);
    domains = resolveDomains(environment.domains, environment.zone, environment.prefix);
  }

  if (domains.length > 0 && hasWranglerDomainArg(parsed.passthrough)) {
    throw new Error(
      'Use either VACPS/--custom-domain options or Wrangler --domain/--domains options, not both.',
    );
  }

  return ['deploy', ...domains.flatMap((domain) => ['--domain', domain]), ...parsed.passthrough];
}

export function resolveDomains(domains, zone, prefix) {
  if (domains.length > 0 && zone !== undefined) {
    throw new Error('Use either full custom domains or a custom-domain zone, not both.');
  }
  if (prefix !== undefined && zone === undefined) {
    throw new Error('A custom-domain prefix requires a custom-domain zone.');
  }

  if (zone !== undefined) {
    const normalizedZone = normalizeHostname(zone, 'custom-domain zone');
    const normalizedPrefix = normalizeLabel(
      prefix ?? DEFAULT_CUSTOM_DOMAIN_PREFIX,
      'custom-domain prefix',
    );
    return [`${normalizedPrefix}.${normalizedZone}`];
  }

  return [...new Set(domains.map((domain) => normalizeHostname(domain, 'custom domain')))];
}

function parseCustomDomainArgs(argv) {
  const domains = [];
  const passthrough = [];
  let zone;
  let prefix;

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    const [flag, inlineValue] = splitArgument(argument);

    if (flag === '--custom-domain') {
      const [value, nextIndex] = optionValue(flag, inlineValue, argv, index);
      domains.push(value);
      index = nextIndex;
      continue;
    }
    if (flag === '--custom-domain-zone') {
      const [value, nextIndex] = optionValue(flag, inlineValue, argv, index);
      zone = singleValue(flag, zone, value);
      index = nextIndex;
      continue;
    }
    if (flag === '--custom-domain-prefix') {
      const [value, nextIndex] = optionValue(flag, inlineValue, argv, index);
      prefix = singleValue(flag, prefix, value);
      index = nextIndex;
      continue;
    }

    passthrough.push(argument);
  }

  return { domains, zone, prefix, passthrough };
}

function customDomainEnvironment(env) {
  const singular = env.VACPS_CUSTOM_DOMAIN?.trim();
  const plural = env.VACPS_CUSTOM_DOMAINS?.trim();
  if (singular && plural) {
    throw new Error('Set either VACPS_CUSTOM_DOMAIN or VACPS_CUSTOM_DOMAINS, not both.');
  }

  return {
    domains: singular
      ? [singular]
      : (plural
          ?.split(',')
          .map((domain) => domain.trim())
          .filter(Boolean) ?? []),
    zone: optionalEnvironmentValue(env.VACPS_CUSTOM_DOMAIN_ZONE),
    prefix: optionalEnvironmentValue(env.VACPS_CUSTOM_DOMAIN_PREFIX),
  };
}

function normalizeHostname(value, description) {
  const hostname = value.trim().toLowerCase().replace(/\.$/, '');
  if (hostname.length > 253 || !hostname.includes('.') || /[:/?#\s]/.test(hostname)) {
    throw new Error(`${description} must be a fully qualified hostname.`);
  }

  for (const label of hostname.split('.')) {
    normalizeLabel(label, description);
  }
  return hostname;
}

function normalizeLabel(value, description) {
  const label = value.trim().toLowerCase();
  if (label.length === 0 || label.length > 63 || !/^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(label)) {
    throw new Error(
      `${description} must contain only lowercase letters, digits, and interior hyphens.`,
    );
  }
  return label;
}

function splitArgument(argument) {
  const equals = argument.indexOf('=');
  return equals === -1
    ? [argument, undefined]
    : [argument.slice(0, equals), argument.slice(equals + 1)];
}

function optionValue(flag, inlineValue, argv, index) {
  if (inlineValue !== undefined) {
    if (!inlineValue) throw new Error(`${flag} requires a value.`);
    return [inlineValue, index];
  }
  const value = argv[index + 1];
  if (!value || value.startsWith('--')) throw new Error(`${flag} requires a value.`);
  return [value, index + 1];
}

function singleValue(flag, current, next) {
  if (current !== undefined) throw new Error(`${flag} may only be specified once.`);
  return next;
}

function optionalEnvironmentValue(value) {
  const normalized = value?.trim();
  return normalized || undefined;
}

function hasWranglerDomainArg(argv) {
  return argv.some((argument) => {
    const [flag] = splitArgument(argument);
    return flag === '--domain' || flag === '--domains';
  });
}

async function main() {
  let wranglerArgs;
  try {
    wranglerArgs = buildWranglerDeployArgs(process.argv.slice(2));
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 2;
    return;
  }

  const domains = [];
  for (let index = 0; index < wranglerArgs.length; index += 1) {
    if (wranglerArgs[index] === '--domain') domains.push(wranglerArgs[index + 1]);
  }
  if (domains.length > 0) {
    console.log(`Deploying Custom Domain${domains.length === 1 ? '' : 's'}: ${domains.join(', ')}`);
  } else {
    console.log(
      'No Custom Domain supplied; existing Custom Domain bindings will be left unchanged.',
    );
  }

  const executable = process.platform === 'win32' ? 'pnpm.cmd' : 'pnpm';
  const child = spawn(executable, ['exec', 'wrangler', ...wranglerArgs], {
    stdio: 'inherit',
    env: process.env,
  });

  await new Promise((resolvePromise, reject) => {
    child.once('error', reject);
    child.once('exit', (code, signal) => {
      if (signal) reject(new Error(`wrangler terminated by signal ${signal}.`));
      else if (code === 0) resolvePromise();
      else reject(new Error(`wrangler deploy exited with code ${code ?? 'unknown'}.`));
    });
  });
}

const isEntrypoint =
  process.argv[1] !== undefined && resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isEntrypoint) {
  await main();
}
