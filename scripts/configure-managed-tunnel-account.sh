#!/usr/bin/env bash

set -euo pipefail

read -rp 'Cloudflare Account ID: ' CLOUDFLARE_ACCOUNT_ID
read -rsp 'Cloudflare API Token: ' CLOUDFLARE_API_TOKEN
echo

if [[ ! "$CLOUDFLARE_ACCOUNT_ID" =~ ^[0-9a-fA-F]{32}$ ]]; then
  echo 'Cloudflare Account ID must be 32 hexadecimal characters.' >&2
  exit 1
fi
if [[ -z "$CLOUDFLARE_API_TOKEN" ]]; then
  echo 'Cloudflare API Token is required.' >&2
  exit 1
fi

printf '%s' "$CLOUDFLARE_ACCOUNT_ID" |
  CLOUDFLARE_ACCOUNT_ID="$CLOUDFLARE_ACCOUNT_ID" \
    CLOUDFLARE_API_TOKEN="$CLOUDFLARE_API_TOKEN" \
    pnpm --filter @vps-agent/control-worker exec wrangler secret put CLOUDFLARE_ACCOUNT_ID

unset CLOUDFLARE_API_TOKEN CLOUDFLARE_ACCOUNT_ID
echo 'Managed Tunnel account context saved. Refresh the Web UI and connect Cloudflare again.'
