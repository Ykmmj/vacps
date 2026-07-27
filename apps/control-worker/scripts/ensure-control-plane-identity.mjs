#!/usr/bin/env node

import { spawn } from 'node:child_process';
import { webcrypto } from 'node:crypto';

const secretNames = await existingSecretNames();
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

async function existingSecretNames() {
  try {
    const output = await capture(['exec', 'wrangler', 'secret', 'list']);
    try {
      const parsed = JSON.parse(output);
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
  } catch {
    // On the first deployment, `secret list` can fail because the Worker has not been created yet.
    return new Set();
  }
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

function capture(args) {
  return new Promise((resolve, reject) => {
    const child = spawn('pnpm', args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => (output += chunk));
    child.stderr.on('data', (chunk) => (output += chunk));
    child.once('error', reject);
    child.once('close', (code) => {
      if (code === 0) resolve(output);
      else reject(new Error(`pnpm ${args.join(' ')} exited with ${code ?? 'an unknown signal'}.`));
    });
  });
}
