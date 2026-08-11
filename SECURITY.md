# Security Policy

## Reporting a vulnerability

Do not open a public issue for a credential leak, authentication bypass, command-injection path, or privilege escalation. Report it privately to the repository maintainers with reproduction steps and affected versions.

## Deployment requirements

- Put the control-plane domain, Web UI, management API, and `/mcp` behind Cloudflare Access or an OAuth-aware MCP handler.
- Protect the control-plane signing key and every Agent's locally generated Ed25519 private key. Never copy an Agent private key to the Worker or browser.
- Expose the Agent only through Cloudflare Tunnel; keep `VACPS_LISTEN_HOST=127.0.0.1`.
- The Agent runs as the account that invokes deployment. `--allow-root` or direct root deployment turns control-plane compromise into root access.
- Back up D1 and the VPS SQLite database.
- Treat all task logs as potentially sensitive. Never emit tokens, model keys, or environment dumps into command output.
