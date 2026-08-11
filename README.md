# VACPS

VACPS is a Cloudflare control plane and a native multi-host task Agent.

- `apps/control-worker` — same-domain Web UI, management API, D1 registry, schedule coordination, and Remote MCP.
- `apps/vacps-native` — the production VPS Agent: a static C++23/QuickJS runtime plus its TypeScript business script.
- `packages/contracts` — shared Zod wire schemas and scheduling semantics.

> **Security warning:** VACPS deliberately supports arbitrary commands. Deploy the control plane behind its authentication boundaries, expose Agents only through Cloudflare Tunnel, and protect every Agent identity key. Enabling `--allow-root` makes authenticated command execution root-equivalent.

## Architecture

```text
Web UI / MCP → Cloudflare Worker + D1 → signed HTTPS → native VACPS Agent
                                                        ├→ QuickJS business script
                                                        ├→ Asio HTTP/FS/process modules
                                                        └→ SQLite queue, schedules, and logs
```

The Worker owns registration, approval, fleet state, Remote MCP, and the D1 index. Each VPS runs the native Agent, keeps its execution state in SQLite, and exposes only its loopback HTTP listener through a Cloudflare Tunnel. Agent and control-plane requests use per-node Ed25519 signatures with timestamped nonces.

## Prerequisites

- Node.js 22 or newer and pnpm 10 for control-plane development and deployment.
- Docker with the repository's native build image for compiling the Agent.
- A Cloudflare account with Workers, D1, KV, and, preferably, Tunnel.
- Linux x86_64 for the published static Agent artifact.

The deployed VPS does not need Node.js, pnpm, or a source checkout. The installer downloads the selected native release.

## Quick deployment

From a development checkout with `pnpm install` complete:

```bash
read -rsp 'Control panel password: ' CONTROL_PANEL_PASSWORD; echo
export CONTROL_PANEL_PASSWORD
pnpm setup:cloudflare
unset CONTROL_PANEL_PASSWORD
```

`CONTROL_PANEL_PASSWORD` must contain at least 12 non-whitespace characters. The setup creates or binds D1 and KV, configures Worker secrets and the control-plane Ed25519 identity, applies migrations, and deploys the Worker.

Open the deployed Web UI, choose Managed Tunnel or Quick Tunnel, generate a one-time registration token, and copy the generated native installer command. The Agent runs as the user who invoked deployment (`SUDO_USER` when invoked through `sudo`); the installer does not create a login account.

Lifecycle commands are served from the same `agent.sh` endpoint:

- `install` — install or resume the native Agent.
- `upgrade` — download a selected native release and restart while preserving identity and data.
- `reinstall` — uninstall and install with a fresh registration token.
- `uninstall` — remove service files while preserving `/var/lib/vacps` by default.

Example upgrade:

```bash
curl -fsSL https://<your-control-plane>/agent.sh | sudo bash -s -- upgrade \
  --native-version 0.1.10
```

See [docs/deployment.md](docs/deployment.md) for Tunnel, identity, upgrade, reinstall, and uninstall details.

## Local development

Install and validate the pnpm workspaces:

```bash
pnpm install
pnpm check
```

Start the control plane with `pnpm dev:control` after creating `apps/control-worker/.dev.vars` and local D1 bindings.

Build and exercise the native Agent only through its Docker entrypoint, with at most four compiler jobs:

```bash
CMAKE_BUILD_PARALLEL_LEVEL=4 apps/vacps-native/docker/build.sh release
```

The release build compiles the C++ runtime, builds the JavaScript bundle, and runs product JavaScript smoke tests. Native design, module surfaces, ownership rules, and coding requirements are documented under [apps/vacps-native/docs](apps/vacps-native/docs).

## Current implementation

The native Agent provides signed registration and telemetry, command and shell execution, cancellation, SQLite-backed tasks and schedules, bounded logs, file and Git tools, and a QuickJS product layer over native HTTP, filesystem, process, crypto, text, URL, and storage modules. The control plane provides approval, fleet status, D1 task/schedule indexes, Remote MCP tools, and the installation UI.

## License

[MIT](LICENSE)
