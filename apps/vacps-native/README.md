# vacps-native

Native VACPS Agent (C++23), **Linux x86_64 musl static ELF**.

Design: `temp/native.md` (repo-local). Sibling of Node agent `apps/vacps`.  
**开发规范：** [`docs/CODING_STANDARDS.md`](docs/CODING_STANDARDS.md)（`std::expected`、标准库优先、nlohmann/json、避免多余拷贝等）。

## Build (required: Docker)

```bash
# from monorepo root (forwards host http_proxy/https_proxy into docker run)
bash apps/vacps-native/docker/build.sh                 # reuse image, Release
bash apps/vacps-native/docker/build.sh debug           # Debug
bash apps/vacps-native/docker/build.sh release --test  # script build + agent + ctest
bash apps/vacps-native/docker/build.sh --rebuild-image # Dockerfile 变更后才需要
```

业务脚本（TypeScript 6.0.3 + esbuild 0.28.1）：

```bash
cd apps/vacps-native/script && npm ci && npm run build   # → dist/vacps.mjs
cd apps/vacps-native/script && npm test                  # vitest 单元测试（纯逻辑）
# 二进制默认加载 script/dist/vacps.mjs，或 --script / VACPS_SCRIPT
```

单元测试：`tests/`，GoogleTest **1.17.0**。容器内：

```bash
ctest --preset release --output-on-failure
```

默认**不**重建编译镜像；本地已有 `vacps-native-build:alpine-3.24.1-clang22` 时直接 `docker run` 编代码。

**Proxy:** `docker/build.sh` 会把宿主的 `http_proxy` / `https_proxy` / `no_proxy`（及大写形式）传给 `docker build --build-arg` 和 `docker run -e`。  
镜像 `Dockerfile` 用 `ARG`/`ENV` 接收，供 `apk`、以及容器内 `curl`/`git` 拉依赖使用。

手动 build 示例：

```bash
docker build \
  --build-arg HTTP_PROXY="$http_proxy" \
  --build-arg HTTPS_PROXY="$https_proxy" \
  --build-arg NO_PROXY="$no_proxy" \
  --build-arg http_proxy="$http_proxy" \
  --build-arg https_proxy="$https_proxy" \
  --build-arg no_proxy="$no_proxy" \
  -t vacps-native-build:alpine-3.24.1-clang22 \
  -f apps/vacps-native/Dockerfile apps/vacps-native
```

Image base: `alpine:3.24.1` with Clang 22 + lld 22 (see `Dockerfile`).

Artifact:

```text
apps/vacps-native/build/release/vacps-agent-linux-x86_64
```

### GitHub Release

CI workflow: [`.github/workflows/release-vacps-native.yml`](../../.github/workflows/release-vacps-native.yml)

```bash
# cut a release from main (or any commit)
git tag vacps-native-v0.1.0
git push origin vacps-native-v0.1.0
# or: Actions → “Release vacps-native” → Run workflow
```

Release assets: static `vacps-agent-linux-x86_64`, `vacps.mjs`, tarball + `SHA256SUMS`.

Smoke:

```bash
./apps/vacps-native/build/release/vacps-agent-linux-x86_64 --version
file ./apps/vacps-native/build/release/vacps-agent-linux-x86_64
```

## Status

