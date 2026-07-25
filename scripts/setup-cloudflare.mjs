#!/usr/bin/env node

import { randomBytes } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';

const rootDirectory = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const options = parseOptions(process.argv.slice(2));
const databaseName = options.get('database-name') ?? 'vps-agent-control';
const suppliedToken = options.get('backend-token') ?? process.env.BACKEND_SHARED_TOKEN;
const suppliedDatabaseId =
  options.get('database-id') ?? process.env.D1_DATABASE_ID ?? (await configuredDatabaseId());
const cloudflareApiToken = options.get('cloudflare-api-token') ?? process.env.CLOUDFLARE_API_TOKEN;
const cloudflareAccountId =
  options.get('cloudflare-account-id') ?? process.env.CLOUDFLARE_ACCOUNT_ID;
const cloudflareOAuthClientId =
  options.get('cloudflare-oauth-client-id') ?? process.env.CLOUDFLARE_OAUTH_CLIENT_ID;
const cloudflareOAuthClientSecret =
  options.get('cloudflare-oauth-client-secret') ?? process.env.CLOUDFLARE_OAUTH_CLIENT_SECRET;
const cloudflareOAuthRedirectUrl =
  options.get('cloudflare-oauth-redirect-url') ?? process.env.CLOUDFLARE_OAUTH_REDIRECT_URL;
const hasCloudflareApiToken = Boolean(cloudflareApiToken);
const cloudflareEnvironment = {
  ...process.env,
  ...(cloudflareApiToken ? { CLOUDFLARE_API_TOKEN: cloudflareApiToken } : {}),
  ...(cloudflareAccountId ? { CLOUDFLARE_ACCOUNT_ID: cloudflareAccountId } : {}),
};
const hasCloudflareOAuthInput = Boolean(
  cloudflareOAuthClientId || cloudflareOAuthClientSecret || cloudflareOAuthRedirectUrl,
);
const backendToken =
  suppliedToken ??
  (hasCloudflareOAuthInput && suppliedDatabaseId ? undefined : randomBytes(32).toString('hex'));

if (options.has('help')) {
  printUsage();
  process.exit(0);
}
if (backendToken && (backendToken.length < 32 || /\s/.test(backendToken))) {
  throw new Error('BACKEND_SHARED_TOKEN must be at least 32 non-whitespace characters.');
}
if (
  hasCloudflareOAuthInput &&
  (!cloudflareOAuthClientId || !cloudflareOAuthClientSecret || !cloudflareOAuthRedirectUrl)
) {
  throw new Error(
    'Cloudflare OAuth setup requires --cloudflare-oauth-client-id, --cloudflare-oauth-client-secret, and --cloudflare-oauth-redirect-url.',
  );
}
if (!options.has('skip-login') && !hasCloudflareApiToken) {
  await run('pnpm', ['--filter', '@vps-agent/control-worker', 'exec', 'wrangler', 'login']);
} else if (hasCloudflareApiToken) {
  console.log('Using CLOUDFLARE_API_TOKEN; skipping interactive Wrangler OAuth login.');
}

const databaseId = suppliedDatabaseId ?? (await createDatabase(databaseName));
await updateDatabaseBinding(databaseId);
if (backendToken) await putSecret('BACKEND_SHARED_TOKEN', backendToken);
if (hasCloudflareOAuthInput) {
  await putSecret('CLOUDFLARE_OAUTH_CLIENT_ID', cloudflareOAuthClientId);
  await putSecret('CLOUDFLARE_OAUTH_CLIENT_SECRET', cloudflareOAuthClientSecret);
  await putSecret('CLOUDFLARE_OAUTH_REDIRECT_URL', cloudflareOAuthRedirectUrl);
}
await run('pnpm', [
  '--filter',
  '@vps-agent/control-worker',
  'exec',
  'wrangler',
  'd1',
  'migrations',
  'apply',
  databaseName,
  '--remote',
]);
await run('pnpm', ['--filter', '@vps-agent/control-worker', 'run', 'deploy']);

console.log('\nControl plane deployed successfully.');
console.log(`D1 database ID: ${databaseId}`);
if (backendToken && !suppliedToken) {
  console.log(`BACKEND_SHARED_TOKEN (save this now): ${backendToken}`);
} else if (suppliedToken) {
  console.log('BACKEND_SHARED_TOKEN: supplied value stored as a Worker secret.');
} else {
  console.log('BACKEND_SHARED_TOKEN: existing Worker secret was left unchanged.');
}
console.log(
  hasCloudflareOAuthInput
    ? 'Cloudflare OAuth is ready. Connect Cloudflare in the Web UI, then create a stable node endpoint and run its generated command on the VPS.'
    : 'Next: configure Cloudflare OAuth for Managed Tunnels or use Quick Tunnel from the Web UI, then run its generated installer command on the VPS.',
);

