# Deployment guide

## Fast path

From a development machine with `pnpm install` complete, run:

```bash
read -rsp 'Control panel password: ' CONTROL_PANEL_PASSWORD; echo
export CONTROL_PANEL_PASSWORD
pnpm setup:cloudflare
unset CONTROL_PANEL_PASSWORD
```

It logs in to Cloudflare, creates D1, updates the local D1 binding, stores the registration and control-panel authentication secrets, applies migrations, and deploys the Worker. The control-panel password must be at least 12 non-whitespace characters. Save the generated `BACKEND_SHARED_TOKEN` as the **registration secret**: every v1 Agent needs the same value to authenticate its registration request. The session signing secret is generated and stored without being printed.

To secure an existing production Worker before its next deploy, set the two Worker Secrets without placing either value in a command argument:

```bash
read -rsp 'Control panel password: ' CONTROL_PANEL_PASSWORD; echo
printf '%s' "$CONTROL_PANEL_PASSWORD" | pnpm --filter @vps-agent/control-worker exec wrangler secret put CONTROL_PANEL_PASSWORD
node -e "process.stdout.write(require('node:crypto').randomBytes(32).toString('base64url'))" | pnpm --filter @vps-agent/control-worker exec wrangler secret put CONTROL_PANEL_SESSION_SECRET
unset CONTROL_PANEL_PASSWORD
```

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

Configure Cloudflare Access for the control-plane domain (`/` and `/api/*`) as an additional boundary. The registration secret (`BACKEND_SHARED_TOKEN` in the Worker configuration) only authenticates Agent-to-control-plane and control-plane-to-Agent requests; it is not an administrator credential. The control plane independently requires its password-backed, signed session for the UI, administrative API, and MCP endpoint.

### Managed Tunnel (stable, recommended)

The Web UI creates a random node ID, a remotely-managed Cloudflare Tunnel, its ingress route, and the proxied DNS CNAME. A one-time local bootstrap automatically creates a scoped private Cloudflare OAuth client; it does not store the bootstrap API Token.

Create a Cloudflare API Token with these permissions:

- Account: **OAuth Client / Write**, **Workers Scripts / Edit**, and **D1 / Edit**

Run this once from the project checkout. It asks for the account ID and API Token, creates a private OAuth client restricted to **Cloudflare Tunnel / Edit** and **DNS / Edit**, stores only that client ID/secret as Worker Secrets, then discards the API Token:

```bash
pnpm configure:managed-tunnels
```

When a valid D1 ID already exists in `apps/control-worker/wrangler.jsonc`, the bootstrap reuses it; it does not create a second database.
When it is invoked only to add Managed Tunnel OAuth configuration, it also leaves the existing `BACKEND_SHARED_TOKEN` unchanged.

If you initialized Managed Tunnel OAuth with a version released before account context was saved automatically, run `pnpm repair:managed-tunnel-account` once. It only stores the Account ID as a Worker Secret and preserves the existing OAuth Client and authorization.

If an older control-plane deployment created the OAuth Client before account binding was introduced, preserve that Client and add only the missing account context once:

```bash
pnpm bind:managed-tunnel-account
```

This writes only `CLOUDFLARE_ACCOUNT_ID` as a Worker Secret; it does not create an OAuth Client, rotate `BACKEND_SHARED_TOKEN`, or deploy a new Worker version.

In the Web UI, choose **托管 Tunnel** and select **连接 Cloudflare**. The bootstrap already saves the account context; after the browser returns, the UI reads its available Zones. A single Zone is selected automatically; otherwise select the domain name from the dropdown (the Zone ID is never entered manually). The completed VPS command appears on the right after you choose **创建稳定 Tunnel**; **复制命令** has no side effect. Do not manually edit its generated node ID, hostname, or Tunnel Token.

Use the Zone apex as the base domain unless you have an Advanced Certificate for deeper hostnames. If you rotate the OAuth client secret or revoke its Cloudflare authorization, rerun the bootstrap or connect Cloudflare again before creating or removing Managed Tunnels.

### Quick Tunnel (temporary)

