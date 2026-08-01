# Process domain model (post-Registry)

See also [`NATIVE_RESOURCE_OWNERSHIP.md`](./NATIVE_RESOURCE_OWNERSHIP.md).

## Layers

```text
ProcessRuntime     shared executor + ProcessBudget (technical limits only)
Process            JS-owned handle; owns Process::State (child, pipes, timers, buffers)
ProcessBudget      max concurrent starts + global buffer budget (reject, never reclaim live)
```

## Removed

- `Registry` string id map
- finished-entry TTL
- reclaim-oldest-finished behind live handles
- `alive_flag` + raw `Registry*`
- Host `kill_all` as business orchestration (backend teardown is not a process table)

## Limits

On over budget:

- **reject** `start()` if at max concurrent processes
- **truncate** buffer growth when global/per-process hard max hit

Never delete another live Process’s state.

## Destructor policy

`~Process` / `dispose()`: **kill** still-running child (process group SIGKILL), cancel timers, close pipes. Not detach.
