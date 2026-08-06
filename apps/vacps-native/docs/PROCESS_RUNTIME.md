# Process domain model

See also [`NATIVE_RESOURCE_OWNERSHIP.md`](./NATIVE_RESOURCE_OWNERSHIP.md).

## Layers

```text
ProcessRuntime     shared main_executor + ProcessBudget (technical limits only)
Process            JS-owned handle; owns Process::State (child, pipes, timers, buffers)
ProcessBudget      max concurrent starts + global buffer budget (reject, never reclaim live)
ProcessSessions    JS product protocol IDs + bounded terminal retention + shutdown close
```

JS surface is `vacps:process` (`Process` class + `run`), registered in ModuleCatalog.
All process domain work runs on `Runtime::main_executor` (never the worker
pool, never another `io_context`).

## Removed

- `Registry` string id map / finished-entry TTL / reclaim
- public `pid` / `running` JS bindings
- Host `kill_all` as business orchestration
- env / stdout / stderr mode API fiction; `hardMax*` / `closeStdin` aliases

## Lifecycle

| API                        | Behavior                                                                                                                                                                                                                                                                                     |
| -------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `start`                    | Acquire budget slot, spawn with `setpgid(0,0)`, arm timeout, drain stdout/stderr, retain the Boost.Process handle and asynchronously reap it.                                                                                                                                                |
| `read`                     | Return retained stdout/stderr chunks in observed completion order from a `(sequence, byteOffset)` cursor. Optional wait uses an Asio timer woken by output or terminal completion; no polling thread.                                                                                        |
| `snapshot`                 | Owner-turn synchronous current status plus independently bounded stdout/stderr previews and exact produced-byte/truncation facts.                                                                                                                                                            |
| `waitForExit`              | Join real child reap + both stream drains and return only small terminal facts. Optional timeout returns `completed=false`; it does not copy captured output and cancels its timer when completion wins.                                                                                     |
| `write`                    | Serialized stdin write. Optional stdin close occurs inside the same serialized operation after its payload, so a later writer cannot race ahead.                                                                                                                                             |
| `wait`                     | Join until process exit **and** both stream drains; returns captured data while the handle is open. Does **not** release buffers itself.                                                                                                                                                     |
| `async_close` / JS `close` | SIGKILL process group, cancel pipes, await real reap + drain barrier, then release retained buffers (invalidates native capture). Idempotent; concurrent callers join. An outstanding concurrent `wait` may fail after close. close-before-start → Closed immediately. Never stops executor. |
| `dispose` / `~Process`     | Non-blocking finalizer: post group SIGKILL + cancel onto owner executor. Does not forge exit/eof flags. State self-retains until completions.                                                                                                                                                |
| `terminate`                | Signal group; resolves after the request (not after exit). Unknown signal names rejected synchronously at the binding.                                                                                                                                                                       |
| `run`                      | create → start (stdin ignore by default) → wait → async_close                                                                                                                                                                                                                                |

The concurrent-process slot is released after real reap and both pipe drains.
Captured buffers remain charged until explicit close or product-session eviction,
so a finished readable session does not count as a running child but still obeys
the global memory budget.

## Product protocol sessions

`script/src/runtime/process-sessions.ts` is owned by the JavaScript
`Application`. It maps shared MCP `process_id` values to JS-owned `Process`
instances, implements command yield, cursor encoding, terminal retention and
shutdown close. This is product/protocol ownership, not a Host/Runtime or C++
resource registry. Unsupported environment injection and PTY requests are
rejected at the HTTP Wide boundary rather than ignored.

## Limits

On over budget:

- **reject** `start()` if at max concurrent processes
- **truncate** buffer growth when global/per-process max hit (0 cap = retain nothing)

Never delete another live Process’s state.

## Runtime cancellation

`Runtime::Async` injects `std::stop_token`. For `start` / `write` / `read` /
`waitForExit` / `wait` / `run`,
a stop bridge posts `Process::dispose()` onto the owner executor (no domain
mutation on the stop callback thread). On stop, wait/write unblock and the
binding returns `runtime::Error::cancelled_op` rather than a normal result.
Explicit `close` ignores the injected stop token (same principle as `Server.close`).
