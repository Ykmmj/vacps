# vacps-native agent contract

## Scope

This file applies **recursively** to all work under `apps/vacps-native/` (source, bindings, scripts, tests, docker/CMake, and docs).

Technical rules are normative in [`docs/CODING_STANDARDS.md`](docs/CODING_STANDARDS.md). This file defines **agent workflow** only and does **not** weaken those standards.

## Before any edit

1. Read [`docs/CODING_STANDARDS.md`](docs/CODING_STANDARDS.md) **completely**.
2. Read the architecture docs it links that are relevant to the task:
   - [`docs/RUNTIME_LAYERING.md`](docs/RUNTIME_LAYERING.md)
   - [`docs/NATIVE_MODULES.md`](docs/NATIVE_MODULES.md)
   - [`docs/NATIVE_RESOURCE_OWNERSHIP.md`](docs/NATIVE_RESOURCE_OWNERSHIP.md)
   - [`docs/PROCESS_RUNTIME.md`](docs/PROCESS_RUNTIME.md)
3. Follow both this file and the coding standards.

## Roles

| Role | Responsibility |
| --- | --- |
| **Codex** | Owns planning, architecture, implementation, and review for core framework work |
| **Pi / Grok** | Implements only explicitly delegated, bounded non-core work from a detailed Codex specification |

Core framework work includes Runtime, QuickJS integration, Binding DSL foundations,
allocator/engine ownership, concurrency, asynchronous control flow, shutdown/lifetime
semantics, and build-system changes that affect those areas. Codex **MUST** implement
these changes directly rather than delegating them to Pi/Grok.

Pi/Grok delegation is optional, not the default implementation path. It is appropriate
for isolated mechanical edits, leaf modules, documentation synchronization, or other
tasks whose architecture and contracts have already been fixed. Codex remains
responsible for reviewing every delegated diff before verification.

## Mandatory gate when Pi / Grok is used

Every Pi/Grok coding or edit prompt **MUST** explicitly contain all of the following. **If any item is omitted, stop before editing**, restate the gate, and apply it.

```text
VACPS-NATIVE AGENT GATE (mandatory):
1. Before any edit, read apps/vacps-native/docs/CODING_STANDARDS.md completely
   and the architecture docs it links that are relevant to this task
   (RUNTIME_LAYERING.md / NATIVE_MODULES.md / NATIVE_RESOURCE_OWNERSHIP.md /
   PROCESS_RUNTIME.md as applicable). Follow AGENTS.md under apps/vacps-native/.
2. Interact in English. Code comments and identifiers remain English.
3. Model: grok-4.5. Thinking: high.
   (Prompt must include: --model grok-4.5 --thinking high)
4. Preserve all unrelated dirty worktree changes; do not revert or clean
   files outside this task.
5. Compile and test only via apps/vacps-native/docker/build.sh with at most
   4 cores (CMAKE_BUILD_PARALLEL_LEVEL=4 or less). Never host-local CMake/Ninja.
6. Do not git commit unless the user explicitly requests it.
7. End with an explicit verification report: commands run, results, and
   checks skipped (and why).
8. For performance work, freeze the public API, observable semantics, workload,
   data scale/distribution, and build/run configuration before editing. Do not
   count a new batch API, reduced output, moved work, relaxed guarantees, or a
   different workload as an optimization of the original hot path. Report such
   changes separately as new capability or workload results.
```

### Gate rules

- Interact and report in **English**; code identifiers/comments/examples stay English.
- Use **`--model grok-4.5`** and **`--thinking high`**.
- **Preserve** unrelated dirty worktree changes; never revert or “clean up” outside the task.
- **Build/test only** through `apps/vacps-native/docker/build.sh` at **≤ 4 cores**.
- **No `git commit`** unless the user explicitly requests it.
- End with a **verification report**: commands run, results, and skipped checks (with reasons).

## Implementation constraints

- **No compatibility scaffolding** unless there is a **released** compatibility contract.
- Do not add dual-track APIs, useless aliases, or speculative Node/N-API shims.
- Place files in the owning layer; follow Wide/Narrow API contracts and failure taxonomy in the coding standards.
- Synchronous binding callbacks run directly in the owner-thread QuickJS turn; do not add per-callback Runtime gates.
- Performance claims **MUST** compare the same API and observable semantics under
  the same workload and configuration. New batching/coalescing APIs are separate
  features, not evidence that the original path became faster.
- Optimize the measured hot path itself. Internal scoped reuse (for example,
  operation-local QuickJS atoms) is valid only when lifetime, output, ordering,
  error, transaction, cancellation, and concurrency semantics remain unchanged.
- Protect the dirty worktree: edit only task-scoped paths.

## Verification (proportionate)

| Change risk | Expectation |
| --- | --- |
| Docs-only | No build required; still run doc consistency checks and report skips |
| Logic / API | Build and run relevant JavaScript through the product binary |
| Full product confidence | `docker/build.sh release` |
| Memory / lifetime | `docker/build.sh asan` plus relevant JavaScript when applicable |
| Concurrency | `docker/build.sh tsan` plus relevant JavaScript when applicable |

Hard rules:

- **Max 4 cores** for all `docker/build.sh` invocations.
- **`--native-only` is not full validation**; never report it as complete product verification.
- Host-local CMake/Ninja/ad hoc compile is **not** acceptable formal validation.
- ASan/TSan/release requirements scale with risk; absence of a run must be explained in the verification report.

## Pre-submit (agents)

Use the concise checklist in `docs/CODING_STANDARDS.md` §11.2, including: contract chosen/documented; boundary validation once; no recovery for programmer misuse; ownership/thread/lifetime; no compatibility scaffolding; performance comparisons preserve API/semantics/workload; correct layer/file/name; tests; Context7 for dependency APIs; docker-only build ≤4 cores; verification report; no unrelated edits.
