# Security Policy

## Reporting a vulnerability

Do not open a public issue for a credential leak, authentication bypass, command-injection path, or privilege escalation. Report it privately to the repository maintainers with reproduction steps and affected versions.

## Deployment requirements

- Put the control-plane domain, Web UI, management API, and `/mcp` behind Cloudflare Access or an OAuth-aware MCP handler.
- Use a unique, random `BACKEND_SHARED_TOKEN` of at least 32 characters. Store it only as a Worker secret and a VPS environment file readable by the agent service account.
- Expose the Agent only through Cloudflare Tunnel; keep `LISTEN_HOST=127.0.0.1`.
- Use a dedicated Linux user for the Agent. NOPASSWD sudo turns control-plane compromise into root access.
- Use TLS Redis (`rediss://`) whenever possible and do not expose Redis to the public Internet. If a provider only offers `redis://`, keep it on a private or otherwise tightly restricted network.
- Back up D1 and the VPS SQLite database. Redis is runtime state, not the system of record.
- Treat all task logs as potentially sensitive. Never emit tokens, model keys, or environment dumps into command output.
