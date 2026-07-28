#!/usr/bin/env node

import { spawn } from 'node:child_process';
import { webcrypto } from 'node:crypto';

const secretNames = await ensureSecretListing();
const hasPrivateKey = secretNames.has('CONTROL_PLANE_SIGNING_PRIVATE_KEY');
const hasPublicKey = secretNames.has('CONTROL_PLANE_SIGNING_PUBLIC_KEY');

if (hasPrivateKey !== hasPublicKey) {
  throw new Error(
    'Control-plane signing-key secrets are incomplete. Restore both CONTROL_PLANE_SIGNING_* secrets before deploying.',
  );
}

if (!hasPrivateKey) {
  const keys = await generateIdentity();
  await putSecret('CONTROL_PLANE_SIGNING_PRIVATE_KEY', keys.privateKey);
  await putSecret('CONTROL_PLANE_SIGNING_PUBLIC_KEY', keys.publicKey);
  console.log('Created the control-plane Ed25519 signing identity.');
} else {
  console.log('Control-plane Ed25519 signing identity already exists.');
}

/**
 * Secret list requires the Worker script to exist. On first deploy, bootstrap a bare
 * `wrangler deploy` (build UI + assets) so identity secrets can be stored.
 */
async function ensureSecretListing() {
  let probe = await probeWorkerSecrets();
  if (probe.exists) return probe.names;

  console.log(
    'Worker script is not deployed yet; performing an initial deploy so identity secrets can be stored...',
  );
  await run(['run', 'build:ui']);
  await run(['run', 'copy:installer']);
  await run(['exec', 'wrangler', 'deploy']);

  probe = await probeWorkerSecrets();
  if (!probe.exists) {
    throw new Error(
      'Initial Worker deploy finished, but secret list still reports the Worker as missing. Check CLOUDFLARE_ACCOUNT_ID and token permissions (Workers Scripts: Edit).',
    );
  }
  return probe.names;
}

async function probeWorkerSecrets() {
  const result = await captureResult(['exec', 'wrangler', 'secret', 'list']);
  if (result.code === 0) {
    return { exists: true, names: parseSecretNames(result.output) };
  }
  if (isWorkerNotFoundError(result.output)) {
    return { exists: false, names: new Set() };
  }
  process.stderr.write(result.output);
  throw new Error(`pnpm exec wrangler secret list exited with ${result.code}.`);
}

function parseSecretNames(output) {
  try {
    const jsonLine =
      output
        .trim()
        .split('\n')
        .find((line) => line.startsWith('[')) ?? output;
    const parsed = JSON.parse(jsonLine);
    if (Array.isArray(parsed)) {
      return new Set(
        parsed
          .map((entry) => (typeof entry?.name === 'string' ? entry.name : undefined))
          .filter(Boolean),
      );
    }
  } catch {
    // Wrangler's human-readable output is supported below.
  }
  return new Set(output.match(/[A-Z][A-Z0-9_]+/g) ?? []);
}

function isWorkerNotFoundError(text) {
  return (
    /Worker ["'][^"']+["'] not found/i.test(text) ||
    /\[code:\s*10007\]/i.test(text) ||
    /script not found/i.test(text)
  );
}

async function generateIdentity() {
  const pair = await webcrypto.subtle.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']);
  return {
    privateKey: Buffer.from(await webcrypto.subtle.exportKey('pkcs8', pair.privateKey)).toString(
      'base64url',
    ),
    publicKey: Buffer.from(await webcrypto.subtle.exportKey('raw', pair.publicKey)).toString(
      'base64url',
    ),
  };
}

async function putSecret(name, value) {
  await run(['exec', 'wrangler', 'secret', 'put', name], `${value}\n`);
}

function run(args, input) {
  return new Promise((resolve, reject) => {
    const child = spawn('pnpm', args, {
      stdio: input === undefined ? 'inherit' : ['pipe', 'inherit', 'inherit'],
    });
    child.once('error', reject);
    child.once('close', (code) => {
      if (code === 0) resolve();
      else reject(new Error(`pnpm ${args.join(' ')} exited with ${code ?? 'an unknown signal'}.`));
    });
    if (input !== undefined) child.stdin.end(input);
  });
}

function captureResult(args) {
  return new Promise((resolve, reject) => {
    const child = spawn('pnpm', args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => (output += chunk));
    child.stderr.on('data', (chunk) => (output += chunk));
    child.once('error', reject);
    child.once('close', (code) => {
      resolve({ code: code ?? 1, output });
    });
  });
}