async function createDatabase(name) {
  const output = await capture('pnpm', [
    '--filter',
    '@vps-agent/control-worker',
    'exec',
    'wrangler',
    'd1',
    'create',
    name,
  ]);
  const databaseId = findDatabaseId(output);
  if (!databaseId) {
    throw new Error(
      'Could not read the D1 database ID. Re-run with --database-id <UUID> after creating the database in Wrangler.',
    );
  }
  return databaseId;
}

function findDatabaseId(output) {
  try {
    const parsed = JSON.parse(output);
    const values = Array.isArray(parsed) ? parsed : [parsed];
    for (const value of values) {
      if (value && typeof value === 'object') {
        const candidate = value.uuid ?? value.database_id ?? value.id;
        if (typeof candidate === 'string' && isUuid(candidate)) return candidate;
      }
    }
  } catch {
    // The regular-expression fallback handles Wrangler's human-readable output.
  }
  return output.match(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/i)?.[0];
}

async function updateDatabaseBinding(databaseId) {
  const configurationPath = resolve(rootDirectory, 'apps/control-worker/wrangler.jsonc');
  const configuration = await readFile(configurationPath, 'utf8');
  const databaseIdMatch = configuration.match(/"database_id"\s*:\s*"([^"]+)"/);
  if (!databaseIdMatch) {
    throw new Error(`Could not update database_id in ${configurationPath}.`);
  }
  if (databaseIdMatch[1] === databaseId) return;
  const nextConfiguration = configuration.replace(
    /("database_id"\s*:\s*")[^"]+("\s*[,}])/,
    `$1${databaseId}$2`,
  );
  await writeFile(configurationPath, nextConfiguration);
}

async function configuredDatabaseId() {
  const configurationPath = resolve(rootDirectory, 'apps/control-worker/wrangler.jsonc');
  const configuration = await readFile(configurationPath, 'utf8');
  const candidate = configuration.match(/"database_id"\s*:\s*"([^"]+)"/)?.[1];
  return candidate && isUuid(candidate) ? candidate : undefined;
}

async function putSecret(name, value) {
  await run(
    'pnpm',
    ['--filter', '@vps-agent/control-worker', 'exec', 'wrangler', 'secret', 'put', name],
    `${value}\n`,
  );
}

function run(command, args, input) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, {
      cwd: rootDirectory,
      env: cloudflareEnvironment,
      stdio: input === undefined ? 'inherit' : ['pipe', 'inherit', 'inherit'],
    });
    child.once('error', reject);
    child.once('close', (code) => {
      if (code === 0) resolvePromise();
      else
        reject(
          new Error(`${command} ${args.join(' ')} exited with ${code ?? 'an unknown signal'}.`),
        );
    });
    if (input !== undefined) child.stdin.end(input);
  });
}

function capture(command, args) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, {
      cwd: rootDirectory,
      env: cloudflareEnvironment,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => (stdout += chunk));
    child.stderr.on('data', (chunk) => (stderr += chunk));
    child.once('error', reject);
    child.once('close', (code) => {
      process.stdout.write(stdout);
      process.stderr.write(stderr);
      if (code === 0) resolvePromise(`${stdout}\n${stderr}`);
      else
        reject(
          new Error(`${command} ${args.join(' ')} exited with ${code ?? 'an unknown signal'}.`),
        );
    });
  });
}

function parseOptions(args) {
  const options = new Map();
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    // Package managers can forward their `--` argument separator to the script.
    if (argument === '--') continue;
    if (!argument?.startsWith('--')) throw new Error(`Unexpected argument: ${argument}`);
    const key = argument.slice(2);
    if (key === 'help' || key === 'skip-login') {
      options.set(key, 'true');
      continue;
    }
    const value = args[index + 1];
    if (!value || value.startsWith('--')) throw new Error(`Missing value for --${key}.`);
    options.set(key, value);
    index += 1;
  }
  return options;
}

function isUuid(value) {
  return /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(value);
}

function printUsage() {
  console.log(`Usage: pnpm setup:cloudflare [options]

Options:
  --backend-token <token>  Reuse this Backend Bearer token (otherwise generated).
  --database-name <name>   D1 name, default: vps-agent-control.
  --database-id <uuid>     Reuse an existing D1 database instead of creating one.
  --cloudflare-account-id <id>
                           Cloudflare account ID used with API-token authentication.
  --cloudflare-api-token <token>
                           Cloudflare API Token; skips browser OAuth.
  --cloudflare-oauth-client-id <id>
                           Cloudflare OAuth client ID for Managed Tunnels.
  --cloudflare-oauth-client-secret <secret>
                           OAuth client secret. Stored only as a Worker secret.
  --cloudflare-oauth-redirect-url <url>
                           Registered callback URL, ending in
                           /api/cloudflare/oauth/callback.
  --skip-login             Skip \`wrangler login\` when already authenticated.

Set CLOUDFLARE_API_TOKEN (and preferably CLOUDFLARE_ACCOUNT_ID) to use API-token
authentication instead of the browser OAuth callback. Environment variables avoid
putting the API Token in shell history and process arguments.`);
}