Choose **Quick Tunnel** in the Web UI to generate a command with `--quick-tunnel`; no domain, Public Hostname, Tunnel Token, or `--public-url` is required. The installer starts a small supervised `cloudflared` helper, discovers its temporary `trycloudflare.com` URL, and restarts the Agent to register that URL. A reconnect may generate a new URL; the helper updates it and the control plane follows the approved node's renewed registration. This is deliberately for demos and testing, not a stable production endpoint.

An approved node keeps its approval when its Quick Tunnel URL changes: the same `BACKEND_ID` updates its endpoint and it does not return to the pending queue. It may display as offline only until the new temporary URL is reachable.

## VPS Agent

Generate the installer command from the Web UI. It selects either the generated stable endpoint or the temporary Quick Tunnel automatically, so a user never has to enter a speculative `--public-url`.

The installer generates a random `BACKEND_ID` (or receives one generated by the Managed Tunnel flow); each Agent consumes only `agent-<BACKEND_ID>`, so all production nodes may share the same Redis database. It installs Node.js 24 and pnpm 10.14.0 through NVM in `/usr/local/lib/vps-agent/nvm`; systemd receives the resulting Node binary's absolute path, so it never depends on a login shell. Prefer a TLS `rediss://` URL; if a Redis provider only offers `redis://`, use it only when the endpoint is private or tightly firewall-restricted. The installer keeps the HTTP API bound to loopback; do not open port 3100 in the host firewall.

If an installation is interrupted after the repository, environment file, and systemd unit have been created, run the same generated command again. The installer verifies the existing Git origin and Agent project structure, preserves the node ID, repairs the Agent-owned data directories, waits for the authenticated health endpoint, and then continues with Tunnel installation. It reads the checkout's raw Git configuration, so the root-run installer can verify a checkout owned by the unprivileged `agent` user. It refuses to resume when `/opt/vps-agent` is unrelated, belongs to another repository, or the requested node ID does not match.

The Agent automatically posts a pending registration to the control plane after it starts, and retries every five minutes. After approval it also sends a telemetry snapshot: CPU, memory, root-disk usage, queue state, OS/kernel, and upload/download byte rates. Set the shared reporting cadence in the Web UI's **Nodes → Global reporting** control (15–3,600 seconds; default 120). The control plane stores the latest snapshot in D1, and the browser polls that cache; this is intentionally not raw time-series retention. A new cadence takes effect after each Agent's next report.

The Agent uses `TELEMETRY_FALLBACK_INTERVAL_SECONDS=120` only when the control plane cannot return the centrally configured interval. It is generated by the installer and normally does not need editing.

Cloudflare records the registration request IP and its geographic metadata, then shows the node as a card with cached telemetry and approval state. Approval runs authenticated health and metrics checks; it will fail safely until the Tunnel route is reachable. The Web UI deliberately has no task-creation form: submit operational tasks through Remote MCP or schedules.

`--allow-apt` is optional and grants `agent` passwordless `sudo apt-get`. Package maintainer scripts run as root, so treat this exactly like root access.

## Uninstall a VPS Agent

For a Managed Tunnel, first choose **移除节点 / Remove node** on its card in the Web UI. This lets the control plane clean up the registered backend and managed Cloudflare resources. Then run the uninstaller on the VPS:

```bash
curl -fsSL https://<your-control-plane>/agent.sh | sudo bash -s -- uninstall
```

It stops and removes the Agent, its local configuration, Quick Tunnel helper, Agent-scoped NVM runtime, and optional apt sudoers rule. It intentionally preserves `/var/lib/vps-agent` (SQLite task records and logs), the `agent` user, and cloudflared. To delete the preserved Agent data and system user, pass both explicit flags:

```bash
curl -fsSL https://<your-control-plane>/agent.sh | sudo bash -s -- uninstall --purge-data --remove-user
```

If this host uses no other cloudflared service, add `--remove-managed-tunnel` to stop and remove the local cloudflared system service. This never deletes the remote Tunnel or DNS record; remove the node card in the Web UI for that cleanup.

## Smoke test

From the Worker or another trusted machine, call:

```bash
curl -H "Authorization: Bearer $BACKEND_SHARED_TOKEN" https://agent.example.com/health
```

After the Agent appears in the approval queue and is approved, run a harmless Shell task such as `pwd && uname -a` through MCP with a known safe working directory.
