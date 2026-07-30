# vacps-native JS 原生模块接口

C++ 只提供**能力/工厂**（类型级 API），**不**预创建业务实例；实例由 JavaScript 创建并持有。  
业务路由（含 `/health`、`/ready`）全部在 script；C++ HTTP 仅传输。  
模块名固定：`import * as x from "vacps:…"`.

## `vacps:log`

| API | 说明 |
|-----|------|
| `trace/debug/info/warn/error(msg: string)` | 写入 spdlog |
| `flush()` | 刷新 |

## `vacps:store`

**工厂模式**：C++ 不预开库；JS 调用 `open(path)` 创建实例并持有。

```ts
import * as store from "vacps:store";
const db = store.open(`${host.dataDir()}/agent.db`);
db.exec("...");
db.close();
```

| API | 说明 |
|-----|------|
| `open(path): Store` | 创建连接（能力） |
| `Store.exec/run/query` | SQL |
| `Store.begin/commit/rollback` | 事务 |
| `Store.path()` / `Store.close()` | 元数据 / 释放 |

## `vacps:host`（进程信息，极薄）

**不是** HTTP / SQL / process / fs 的入口。无 `sleep`。

| API | 说明 |
|-----|------|
| `version()` | agent 版本 |
| `dataDir()` | `VACPS_DATA_DIR` |
| `listenHost()` / `listenPort()` | 配置默认监听地址（env/CLI） |
| `nowMs()` | Unix 毫秒 |
| `platform()` | `linux-x86_64-musl` |
| `getenv(name)` | 进程环境变量；未设置返回 `null` |

C++ `Host` 类本身：QuickJS runtime、脚本加载、`invoke_export`、Promise↔Asio 唤醒。  
Context opaque = `Host*`（已删除 `HostState`）。

## `vacps:http`（入站 Server + 出站 request）

### 入站 Server（工厂，JS 持有）

```ts
import * as http from "vacps:http";
const server = http.createServer();
server.listen();
server.close();
```

| API | 说明 |
|-----|------|
| `createServer(options?)` | 工厂；`options?: { host?, port? }` |
| `Server.listen()` / `close()` / `isListening()` | 传输层生命周期 |

Session → `http::dispatch_to_script` → `Host::invoke_export("handleRequest")`。

### 出站 `request`（HTTP/HTTPS Promise）

```ts
const res = await http.request({
  method: "POST",
  url: "https://control.example/v1/register",
  headers: { "content-type": "application/json" },
  body: JSON.stringify(payload),
  timeoutMs: 15_000,
});
// res.status, res.headers, res.body (ArrayBuffer)
```

| 行为 | 说明 |
|------|------|
| HTTPS | `verify_peer` + SNI + `host_name_verification`；TLS ≥ 1.2 |
| CA | `Config.ca_bundle` / `VACPS_CA_BUNDLE` → 平台默认路径；缺失 **fail-closed**（不跳过校验） |
| 重定向 | **不**自动跟随（3xx 原样返回） |
| 超时 | Beast `expires_after`（默认 30s） |
| body 上限 | `maxResponseBytes`（默认 8 MiB） |

## 控制面（业务 script，非 C++ 路由）

依赖 monorepo **`@vacps/contracts`**（esbuild 打进 `vacps.mjs`）：

* `registerBackendSchema` → `POST {CONTROL_PLANE_URL}/api/registrations`
* `backendTelemetrySchema` → `POST …/api/telemetry`
* 签名：`x-vacps-id|timestamp|nonce|signature`（与 Node Agent 同一 canonical 串；私钥 PKCS#8 或 raw seed base64url）
* 状态：`agent.db` 表 `agent_state`
* C++ 每 15s `invoke_export("tickControlPlane")`：registration / telemetry + **task pump**

### 任务 API（业务 script）

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/tasks` | `taskDispatchSchema`（`@vacps/contracts`）；可选控制面签名 |
| GET | `/tasks/:id` | 状态 / result / error |
| GET | `/tasks/:id/logs` | `?offset=&stream=` |
| POST | `/tasks/:id/cancel` | 队列中取消 / 运行中打标 |

* 表：`tasks` / `task_logs` / `request_nonces` / `task_idempotency`（migration）
* 执行：`command` / `shell` → `process.run`；`agent` → 409 capability_unavailable
* 并发：pump 单任务（`claimNextQueued`）
* **崩溃恢复**：启动时 `running`/`starting` → `failed` + `agent_restarted`（不自动重跑）
* **retry**：`POST /tasks/:id/retry` → 新 `task_id` 入队（仅终态）
* **idempotency_key**：重复 POST 返回原任务（`deduped: true`）

## `vacps:fs`

路径规则对齐 Node `path-guard`（禁止 `/proc` `/sys` `/dev`；相对路径相对 dataDir）。

全部 API 返回 **`Promise`**（不阻塞 Host `io_context` 线程）：

| API | 实现 |
|-----|------|
| `readText` / `writeText` / `appendText` / `readBytes` / `writeBytes` | `probe_io_uring()` 成功 → Asio `stream_file`；否则 `thread_pool` + 同步读写 |
| `stat(path)` | type / size / mtimeMs / readable / writable（thread_pool） |
| `mkdir` / `exists` / `remove` / `rename` / `list` | 始终 `asio::thread_pool` 卸载 |

Host 持有 2 线程 `thread_pool`（`Host::pool()`）。启动时 `probe_io_uring()`（setup + NOP submit + wait），**不是**只测 compile-time 宏。

**io_uring / Docker 注意**（Asio 无自动降级）：

| 环境 | 典型表现 |
|------|----------|
| Docker 默认 seccomp（拦 setup/register/enter） | `io_uring_queue_init` → EPERM；Asio 构造 `stream_file` 会 **抛** `system_error`。我们 probe 失败 → 永不建 `stream_file`，走 thread_pool |
| 不完整 seccomp（允许 setup，拦 enter） | submit 失败或 Asio 不传播错误 → 操作可能 **永久 pending**。probe 含 submit/wait，失败则不用 stream_file |
| 应用吞掉 `stream_file` 构造异常且不 `reject` Promise | 也会「永久 pending」——`fs_spawn` 已 try/catch + 必 `notify_progress` |

探测失败时日志：`io_uring probe: … — thread_pool fallback`。

## `vacps:process`

| API | 说明 |
|-----|------|
| `run(argv, options?): Promise<RunResult>` | Boost.Process v2 + Asio 协程 |

## `vacps:crypto`（OpenSSL）

| API | 说明 |
|-----|------|
| `randomBytes(n)` | `RAND_bytes` |
| `sha256` / `sha256Hex` | EVP SHA-256 |
| `toHex` / `fromHex` | 十六进制编解码 |
| `base64Encode` / `base64Decode` | OpenSSL `EVP_EncodeBlock` / `EVP_DecodeBlock`（标准 Base64，带 `=` padding） |
| `base64UrlEncode` / `base64UrlDecode` | Base64url（`-_`，encode 无 padding） |
| `ed25519SeedFromPrivateKey(b64url)` | raw 32B seed 或 PKCS#8/DER → 32B seed（`d2i_AutoPrivateKey` + `get_raw_private_key`） |
| `ed25519PublicFromPrivate(priv32)` | seed → 32 字节公钥 |
| `ed25519Sign(priv32, msg)` | 原始 32 字节 seed → 64 字节签名 |
| `ed25519Verify(pub32, msg, sig64)` | 验签 |
