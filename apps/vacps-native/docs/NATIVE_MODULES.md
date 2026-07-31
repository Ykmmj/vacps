# vacps-native JS 原生模块接口

C++ 只提供**能力/工厂**（类型级 API），**不**预创建业务实例；实例由 JavaScript 创建并持有。  
业务路由（含 `/health`、`/ready`）全部在 script；C++ HTTP 仅传输。  
模块名固定：`import * as x from "vacps:…"`.

## Global: `URL`（Ada 4.x）

ScriptRuntime 启动时安装 `globalThis.URL`（[Ada](https://github.com/ada-url/ada) v4 WHATWG 解析）。  
供 Zod `z.url()` 等依赖浏览器/Node URL API 的代码使用。

| API                                                                                                | 说明                            |
| -------------------------------------------------------------------------------------------------- | ------------------------------- |
| `new URL(input, [base])`                                                                           | 非法 → `TypeError: Invalid URL` |
| `url.href` / `protocol` / `hostname` / `host` / `pathname` / `search` / `hash` / `port` / `origin` | getters                         |
| `URL.canParse(input, [base])`                                                                      | static boolean                  |
| `toString()` / `toJSON()`                                                                          | → href                          |

## `vacps:log`

| API                                        | 说明        |
| ------------------------------------------ | ----------- |
| `trace/debug/info/warn/error(msg: string)` | 写入 spdlog |
| `flush()`                                  | 刷新        |

## `vacps:store`

**create-at-JS-call**：C++ 不预开库；JS 调用 `Store.open(path, options?)` 创建实例并持有。  
无 free `open()`；无 `begin`/`commit`/`rollback`（多步原子用 `transaction`）。

```ts
import { Store } from 'vacps:store';
import * as host from 'vacps:host';
const db = await Store.open(`${host.dataDir()}/agent.db`);
await db.exec('...');
await db.close();
```

| API | 说明 |
| --- | --- |
| `Store.open(path, options?)` | 异步创建连接（`mode?`） |
| `Store.exec` / `run` / `query` | SQL（Promise） |
| `Store.transaction(steps)` | 多步原子（BEGIN IMMEDIATE + steps + COMMIT） |
| `Store.path` / `Store.closed` / `Store.close()` | 元数据 / 释放 |

## `vacps:host`（进程信息，极薄）

**不是** HTTP / SQL / process / fs 的入口。无 `sleep`。

| API                             | 说明                            |
| ------------------------------- | ------------------------------- |
| `version()`                     | agent 版本                      |
| `dataDir()`                     | `VACPS_DATA_DIR`                |
| `listenHost()` / `listenPort()` | 配置默认监听地址（env/CLI）     |
| `nowMs()`                       | Unix 毫秒                       |
| `platform()`                    | `linux-x86_64-musl`             |
| `getenv(name)`                  | 进程环境变量；未设置返回 `null` |

C++ `ScriptRuntime`：QuickJS engine、脚本加载、`invoke_export`、Promise↔Asio 唤醒。  
Context opaque = `ScriptRuntime*`；共享服务经 `ScriptServices`（`services()`），不是 service locator。

## Event loop & Promise bridge（架构定稿）

`io_context::run()` 是唯一进程级事件循环；QuickJS 只提供 Promise + job queue。  
`await_settled()` 是 “跑到某个 Promise settle” 的协程泵（≈ `js_std_await` 的 Asio 版）。  
所有 native 异步 API **必须** 经 `spawn_js_promise`（`promise_bridge.hpp`），禁止手抄 `JS_NewPromiseCapability + co_spawn`。

### 五条规则

1. `io_context::run()` 是唯一事件循环；禁止嵌套 `run_one()` / `poll()` / QuickJS `js_std_loop()`。
2. `await_settled()` 只做状态检查、**预算内** job drain、`co_await wait_progress()`；绝不忙等。
3. 每个 native Promise：全部终止路径 settle attempt 一次（settle-once），随后 **无条件** `notify_progress`（scope guard）。
4. 所有 `JS_*`、`JSValue` 创建/释放与 Promise settlement 只在 JS executor（当前 = 唯一 `ioc` 线程）。
5. 导出函数返回的 Promise 必须覆盖它启动的全部异步工作；开放后台 timer / 事件监听 / fire-and-forget 前，必须先引入全局 job pump。

### 分工

| 层           | 手段                                                             |
| ------------ | ---------------------------------------------------------------- |
| 业务错误     | `Result` / `std::expected`（尽量不 throw）                       |
| 协程故障屏障 | `catch (std::exception)` / `catch (...)` → reject（若未 settle） |
| 生命周期     | `spawn_js_promise`：settle-once + NotifyGuard                    |

`progress_generation` + timer cancel 实现 `wait_progress` / `notify_progress` 事件版本协议。

## `vacps:http`（入站 Server + 出站 request）

### 入站 Server（`new Server`，JS 持有）

```ts
import * as http from 'vacps:http';
const server = new http.Server({ host: '127.0.0.1', port: 8788 });
await server.listen();
// server.listening === true
await server.close();
```

| API                                    | 说明                                          |
| -------------------------------------- | --------------------------------------------- |
| `new Server(options)`                  | `options: { host?, port }` — `port` required  |
| `Server.listening`                     | `readonly boolean` while acceptor is open     |
| `Server.listen()` / `close()`          | Promise lifecycle; bind only in `listen()`    |

Session → `IRequestHandler` / `ScriptRequestHandler` → `ScriptRuntime::invoke_export("handleRequest")`。

### 出站 `request`（HTTP/HTTPS Promise）

```ts
const res = await http.request({
  method: 'POST',
  url: 'https://control.example/v1/register',
  headers: { 'content-type': 'application/json' },
  body: JSON.stringify(payload),
  timeoutMs: 15_000,
});
// res.status, res.headers, res.body (ArrayBuffer)
```

| 行为      | 说明                                                                                      |
| --------- | ----------------------------------------------------------------------------------------- |
| HTTPS     | `verify_peer` + SNI + `host_name_verification`；TLS ≥ 1.2                                 |
| CA        | `Config.ca_bundle` / `VACPS_CA_BUNDLE` → 平台默认路径；缺失 **fail-closed**（不跳过校验） |
| 重定向    | **不**自动跟随（3xx 原样返回）                                                            |
| 超时      | Beast `expires_after`（默认 30s）                                                         |
| body 上限 | `maxResponseBytes`（默认 8 MiB）                                                          |

## 控制面（业务 script，非 C++ 路由）

依赖 monorepo **`@vacps/contracts`**（esbuild 打进 `vacps.mjs`）：

- `registerBackendSchema` → `POST {CONTROL_PLANE_URL}/api/registrations`
- `backendTelemetrySchema` → `POST …/api/telemetry`
- 签名：`x-vacps-id|timestamp|nonce|signature`（与 Node Agent 同一 canonical 串；私钥 PKCS#8 或 raw seed base64url）
- 状态：`agent.db` 表 `agent_state`
- C++ 每 15s `invoke_export("tickControlPlane")`：registration / telemetry + **task pump**

### 任务 API（业务 script）

| 方法 | 路径                | 说明                                                       |
| ---- | ------------------- | ---------------------------------------------------------- |
| POST | `/tasks`            | `taskDispatchSchema`（`@vacps/contracts`）；可选控制面签名 |
| GET  | `/tasks/:id`        | 状态 / result / error                                      |
| GET  | `/tasks/:id/logs`   | `?offset=&stream=`                                         |
| POST | `/tasks/:id/cancel` | 队列中取消 / 运行中打标                                    |

- 表：`tasks` / `task_logs` / `request_nonces` / `task_idempotency`（`schema.ts` 有序建表）
- 执行：`command` / `shell` → `process.run`；`agent` → 409 capability_unavailable
- 并发：pump 单任务（`claimNextQueued`）
- **崩溃恢复**：启动时 `running`/`starting` → `failed` + `agent_restarted`（不自动重跑）
- **retry**：`POST /tasks/:id/retry` → 新 `task_id` 入队（仅终态）
- **idempotency_key**：重复 POST 返回原任务（`deduped: true`）

## `vacps:fs`

**纯 I/O**：C++ 不做产品路径策略（无 PathSandbox）。相对路径仅拼到 `dataDir` 下；绝对路径原样使用。  
MCP / 文件工具的黑名单与 workspace 约束在 JS `runtime/path-guard.ts`（与 Node agent 一致）。  
宿主内部（如 telemetry 读 `/proc`）可直接走 C++ path helpers，不经 path-guard。

### 主 API：`File`（handle）

| 项 | 说明 |
| -- | --- |
| 打开 | `File.open(path, flags, mode?)` 或 `File.open(path, { flags, mode? })` |
| flags | Node/POSIX 风格整数位掩码（`O_RDONLY` / `O_WRONLY` / `O_RDWR` / `O_CREAT` / …）；**无**字符串 OpenMode |
| 后端 | **双后端（必保留）**：`probe_io_uring()` 成功 → Asio `random_access_file`；否则 `thread_pool` + 私有 FD。不是临时兼容层，不可合并为“仅 POSIX” |
| create `mode` | pool：`open(2)` 使用 JS/`OpenOptions.mode`；Asio：`random_access_file::open` 无 mode 参数（Boost 限制） |
| I/O | `read` / `readAt` / `write` / `writeAt` / `readText` / `writeText` / `truncate` / `stat` / `flush` / `close`（全部 `Promise`） |

### 命名空间 ops（path helpers）

| API | 实现 |
| --- | --- |
| `mkdir` / `exists` / `remove` / `rename` / `readDirectory` / `stat` | 始终 `asio::thread_pool` 卸载同步 path helpers |

内容 I/O 仅通过 `File`（无 free path-level content helpers）。

ScriptServices 持有 2 线程 `thread_pool`（`ScriptRuntime::services().pool`）。启动时 `probe_io_uring()`（setup + NOP submit + wait），**不是**只测 compile-time 宏。`ScriptRuntime::use_asio_file()` 把探测结果传给 `File::async_open`。

**io_uring / Docker 注意**（Asio 无自动降级）：

| 环境 | 典型表现 |
| ---- | -------- |
| Docker 默认 seccomp（拦 setup/register/enter） | `io_uring_queue_init` → EPERM；Asio 构造 `random_access_file` 会 **抛** `system_error`。probe 失败 → 永不建 Asio file，走 thread_pool |
| 不完整 seccomp（允许 setup，拦 enter） | submit 失败或 Asio 不传播错误 → 操作可能 **永久 pending**。probe 含 submit/wait，失败则不用 Asio file |
| 应用吞掉构造异常且不 `reject` Promise | 也会「永久 pending」——`spawn_js_promise` 已 try/catch + 必 `notify_progress` |

探测失败时日志：`io_uring probe: … — thread_pool fallback`。


## `vacps:process`

| API                                       | 说明                         |
| ----------------------------------------- | ---------------------------- |
| `run(argv, options?): Promise<RunResult>` | Boost.Process v2 + Asio 协程 |

## `vacps:crypto`（OpenSSL）

| API                                   | 说明                                                                                  |
| ------------------------------------- | ------------------------------------------------------------------------------------- |
| `randomBytes(n)`                      | `RAND_bytes`                                                                          |
| `sha256` / `sha256Hex`                | EVP SHA-256                                                                           |
| `toHex` / `fromHex`                   | 十六进制编解码                                                                        |
| `base64Encode` / `base64Decode`       | OpenSSL `EVP_EncodeBlock` / `EVP_DecodeBlock`（标准 Base64，带 `=` padding）          |
| `base64UrlEncode` / `base64UrlDecode` | Base64url（`-_`，encode 无 padding）                                                  |
| `ed25519SeedFromPrivateKey(b64url)`   | raw 32B seed 或 PKCS#8/DER → 32B seed（`d2i_AutoPrivateKey` + `get_raw_private_key`） |
| `ed25519PublicFromPrivate(priv32)`    | seed → 32 字节公钥                                                                    |
| `ed25519Sign(priv32, msg)`            | 原始 32 字节 seed → 64 字节签名                                                       |
| `ed25519Verify(pub32, msg, sig64)`    | 验签                                                                                  |
