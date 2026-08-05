# vacps-native

Native VACPS Agent (C++23), **Linux x86_64 musl static ELF**.

Design: `temp/native.md` (repo-local). Sibling of Node agent `apps/vacps`.
**开发规范：** [`docs/CODING_STANDARDS.md`](docs/CODING_STANDARDS.md)
**Runtime 分层：** [`docs/RUNTIME_LAYERING.md`](docs/RUNTIME_LAYERING.md)
**JS 模块 surface：** [`docs/NATIVE_MODULES.md`](docs/NATIVE_MODULES.md)
**资源所有权：** [`docs/NATIVE_RESOURCE_OWNERSHIP.md`](docs/NATIVE_RESOURCE_OWNERSHIP.md)

## Build (required: Docker)

```bash
# from monorepo root (forwards host http_proxy/https_proxy into docker run)
bash apps/vacps-native/docker/build.sh                 # reuse image, Release
bash apps/vacps-native/docker/build.sh debug           # Debug
bash apps/vacps-native/docker/build.sh release         # script build/test + agent + JS smoke
bash apps/vacps-native/docker/build.sh --rebuild-image # Dockerfile 变更后才需要
bash apps/vacps-native/docker/build.sh release --native-only  # C++ only (no script bundle)

# Sanitizer modes (dedicated dirs, dynamic Debug; one-shot container uses
# --security-opt seccomp=unconfined so io_uring / TSan can execute):
CMAKE_BUILD_PARALLEL_LEVEL=4 bash apps/vacps-native/docker/build.sh asan
CMAKE_BUILD_PARALLEL_LEVEL=4 bash apps/vacps-native/docker/build.sh tsan
```

业务脚本（TypeScript + esbuild）：

```bash
cd apps/vacps-native/script && npm ci && npm run build   # → dist/vacps.mjs
cd apps/vacps-native/script && npm test                  # vitest 单元测试（纯逻辑）
# 二进制默认加载 script/dist/vacps.mjs，或 --script PATH
```

Native 集成行为通过编译后的产品二进制直接运行 JavaScript 验证，不维护独立 C++ 单元测试目标。

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

## Architecture (current)

| Piece | Status |
| --- | --- |
| Docker toolchain (+ proxy) | done |
| CMake presets | done |
| Boost 1.91.0 | Asio / Beast / Process v2 |
| OpenSSL (apk static) | Ed25519 / RAND / SHA-256 / TLS |
| SQLite 3.53.4 amalgamation | C++ 域库（`src/storage`）+ **`vacps:store` 模块** |
| spdlog 1.17.0 | stderr；CLI `--log-level` |
| nlohmann/json 3.12.0 | JSON |
| QuickJS 2026-06-04 | 经 `Runtime` / `JsEngine`；owner 线程 only；专属 mimalloc heap |
| mimalloc 3.4.4 | QuickJS 专属 heap + 全局 C++ `new/delete`（TSan 除外）；不覆盖 C `malloc/free` |
| Runtime | phase 机、job pump、`await_value`、shutdown；内部 Runtime::Impl |
| Runtime::Async / Callbacks / Script | JS→native Promise + `run_blocking`；native event→JS callback→await sync/thenable；host 模块求值。同步 binding 在 owner-thread QuickJS turn 内直接执行 |
| Binding DSL + `qjs::OwnedValue` | `create_function` / `create_async_function` / `ClassBuilder` (`async_method` / `static_async_function` / `ClassJsEdges`) |
| ModuleCatalog | **immovable**；注册 `vacps:crypto` / `vacps:host` / `vacps:log` / **`vacps:store`** / **`vacps:fs`** / **`vacps:http`** |
| Globals | `URL` / `URLSearchParams` / `TextEncoder` / `TextDecoder` |
| Application + EntryModule | 组合根；信号；入口 ESM `initialize`/`shutdown` |
| 业务 script（ESM） | CLI `--script`（或默认路径候选）；产品路由在 JS |

**`vacps:store`：** 仅导出 class `Store`（`Store.open`、只读 `path`/`closed`，async `exec`/`run`/`query`/`transaction`/`close`）。固定 `query(sql, params?, options?)`；`transaction` 在单个 worker 作业内复用重复 SQL 的 prepared VM。所有权：`ClassHolder` + `shared_ptr`；显式 `close` awaitable；finalizer 只丢 holder。详见 [`docs/NATIVE_MODULES.md`](docs/NATIVE_MODULES.md) 与 [`docs/NATIVE_RESOURCE_OWNERSHIP.md`](docs/NATIVE_RESOURCE_OWNERSHIP.md)。