| Piece                                            | Status                                                                                                                                         |
| ------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| Docker toolchain (+ proxy)                       | done                                                                                                                                           |
| CMake presets                                    | done                                                                                                                                           |
| Boost 1.91.0 (FetchContent)                      | Asio/Beast 传输层（无 C++ 产品路由）                                                                                                           |
| SQLite 3.53.4 amalgamation                       | connection + PRAGMA only（业务表/SQL 归 JS `vacps:store`）                                                                                     |
| spdlog 1.17.0                                    | stderr logger; `VACPS_LOG_LEVEL` / `--log-level`                                                                                               |
| nlohmann/json 3.12.0                             | HTTP JSON bodies                                                                                                                               |
| QuickJS RAII + Asio Promise 桥                   | `ScriptRuntime`：`await_settled` + `notify_progress`                                                                                           |
| Native modules                                   | `log` / `store` / `host` / `fs` / `process` / `crypto`（见 `docs/NATIVE_MODULES.md`）                                                          |
| 业务 script（ESM）                               | 全部路由含 `/health` `/ready`；`--script`                                                                                                      |
| Listen `127.0.0.1:8788`                          | done                                                                                                                                           |
| OpenSSL（apk static）                            | Ed25519 / RAND / SHA-256                                                                                                                       |
| vacps:http outbound `request`                    | done（HTTP/HTTPS；CA fail-closed）                                                                                                             |
| Control-plane registration/telemetry             | JS via `@vacps/contracts` + signed `http.request`；timer `tickControlPlane`                                                                    |
| Tasks inbox + pump                               | POST/GET/cancel/retry/logs；`command`/`shell` via `process.run`；contracts `taskDispatchSchema`                                                |
| Process group                                    | `setpgid` + timeout/terminate `kill(-pgid)`（shell 子树）                                                                                      |
| `vacps:process`                                  | `run` + `start`/`read`/`write`/`terminate`（`process::Registry` on ScriptServices）                                                            |
| `/exec/command` `/exec/shell`                    | JS `ProcessManager`（fire-and-wait）                                                                                                           |
| `/process/start_*` `/read` `/write` `/terminate` | 长驻进程 API                                                                                                                                   |
| Task mid-cancel                                  | `POST /tasks/:id/cancel` → `process.terminate` 杀进程组                                                                                        |
| `/fs/*`                                          | read/stat/list/write/mkdir/delete/move + glob/grep/edit/apply_patch                                                                            |
| `/capabilities`                                  | 工具探测（`pi: false`，不接 Pi）                                                                                                               |
| `/metrics` + health shape                        | `/proc` 遥测；`backendHealthSchema` 对齐                                                                                                       |
| `/schedulers/*`                                  | SQLite；**绝对 `next_run_at` + `revision` CAS claim**；无 Redis                                                                                |
| Env                                              | `CONTROL_PLANE_URL` / `PUBLIC_BASE_URL` / `BACKEND_ID` / `AGENT_*_KEY` / `REGISTRATION_TOKEN` / `CONTROL_PLANE_PUBLIC_KEY` / `VACPS_CA_BUNDLE` |

### Product stance

**vacps-native 直接替代 Node `apps/vacps`**（不做双端口影子部署）。  
**不接 Pi**（协议里若收到 `kind=agent` 任务直接 409）。

**调度状态机（绝对时刻）**

```text
规则层：cron + timezone + revision
        ↓ 控制面 IANA 投影
执行层：next_run_at（canonical UTC ISO）
        ↓ CAS claim (revision, raw next_run_at) + 同事务 occurrence task
节点：now_ms >= next → enqueue → advance from scheduled_for
```

- wire 带 `revision` / `policy` / `next_run_at`；同 rev 游标只向前合并；更高 rev 可合法回拨
- occurrence：`schedule_id:revision:scheduled_for_ms` 作 `task_id`（确定性幂等）
- misfire：`run_once` | `skip` | `catch_up`（有 `max_catchup_runs` 硬上限）
- `last_fired_minute` 仅观测；一致性靠 CAS + 事务
- claim 后 agent 签名 `POST /api/schedules/:id/occurrences/ack`；**CP 用本侧 Intl 重算权威 next**（`locally_advanced_to` 只诊断）
- 绝对时间是执行协议；IANA 投影在 CP（native 本地 advance = 离线连续，非最终权威）
- 语义冻结见 `@vacps/contracts` `schedule-semantics`：misfire / DST gap=skip / overlap=双 UTC 都可触发 / revision merge

### Remaining polish (vs Node)

| Item                     | Notes                                                                          |
| ------------------------ | ------------------------------------------------------------------------------ |
| Pi                       | 不做                                                                           |
| 控制面 schedule fixtures | `apps/control-worker/tests/fixtures/schedule-control-plane.ts` + ack/e2e tests |

Smoke after build:

```bash
VACPS_LISTEN_PORT=8788 ./apps/vacps-native/build/release/vacps-agent-linux-x86_64 \
  --data-dir /tmp/vacps-data \
  --script apps/vacps-native/script/dist/vacps.mjs &
curl -sS http://127.0.0.1:8788/health
curl -sS http://127.0.0.1:8788/ready
curl -sS http://127.0.0.1:8788/script/ping
```

## Layout

```text
apps/vacps-native/
├── Dockerfile / docker/build.sh
├── CMakeLists.txt / CMakePresets.json
├── docs/
├── src/
│   ├── main.cpp
│   ├── app/          # config, log, version, error
│   ├── quickjs/      # JS runtime RAII + vacps:* modules
│   ├── process/
│   ├── storage/      # SQLite
│   ├── http/
│   ├── crypto/
│   └── fs/           # pure file I/O (policy in JS path-guard)
├── tests/            # gtest
└── README.md
```
