# Contributing

## Development checks

```bash
pnpm install
pnpm check
```

Keep the shared Zod schemas in `packages/contracts` in sync with both HTTP APIs. New task states, profiles, or scheduler fields must be represented in the D1 migration and VPS SQLite storage where applicable.

## Pull requests

- Keep changes scoped to one concern.
- Add tests for pure validation and executor behavior when changed.
- Do not commit `.env`, D1 state, SQLite databases, output logs, or access tokens.
- Explain migration and rollout requirements for operational changes.
