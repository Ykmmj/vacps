#!/usr/bin/env node

import { randomBytes, webcrypto } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, URL } from 'node:url';
import { spawn } from 'node:child_process';

const rootDirectory = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const options = parseOptions(process.argv.slice(2));
const databaseName = options.get('database-name') ?? 'vacps-control';
const suppliedAdminPassword = options.get('admin-password') ?? process.env.CONTROL_PANEL_PASSWORD;
const suppliedSessionSecret = process.env.CONTROL_PANEL_SESSION_SECRET;
const suppliedDatabaseId =
  options.get('database-id') ?? process.env.D1_DATABASE_ID ?? (await configuredDatabaseId());
const suppliedOAuthKvNamespaceId =
  options.get('oauth-kv-id') ??
  process.env.OAUTH_KV_NAMESPACE_ID ??
  (await configuredOAuthKvNamespaceId());
const cloudflareApiToken = options.get('cloudflare-api-token') ?? process.env.CLOUDFLARE_API_TOKEN;
const cloudflareAccountId =
  options.get('cloudflare-account-id') ?? process.env.CLOUDFLARE_ACCOUNT_ID;
const cloudflareOAuthClientId =
  options.get('cloudflare-oauth-client-id') ?? process.env.CLOUDFLARE_OAUTH_CLIENT_ID;
const cloudflareOAuthClientSecret =
  options.get('cloudflare-oauth-client-secret') ?? process.env.CLOUDFLARE_OAUTH_CLIENT_SECRET;
const cloudflareOAuthRedirectUrl =
  options.get('cloudflare-oauth-redirect-url') ?? process.env.CLOUDFLARE_OAUTH_REDIRECT_URL;
const bootstrapManagedTunnelOAuth =
  options.has('bootstrap-managed-tunnel-oauth') ||
  process.env.BOOTSTRAP_MANAGED_TUNNEL_OAUTH === 'true';
const controlPlaneUrl = options.get('control-plane-url') ?? process.env.CONTROL_PLANE_URL;
const hasCloudflareApiToken = Boolean(cloudflareApiToken);
const cloudflareEnvironment = {
  ...process.env,
  ...(cloudflareApiToken ? { CLOUDFLARE_API_TOKEN: cloudflareApiToken } : {}),
  ...(cloudflareAccountId ? { CLOUDFLARE_ACCOUNT_ID: cloudflareAccountId } : {}),
};
const hasCloudflareOAuthInput = Boolean(
  cloudflareOAuthClientId || cloudflareOAuthClientSecret || cloudflareOAuthRedirectUrl,
);
const sessionSecret = suppliedSessionSecret ?? randomBytes(32).toString('base64url');

if (options.has('help')) {
  printUsage();
  process.exit(0);
}
if (!suppliedAdminPassword) {
  throw new Error(
    'CONTROL_PANEL_PASSWORD is required. Set it as an environment variable or pass --admin-password.',
  );
}
if (suppliedAdminPassword.length < 12 || /\s/.test(suppliedAdminPassword)) {
  throw new Error('CONTROL_PANEL_PASSWORD must be at least 12 non-whitespace characters.');
}
if (
  suppliedSessionSecret &&
  (suppliedSessionSecret.length < 32 || /\s/.test(suppliedSessionSecret))
) {
  throw new Error('CONTROL_PANEL_SESSION_SECRET must be at least 32 non-whitespace characters.');
}
if (
  hasCloudflareOAuthInput &&
  (!cloudflareOAuthClientId || !cloudflareOAuthClientSecret || !cloudflareOAuthRedirectUrl)
) {
  throw new Error(
    'Cloudflare OAuth setup requires --cloudflare-oauth-client-id, --cloudflare-oauth-client-secret, and --cloudflare-oauth-redirect-url.',
  );
}
if (bootstrapManagedTunnelOAuth && hasCloudflareOAuthInput) {
  throw new Error(
    'Choose either --bootstrap-managed-tunnel-oauth or explicit --cloudflare-oauth-client-* values.',
  );
}
if (bootstrapManagedTunnelOAuth && (!cloudflareApiToken || !cloudflareAccountId)) {
  throw new Error(
    'Managed Tunnel OAuth bootstrap requires CLOUDFLARE_API_TOKEN and CLOUDFLARE_ACCOUNT_ID.',
  );
}
if (!options.has('skip-login') && !hasCloudflareApiToken) {
  await run('pnpm', ['--filter', '@vacps/control-worker', 'exec', 'wrangler', 'login']);
} else if (hasCloudflareApiToken) {
  console.log('Using CLOUDFLARE_API_TOKEN; skipping interactive Wrangler OAuth login.');
}