**`vacps:fs`：** class `File` + 命名空间路径操作（async）。详见 [`docs/NATIVE_MODULES.md`](docs/NATIVE_MODULES.md)。

**`vacps:http`：** 导出 outbound `request` + inbound class `Server(options, onRequest)`（QuickJS-native binding DSL，非 N-API/Node addon）。`request`：options 同步解码后 `Runtime::Async`/Asio；binary body/`ArrayBuffer` 响应；单一 `timeoutMs`；stop 取消；`maxResponseBytes`；TLS 校验 + host composition CA；无 JS `caBundle`；不跟随重定向。`Server`：显式 greenfield class；`listen()` → `ListenAddress`；`close()`；只读 `listening`/`address`；binary request body；`onRequest` 可同步或返回 Promise；经 `Runtime::Callbacks` 反向 await。纯 transport（无 product routes/JSON）。详见 [`docs/NATIVE_MODULES.md`](docs/NATIVE_MODULES.md)。

**已编译：** `vacps:crypto` / `vacps:host` / `vacps:log` / `vacps:store` / `vacps:fs` / `vacps:http`（`request` + `Server`）/ `vacps:process`（`Process` + `run`）。

### Product stance

**vacps-native 直接替代 Node `apps/vacps`**（不做双端口影子部署）。
**不接 Pi**（协议里若收到 `kind=agent` 任务直接 409）。

**调度状态机（绝对时刻）** — 产品语义（script / contracts）：

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
- 语义冻结见 `@vacps/contracts` `schedule-semantics`

### CLI（C++ 进程旋钮）

C++ 启动配置**仅**接受命令行（`host::parse_command_line` → `Application::Options`）。不读 `VACPS_DATA_DIR` / `VACPS_LOG_LEVEL` / `VACPS_SCRIPT` / `VACPS_CA_BUNDLE` / `VACPS_JS_*`。

| 项 | 说明 |
| --- | --- |
| `--help` / `-h` | 打印用法并退出（须单独使用） |
| `--version` / `-V` | 打印版本并退出（须单独使用） |
| `--script PATH` | 入口 ESM；省略时尝试 `script/dist/vacps.mjs` 等默认候选 |
| `--data-dir DIR` | Host → `vacps:host` `dataDir()`（默认 `data`） |
| `--log-level LEVEL` | spdlog canonical：trace, debug, info, warn, error, critical, off（默认 info） |
| `--ca-bundle PATH` | module composition CA（`vacps:http` TLS；非 JS 选项） |
| `--js-heap-limit-bytes N` | QuickJS heap（`N > 0`） |
| `--js-stack-limit-bytes N` | QuickJS stack（`N > 0`） |
| `--js-time-budget-ms N` | JS time budget（`N >= 0`；`0` 关闭 watchdog） |
| `--lifecycle-timeout-ms N` | 入口 initialize/shutdown 超时（`N > 0`；默认 30000） |
| 产品策略 env | **仍**由 script 经 `host.getenv` 自读（listen、auth、控制面密钥、`PATH`/`HOME` 等） |

Smoke after build（需业务 bundle 与其依赖的模块 surface 齐全时）：

```bash
VACPS_LISTEN_PORT=8788 ./apps/vacps-native/build/release/vacps-agent-linux-x86_64 \
  --data-dir /tmp/vacps-data \
  --script apps/vacps-native/script/dist/vacps.mjs &
curl -sS http://127.0.0.1:8788/health
```

## Layout

```text
apps/vacps-native/
├── Dockerfile / docker/build.sh
├── CMakeLists.txt / CMakePresets.json
├── docs/
├── src/
│   ├── main.cpp
│   ├── app/          # log, version, error, platform
│   ├── bootstrap/    # process_init only
│   ├── host/         # command_line + Application::Options + EntryModule
│   ├── qjs/          # OwnedValue, ScopedCString
│   ├── binding/      # native binding DSL（ClassHolder / ClassBuilder / ClassJsEdges）
│   ├── runtime/      # vacps::Runtime + Async/Callbacks/Script；Runtime::Impl
│   ├── globals/      # URL / TextEncoder / TextDecoder
│   ├── modules/      # catalog + crypto/host/log/store/fs/http/process
│   ├── process/      # 子进程域库（经 vacps:process 导出）
│   ├── storage/      # SQLite 域库（Store；经 vacps:store 导出）
│   ├── http/         # HTTP 域库（outbound client + inbound server transport；经 vacps:http 导出）
│   ├── crypto/
│   └── fs/           # 文件 I/O 域库（经 vacps:fs 导出）
├── script/           # TypeScript 业务入口
└── README.md
```
