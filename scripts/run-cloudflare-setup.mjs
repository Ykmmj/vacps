#!/usr/bin/env node

import { spawn } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const rootDirectory = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const environment = { ...process.env };
const httpsProxy = environment.HTTPS_PROXY ?? environment.https_proxy;
const httpProxy = environment.HTTP_PROXY ?? environment.http_proxy;

if (httpsProxy || httpProxy) {
  if (!environment.HTTPS_PROXY && httpsProxy) environment.HTTPS_PROXY = httpsProxy;
  if (!environment.HTTP_PROXY && httpProxy) environment.HTTP_PROXY = httpProxy;
  if (!environment.NO_PROXY && environment.no_proxy) environment.NO_PROXY = environment.no_proxy;
  if (!environment.NODE_USE_ENV_PROXY) environment.NODE_USE_ENV_PROXY = '1';
  console.log('Proxy environment detected; enabling Node environment proxy support.');
}

const child = spawn(
  process.execPath,
  [resolve(rootDirectory, 'scripts/setup-cloudflare.mjs'), ...process.argv.slice(2)],
  { env: environment, stdio: 'inherit' },
);

child.once('error', (error) => {
  throw error;
});
child.once('exit', (code, signal) => {
  if (signal) process.kill(process.pid, signal);
  else process.exitCode = code ?? 1;
});