const databaseId = suppliedDatabaseId ?? (await createDatabase(databaseName));
await updateDatabaseBinding(databaseId);
const oauthKvNamespaceId = suppliedOAuthKvNamespaceId ?? (await createKvNamespace('OAUTH_KV'));
await updateOAuthKvBinding(oauthKvNamespaceId);
// The control-plane identity signs every Worker -> Agent request. Its public half is intentionally
// returned with each one-time registration token so an Agent can authenticate the Worker locally.
const signingSecretNames = await workerSecretNames();
const hasControlPlanePrivateKey = signingSecretNames.has('CONTROL_PLANE_SIGNING_PRIVATE_KEY');
const hasControlPlanePublicKey = signingSecretNames.has('CONTROL_PLANE_SIGNING_PUBLIC_KEY');
if (hasControlPlanePrivateKey !== hasControlPlanePublicKey) {
  throw new Error(
    'Control-plane signing-key secrets are incomplete. Restore both CONTROL_PLANE_SIGNING_* secrets before deploying.',
  );
}
if (!hasControlPlanePrivateKey) {
  const signingKeys = await generateControlPlaneSigningKeys();
  await putSecret('CONTROL_PLANE_SIGNING_PRIVATE_KEY', signingKeys.privateKey);
  await putSecret('CONTROL_PLANE_SIGNING_PUBLIC_KEY', signingKeys.publicKey);
}
await putSecret('CONTROL_PANEL_PASSWORD', suppliedAdminPassword);
await putSecret('CONTROL_PANEL_SESSION_SECRET', sessionSecret);
const oauthConfiguration = bootstrapManagedTunnelOAuth
  ? await createManagedTunnelOAuthClient()
  : hasCloudflareOAuthInput
    ? {
        clientId: cloudflareOAuthClientId,
        clientSecret: cloudflareOAuthClientSecret,
        redirectUrl: cloudflareOAuthRedirectUrl,
      }
    : undefined;
if (oauthConfiguration) {
  await putSecret('CLOUDFLARE_OAUTH_CLIENT_ID', oauthConfiguration.clientId);
  await putSecret('CLOUDFLARE_OAUTH_CLIENT_SECRET', oauthConfiguration.clientSecret);
  await putSecret('CLOUDFLARE_OAUTH_REDIRECT_URL', oauthConfiguration.redirectUrl);
  if (cloudflareAccountId) await putSecret('CLOUDFLARE_ACCOUNT_ID', cloudflareAccountId);
  if (oauthConfiguration.scopes)
    await putSecret('CLOUDFLARE_OAUTH_SCOPES', oauthConfiguration.scopes);
}
await run('pnpm', [
  '--filter',
  '@vacps/control-worker',
  'exec',
  'wrangler',
  'd1',
  'migrations',
  'apply',
  databaseName,
  '--remote',
]);
await run('pnpm', ['--filter', '@vacps/control-worker', 'run', 'deploy']);

console.log('\nControl plane deployed successfully.');
console.log(`D1 database ID: ${databaseId}`);
console.log(`OAUTH_KV namespace ID: ${oauthKvNamespaceId}`);
console.log('Control-panel and control-plane signing secrets were stored as Worker secrets.');
console.log(
  bootstrapManagedTunnelOAuth
    ? 'Managed Tunnel OAuth is ready. Connect Cloudflare in the Web UI, then create a stable node endpoint and run its generated command on the VPS.'
    : hasCloudflareOAuthInput
      ? 'Cloudflare OAuth is ready. Connect Cloudflare in the Web UI, then create a stable node endpoint and run its generated command on the VPS.'
      : 'Next: configure Cloudflare OAuth for Managed Tunnels or use Quick Tunnel from the Web UI, then run its generated installer command on the VPS.',
);

