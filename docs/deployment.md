# Deployment guide

## Fast path

From a development machine with `pnpm install` complete, run:

```bash
pnpm setup:cloudflare
```

It logs in to Cloudflare, creates D1, updates the local D1 binding, stores `BACKEND_SHARED_TOKEN`, applies migrations, and deploys the Worker. Save the generated token: every v1 Agent needs the same value.

If the browser reports that the `localhost:8976` OAuth callback is unavailable,
use a Cloudflare API Token instead. Create a custom Account token scoped to the
target account with `Workers Scripts: Edit` and `D1: Edit`, then run:

```bash
export CLOUDFLARE_ACCOUNT_ID='<cloudflare-account-id>'
read -rsp 'Cloudflare API Token: ' CLOUDFLARE_API_TOKEN; echo
export CLOUDFLARE_API_TOKEN
pnpm setup:cloudflare
unset CLOUDFLARE_API_TOKEN
```

The bootstrap detects `CLOUDFLARE_API_TOKEN` and skips interactive OAuth. Do
not place this token in the repository, `.env` files committed to Git, or a VPS.
As an alternative, pass `--cloudflare-account-id <id>` and
`--cloudflare-api-token <token>` after `pnpm setup:cloudflare --`; avoid this
form on a shared computer because the token may be kept in shell history and
exposed in process arguments.

Create a remotely managed Tunnel in the Cloudflare dashboard, copy its install token, and add a published route for each VPS hostname to `http://127.0.0.1:3100`. Then configure Cloudflare Access for the control-plane domain (`/` and `/api/*`).

## VPS Agent

Run the installer on the VPS:

```bash
curl -fsSL https://<your-worker-domain>/install-agent.sh | sudo bash -s -- \
  --repo https://github.com/<owner>/vps-agent-platform.git \
  --backend-id vps-la-01 \
  --backend-token <control-plane-token> \
  --redis-url 'rediss://default:<password>@<host>:<port>' \
  --tunnel-token <tunnel-token>
```

Each Agent consumes only `agent-<BACKEND_ID>`, so all production nodes may share the same TLS Redis database. The installer keeps the HTTP API bound to loopback; do not open port 3100 in the host firewall.

`--allow-apt` is optional and grants `agent` passwordless `sudo apt-get`. Package maintainer scripts run as root, so treat this exactly like root access.

## Smoke test

From the Worker or another trusted machine, call:

```bash
curl -H "Authorization: Bearer $BACKEND_SHARED_TOKEN" https://agent.example.com/health
```

Add the returned endpoint as a Backend in the Web UI. Then run a harmless Shell task such as `pwd && uname -a` with a known safe working directory.
