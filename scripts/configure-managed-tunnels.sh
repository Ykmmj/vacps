#!/usr/bin/env bash

set -euo pipefail

read -rp 'Cloudflare Account ID: ' CLOUDFLARE_ACCOUNT_ID
read -rsp 'Cloudflare API Token: ' CLOUDFLARE_API_TOKEN
echo

if [[ -z "$CLOUDFLARE_ACCOUNT_ID" || -z "$CLOUDFLARE_API_TOKEN" ]]; then
  echo 'Account ID and API Token are required.' >&2
  exit 1
fi

export CLOUDFLARE_ACCOUNT_ID
export CLOUDFLARE_API_TOKEN
export BOOTSTRAP_MANAGED_TUNNEL_OAUTH=true

pnpm setup:cloudflare

unset BOOTSTRAP_MANAGED_TUNNEL_OAUTH CLOUDFLARE_API_TOKEN CLOUDFLARE_ACCOUNT_ID