async function createDatabase(name) {
  const output = await capture('pnpm', [
    '--filter',
    '@vacps/control-worker',
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

async function createKvNamespace(binding) {
  const output = await capture('pnpm', [
    '--filter',
    '@vacps/control-worker',
    'exec',
    'wrangler',
    'kv',
    'namespace',
    'create',
    binding,
  ]);
  const namespaceId = findKvNamespaceId(output);
  if (!namespaceId) {
    throw new Error(
      `Could not read the KV namespace ID for ${binding}. Re-run with --oauth-kv-id <id> after creating it in Wrangler.`,
    );
  }
  return namespaceId;
}

function findKvNamespaceId(output) {
  try {
    const parsed = JSON.parse(output);
    const values = Array.isArray(parsed) ? parsed : [parsed];
    for (const value of values) {
      const candidate = value?.id ?? value?.kv_namespaces?.[0]?.id;
      if (typeof candidate === 'string' && isKvNamespaceId(candidate)) return candidate;
    }
  } catch {
    // Fall through to the regular-expression fallback for Wrangler's human-readable output.
  }
  // Wrangler prints a suggested binding block; the namespace ID is a 32-character hex string.
  return (
    output.match(/"id"\s*:\s*"([0-9a-f]{32})"/i)?.[1] ??
    output.match(/id\s*=\s*"([0-9a-f]{32})"/i)?.[1] ??
    output.match(/\b[0-9a-f]{32}\b/i)?.[0]
  );
}

async function updateOAuthKvBinding(namespaceId) {
  const configurationPath = resolve(rootDirectory, 'apps/control-worker/wrangler.jsonc');
  const configuration = await readFile(configurationPath, 'utf8');
  const bindingPattern = /("binding"\s*:\s*"OAUTH_KV"[\s\S]*?"id"\s*:\s*")[^"]+("\s*[,}])/;
  const match = configuration.match(bindingPattern);
  if (!match) {
    throw new Error(`Could not update the OAUTH_KV namespace id in ${configurationPath}.`);
  }
  if (match[0].includes(`"${namespaceId}"`)) return;
  const nextConfiguration = configuration.replace(bindingPattern, `$1${namespaceId}$2`);
  await writeFile(configurationPath, nextConfiguration);
}

async function configuredOAuthKvNamespaceId() {
  const configurationPath = resolve(rootDirectory, 'apps/control-worker/wrangler.jsonc');
  const configuration = await readFile(configurationPath, 'utf8');
  const candidate = configuration.match(
    /"binding"\s*:\s*"OAUTH_KV"[\s\S]*?"id"\s*:\s*"([^"]+)"/,
  )?.[1];
  return candidate && isKvNamespaceId(candidate) ? candidate : undefined;
}

async function createManagedTunnelOAuthClient() {
  const redirectUrl = await managedTunnelRedirectUrl();
  const availableScopes = await cloudflareApi('/oauth/scopes');
  const scopeIds = [
    oauthScopeId(availableScopes, 'Cloudflare Tunnel Write'),
    oauthScopeId(availableScopes, 'DNS Write'),
  ];
  const client = await cloudflareApi(`/accounts/${cloudflareAccountId}/oauth_clients`, {
    method: 'POST',
    body: JSON.stringify({
      client_name: 'VACPS Managed Tunnels',
      grant_types: ['authorization_code', 'refresh_token'],
      redirect_uris: [redirectUrl],
      response_types: ['code'],
      scopes: scopeIds,
      token_endpoint_auth_method: 'client_secret_post',
    }),
  });
  if (!client?.client_id || !client?.client_secret)
    throw new Error('Cloudflare did not return an OAuth Client ID and Client Secret.');
  return {
    clientId: client.client_id,
    clientSecret: client.client_secret,
    redirectUrl,
    scopes: scopeIds.join(' '),
  };
}

async function managedTunnelRedirectUrl() {
  const controlPlane = controlPlaneUrl ?? (await workersDevControlPlaneUrl());
  try {
    const parsed = new URL(controlPlane);
    if (parsed.protocol !== 'https:') throw new Error('A public callback requires HTTPS.');
    return new URL('/api/cloudflare/oauth/callback', parsed).toString();
  } catch {
    throw new Error('CONTROL_PLANE_URL must be a valid HTTPS URL.');
  }
}

async function workersDevControlPlaneUrl() {
  const subdomain = await cloudflareApi(`/accounts/${cloudflareAccountId}/workers/subdomain`);
  if (!subdomain?.subdomain)
    throw new Error(
      'Could not determine the account workers.dev subdomain. Set CONTROL_PLANE_URL.',
    );
  const workerName = await configuredWorkerName();
  return `https://${workerName}.${subdomain.subdomain}.workers.dev`;
}

async function configuredWorkerName() {
  const configurationPath = resolve(rootDirectory, 'apps/control-worker/wrangler.jsonc');
  const configuration = await readFile(configurationPath, 'utf8');
  const workerName = configuration.match(/"name"\s*:\s*"([^"]+)"/)?.[1];
  if (!workerName) throw new Error(`Could not read the Worker name from ${configurationPath}.`);
  return workerName;
}

async function cloudflareApi(path, init = {}) {
  const response = await fetch(`https://api.cloudflare.com/client/v4${path}`, {
    ...init,
    headers: {
      authorization: `Bearer ${cloudflareApiToken}`,
      'content-type': 'application/json',
      ...init.headers,
    },
  });
  const payload = await response.json().catch(() => undefined);
  if (!response.ok || !payload?.success) {
    const message =
      payload?.errors?.[0]?.message ?? `Cloudflare API returned HTTP ${response.status}.`;
    throw new Error(message);
  }
  return payload.result;
}

function oauthScopeId(scopes, name) {
  const scope = Array.isArray(scopes)
    ? scopes.find((candidate) => candidate?.name === name)
    : undefined;
  if (!scope?.id) throw new Error(`Cloudflare OAuth scope not found: ${name}.`);
  return scope.id;
}

async function putSecret(name, value) {
  await run(
    'pnpm',
    ['--filter', '@vacps/control-worker', 'exec', 'wrangler', 'secret', 'put', name],
    `${value}\n`,
  );
}

async function workerSecretNames() {
  try {
    const output = await capture('pnpm', [
      '--filter',
      '@vacps/control-worker',
      'exec',
      'wrangler',
      'secret',
      'list',
    ]);
    try {
      const parsed = JSON.parse(output);
      if (Array.isArray(parsed))
        return new Set(
          parsed
            .map((entry) => (typeof entry?.name === 'string' ? entry.name : undefined))
            .filter(Boolean),
        );
    } catch {
      // Fall through to Wrangler's human-readable output.
    }
    return new Set(output.match(/[A-Z][A-Z0-9_]+/g) ?? []);
  } catch {
    // `wrangler secret list` fails when the Worker does not exist yet (first-time setup).
    return new Set();
  }
}

async function generateControlPlaneSigningKeys() {
  const keyPair = await webcrypto.subtle.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']);
  return {
    privateKey: Buffer.from(await webcrypto.subtle.exportKey('pkcs8', keyPair.privateKey)).toString(
      'base64url',
    ),
    publicKey: Buffer.from(await webcrypto.subtle.exportKey('raw', keyPair.publicKey)).toString(
      'base64url',
    ),
  };
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
    if (key === 'help' || key === 'skip-login' || key === 'bootstrap-managed-tunnel-oauth') {
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

function isKvNamespaceId(value) {
  return /^[0-9a-f]{32}$/i.test(value);
}

function printUsage() {
  console.log(`Usage: pnpm setup:cloudflare [options]

Options:
  --admin-password <password>
                           Control-panel password. Prefer CONTROL_PANEL_PASSWORD
                           so it is not retained in shell history.
  --database-name <name>   D1 name, default: vacps-control.
  --database-id <uuid>     Reuse an existing D1 database instead of creating one.
  --oauth-kv-id <id>       Reuse an existing OAUTH_KV namespace instead of creating one.
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
  --bootstrap-managed-tunnel-oauth
                           Create a scoped private OAuth Client, store its
                           Client ID/Secret, and discard the supplied API Token.
  --control-plane-url <url>
                           Public Worker URL for the OAuth callback. Defaults
                           to the configured workers.dev URL.
  --skip-login             Skip \`wrangler login\` when already authenticated.

Set CLOUDFLARE_API_TOKEN (and preferably CLOUDFLARE_ACCOUNT_ID) to use API-token
authentication instead of the browser OAuth callback. Environment variables avoid
putting secrets in shell history and process arguments. CONTROL_PANEL_PASSWORD is
required; CONTROL_PANEL_SESSION_SECRET is generated securely when omitted.`);
}
