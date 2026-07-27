# VPS Agent Platform

Cloudflare Workers control plane for queued Shell and Pi-powered agent work on multiple VPS hosts. It is a TypeScript pnpm monorepo with exactly three workspaces:

- `apps/control-worker` — same-domain Web UI, management API, D1 registry, schedule reconciliation, and Remote MCP.
- `apps/vps-agent` — the single process deployed to each VPS: Fastify, BullMQ, LangGraph lifecycle, SQLite, Pi adapter, and Shell executor.
- `packages/contracts` — shared Zod schemas and API contracts.

> **Security warning:** v1 deliberately supports arbitrary commands. Deploy it behind Cloudflare Access, keep the control-panel password private, and do not expose a VPS Agent port directly to the Internet. Each Agent has its own Ed25519 identity; no shared backend bearer secret exists.

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

- Node.js 24 LTS and pnpm 10.14.0 (the VPS installer installs these through an Agent-scoped NVM directory)
- A Redis instance reachable from each VPS. Use TLS (`rediss://`) whenever traffic crosses a public or untrusted network; a non-TLS `redis://` endpoint must be private and firewall-restricted.
- A Cloudflare account with Workers, D1, Access, and (recommended) Tunnel
- A Pi adapter that implements the included NDJSON protocol

## Quick deployment

The project provides an interactive control-plane bootstrap and a non-Docker VPS installer. Redis is the only external runtime dependency.

```bash
read -rsp 'Control panel password: ' CONTROL_PANEL_PASSWORD; echo
export CONTROL_PANEL_PASSWORD
# Logs in to Cloudflare, creates/binds D1 and KV, creates Worker secrets and a control-plane
# Ed25519 signing identity, migrates, and deploys.
pnpm setup:cloudflare
unset CONTROL_PANEL_PASSWORD
```

`CONTROL_PANEL_PASSWORD` must be at least 12 non-whitespace characters. The setup stores it only as a Worker Secret and generates the session-signing secret without printing it. Prefer the environment-variable form over `--admin-password`, which can be retained in shell history.

If Wrangler's browser callback is unavailable (a WSL networking issue, for
example), create a scoped Cloudflare API Token and run the same command without
interactive login. Set `CLOUDFLARE_ACCOUNT_ID` to the account ID shown in the
Cloudflare dashboard, and grant the token Account-level `Workers Scripts: Edit`,
`D1: Edit`, and `Workers KV Storage: Edit` permissions.

```bash
export CLOUDFLARE_ACCOUNT_ID='<cloudflare-account-id>'
read -rsp 'Cloudflare API Token: ' CLOUDFLARE_API_TOKEN; echo
export CLOUDFLARE_API_TOKEN
pnpm setup:cloudflare
unset CLOUDFLARE_API_TOKEN
```

The equivalent parameter form is `pnpm setup:cloudflare -- --cloudflare-account-id <id> --cloudflare-api-token <token>`. Prefer the environment-variable form because command-line tokens can be recorded in shell history and visible to local processes.

Open the deployed Web UI and choose one of its connection modes before copying the VPS command:

- **Managed Tunnel** creates a random node ID, stable hostname, Cloudflare Tunnel, and DNS record. A one-time local bootstrap uses an API Token to create a scoped OAuth client, then discards the Token; it never reaches the Worker, browser, VPS, or installer command. The bootstrap saves only the selected Cloudflare account context, and the Web UI loads Zones automatically after browser authorization.
- **Quick Tunnel** creates a temporary `trycloudflare.com` URL on the VPS and re-registers the Agent whenever that URL changes. Use it only for demos or testing.

Before copying an installer command, use the Web UI to generate a one-time registration Token. It is shown once, lasts ten minutes, and is consumed when the Agent binds its locally generated Ed25519 public key. The installer installs Node.js 24 and pnpm 10.14.0 through an Agent-scoped NVM directory, generates that key pair locally, builds the agent, creates its systemd unit, configures SQLite/log directories, and installs `cloudflared`. After startup the Agent registers itself as **pending**; approve its card in the Web UI after the health check succeeds.

Each report writes one current snapshot containing CPU, memory, root-disk usage, queue state, operating system, and upload/download byte rates. D1 keeps only the latest snapshot, which makes the UI inexpensive to poll and leaves a clean input for future roll-up charts; it is not raw time-series retention.

To remove an Agent from a VPS, first remove its node card from the Web UI when it uses a Managed Tunnel, then run `agent.sh uninstall` from the control-plane endpoint as root. The uninstaller preserves `/var/lib/vps-agent` by default; use `--purge-data --remove-user` only when deleting its SQLite task history, logs, and service user is intended.

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
