#!/usr/bin/env bash
# End-to-end verification of the Remote MCP OAuth flow.
# Runs dynamic client registration -> PKCE -> consent approval -> token exchange
# -> authenticated MCP call, all headlessly. Prompts for the control-panel
# password locally (never echoed, never passed as an argument).
#
# Usage: bash scripts/verify-mcp-oauth.sh https://<your-control-domain>
set -euo pipefail

BASE="${1:?Usage: verify-mcp-oauth.sh <control-plane-base-url>}"
BASE="${BASE%/}"
REDIRECT="http://localhost:9999/callback"
REDIRECT_ENC="http%3A%2F%2Flocalhost%3A9999%2Fcallback"

json() { node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{try{process.stdout.write(String(JSON.parse(s)[process.argv[1]]??""))}catch{}})' "$1"; }

echo "1) Dynamic client registration…"
REG=$(curl -s -X POST "$BASE/register" -H 'content-type: application/json' \
  --data '{"client_name":"verify-mcp-oauth","redirect_uris":["'"$REDIRECT"'"],"grant_types":["authorization_code","refresh_token"],"response_types":["code"],"token_endpoint_auth_method":"none"}')
CLIENT_ID=$(printf '%s' "$REG" | json client_id)
[ -n "$CLIENT_ID" ] || { echo "   FAILED to register client: $REG"; exit 1; }
echo "   client_id=$CLIENT_ID"

echo "2) Generating PKCE challenge…"
VERIFIER=$(openssl rand -base64 60 | tr -d '\n=+/' | cut -c1-64)
CHALLENGE=$(printf '%s' "$VERIFIER" | openssl dgst -sha256 -binary | openssl base64 | tr '+/' '-_' | tr -d '=\n')
STATE="verify-$$"
AUTHZ="$BASE/authorize?response_type=code&client_id=$CLIENT_ID&redirect_uri=$REDIRECT_ENC&scope=mcp&state=$STATE&code_challenge=$CHALLENGE&code_challenge_method=S256"

read -rsp "Control panel password: " PW; echo
echo "3) Approving consent…"
LOCATION=$(curl -s -o /dev/null -D - "$AUTHZ" -X POST \
  --data-urlencode "decision=approve" --data-urlencode "password=$PW" \
  | tr -d '\r' | grep -i '^location:' | sed 's/^[Ll]ocation: //')
PW=""
CODE=$(printf '%s' "$LOCATION" | sed -n 's/.*[?&]code=\([^&]*\).*/\1/p')
# The code is `userId:grantId:secret`; URLSearchParams percent-encodes the colons in the
# redirect, so decode once here and let --data-urlencode re-encode it correctly on the wire.
CODE=$(printf '%s' "$CODE" | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>process.stdout.write(decodeURIComponent(s.trim())))')
[ -n "$CODE" ] || { echo "   FAILED — no auth code (wrong password or denied). Redirect: ${LOCATION:-<none>}"; exit 1; }
echo "   got authorization code"

echo "4) Exchanging code for an access token…"
TOKENS=$(curl -s -X POST "$BASE/token" \
  --data-urlencode "grant_type=authorization_code" \
  --data-urlencode "code=$CODE" \
  --data-urlencode "redirect_uri=$REDIRECT" \
  --data-urlencode "client_id=$CLIENT_ID" \
  --data-urlencode "code_verifier=$VERIFIER")
ACCESS=$(printf '%s' "$TOKENS" | json access_token)
[ -n "$ACCESS" ] || { echo "   FAILED token exchange: $TOKENS"; exit 1; }
echo "   got access token"

echo "5) Authenticated MCP initialize…"
RESP=$(curl -s -w $'\n%{http_code}' -X POST "$BASE/mcp" \
  -H "authorization: Bearer $ACCESS" \
  -H 'content-type: application/json' \
  -H 'accept: application/json, text/event-stream' \
  --data '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"verify","version":"0"}}}')
STATUS=$(printf '%s' "$RESP" | tail -n1)
BODY=$(printf '%s' "$RESP" | sed '$d')
if [ "$STATUS" = "200" ] && printf '%s' "$BODY" | grep -q 'serverInfo'; then
  echo "   PASS — /mcp accepted the token (HTTP $STATUS, server responded to initialize)"
  echo
  echo "OAuth flow verified end-to-end. ✅"
else
  echo "   UNEXPECTED — HTTP $STATUS"
  printf '%s\n' "$BODY" | head -c 500
  exit 1
fi
