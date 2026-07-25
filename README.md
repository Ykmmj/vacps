# VPS Agent Platform

Cloudflare Workers control plane for queued Shell and Pi-powered agent work on multiple VPS hosts. It is a TypeScript pnpm monorepo with exactly three workspaces:

- `apps/control-worker` — same-domain Web UI, management API, D1 registry, schedule reconciliation, and Remote MCP.
- `apps/vps-agent` — the single process deployed to each VPS: Fastify, BullMQ, LangGraph lifecycle, SQLite, Pi adapter, and Shell executor.
- `packages/contracts` — shared Zod schemas and API contracts.

> **Security warning:** v1 deliberately supports arbitrary commands. Deploy it only behind Cloudflare Access and use a long, secret backend token. Do not expose a VPS Agent port directly to the Internet.

## Architecture

```text
Web UI / MCP → Cloudflare Worker + D1 → HTTPS → target VPS Agent
                                              ├→ BullMQ + Redis
                                              ├→ LangGraph lifecycle
                                              ├→ Pi adapter / ShellExecutor
                                              └→ SQLite + log files
```

Cloudflare does not connect to Redis or execute Shell commands. A task UUID is created by the control plane and used as the D1 task ID, BullMQ job ID, LangGraph `thread_id`, and SQLite task ID.

## Prerequisites

- Node.js 22.14 or later (use an active LTS release; native SQLite prebuilds are published for LTS versions)
- pnpm 10 or later (`corepack enable` and `corepack prepare pnpm@10.14.0 --activate`)
- A TLS-enabled Redis instance reachable from each VPS
- A Cloudflare account with Workers, D1, Access, and (recommended) Tunnel
- A Pi adapter that implements the included NDJSON protocol

## Quick deployment

The project provides an interactive control-plane bootstrap and a non-Docker VPS installer. Redis is the only external runtime dependency.

```bash
# Logs in to Cloudflare, creates/binds D1, stores the Backend token, migrates, and deploys.
pnpm setup:cloudflare
```

If Wrangler's browser callback is unavailable (a WSL networking issue, for
example), create a scoped Cloudflare API Token and run the same command without
interactive login. Set `CLOUDFLARE_ACCOUNT_ID` to the account ID shown in the
Cloudflare dashboard, and grant the token Account-level `Workers Scripts: Edit`
and `D1: Edit` permissions.

```bash
export CLOUDFLARE_ACCOUNT_ID='<cloudflare-account-id>'
read -rsp 'Cloudflare API Token: ' CLOUDFLARE_API_TOKEN; echo
export CLOUDFLARE_API_TOKEN
pnpm setup:cloudflare
unset CLOUDFLARE_API_TOKEN
```

The equivalent parameter form is `pnpm setup:cloudflare -- --cloudflare-account-id <id> --cloudflare-api-token <token>`. Prefer the environment-variable form because command-line tokens can be recorded in shell history and visible to local processes.

```bash
# Create a Tunnel hostname for the VPS first, then run this on the VPS.
curl -fsSL https://<your-worker-domain>/install-agent.sh | sudo bash -s -- \
  --repo https://github.com/<owner>/vps-agent-platform.git \
  --backend-name 'Los Angeles VPS' \
  --control-plane-url https://<your-worker-domain> \
  --public-url https://la-agent.example.com \
  --backend-token <token-printed-by-setup> \
  --redis-url 'rediss://default:<password>@<host>:<port>' \
  --tunnel-token <cloudflare-tunnel-token>
```

The installer generates its node ID automatically, downloads Node.js 22 LTS when necessary, builds the agent, creates its systemd unit, configures SQLite/log directories, and installs `cloudflared`. A remotely managed Tunnel route must point the chosen hostname to `http://127.0.0.1:3100`. After startup the Agent registers itself as **pending**; approve its card in the Web UI after the health check succeeds.

To allow the Agent to install system packages, add `--allow-apt`. This writes an `apt-get` sudoers rule and is root-equivalent; it is intentionally disabled by default.

## Local development

```bash
pnpm install
pnpm check

# Create a local D1 database and apply the control-plane migration.
pnpm --filter @vps-agent/control-worker exec wrangler d1 create vps-agent-control
pnpm --filter @vps-agent/control-worker exec wrangler d1 migrations apply vps-agent-control --local

# Copy and fill the example files before starting either runtime.
cp apps/control-worker/.dev.vars.example apps/control-worker/.dev.vars
cp apps/vps-agent/.env.example apps/vps-agent/.env
```

Run the control plane with `pnpm dev:control`. Start a local VPS Agent only after supplying Redis and a safe test directory in its environment: `pnpm dev:agent`.

See [`docs/deployment.md`](docs/deployment.md) for the security/operations checklist and [`docs/pi-adapter-protocol.md`](docs/pi-adapter-protocol.md) for the Pi integration boundary.

## Current implementation status

The repository implements the v1 skeleton and the minimum VPS execution path: authenticated task admission, per-VPS queueing, SQLite task/command records, bounded Shell logs, cancellation, a five-node LangGraph flow, D1 registry/task/schedule APIs, Remote MCP tools, and a Svelte + Tailwind approval console. Tasks are created through MCP or schedules; the Web UI is reserved for Agent installation and registration approval.

Two integration tasks are intentionally environment-specific:

- Configure a concrete Pi SDK/CLI adapter using the documented protocol. This prevents Pi from bypassing command policy or audit logs.
- Decide how scheduled tasks are indexed back into D1 when they are emitted autonomously by BullMQ. `schedules.run_now` is fully indexed today; scheduled jobs are persisted and executed on the target VPS, but their control-plane index callback needs a deployment-specific authenticated webhook.

## License

[MIT](LICENSE).
