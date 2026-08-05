# vacps-native JS 原生模块接口

当前 **ModuleCatalog** 注册并编译的 `vacps:*` 模块：

| Specifier       | 形态                                      |
| --------------- | ----------------------------------------- |
| `vacps:crypto`  | 同步自由函数                              |
| `vacps:host`    | 同步自由函数（进程信息）                  |
| `vacps:log`     | 同步日志 + async `flush`                  |
| `vacps:store`   | 仅导出 class `Store`（静态 `open` + 实例方法） |
| `vacps:fs`      | class `File` + 命名空间路径操作（async）  |
| `vacps:http`    | outbound `request`（async）+ inbound class `Server` |
| `vacps:process` | class `Process` + `run`（async；子进程） |

另有产品全局 API（非 `vacps:` 模块）：`URL` / `URLSearchParams` / `TextEncoder` / `TextDecoder`。

C++ 提供**能力**；业务路由与策略在 script。模块名固定：`import * as x from "vacps:…"`。

架构与 opaque / Promise 规则见 [`RUNTIME_LAYERING.md`](./RUNTIME_LAYERING.md)。
有状态 class 的所有权与关闭语义见 [`NATIVE_RESOURCE_OWNERSHIP.md`](./NATIVE_RESOURCE_OWNERSHIP.md)。

---

## 全局 API：`URL` / `URLSearchParams` / `TextEncoder` / `TextDecoder`

`Application` 在 `initialize` 时经 `install_global_apis(ctx)` 安装（`src/globals`，Ada URL + 绑定 DSL `ClassBuilder`；纯同步 Env 仅需 `JSContext*`）。
TS 声明见 `script/types/url.d.ts` 等（只声明已实现面）。

### `URL`

| API | 说明 |
| --- | --- |
| `new URL(input, [base])` | 非法 → `TypeError: Invalid URL` |
| `href` / `origin` / `protocol` / `username` / `password` / `host` / `hostname` / `port` / `pathname` / `search` / `hash` | **getters**（Ada） |
| `search` **setter** | 更新 Ada 并 re-parse 到 live `searchParams` |
| `searchParams` | **live** 同一对象；mutation 回写 `search`/`href` |
| `URL.canParse(input, [base])` | static boolean |
| `toString()` / `toJSON()` | → href |

**未实现：** 除 `search` 外的 component setters；`URL.parse` static。

### `URLSearchParams`

| API | 说明 |
| --- | --- |
| `new URLSearchParams([init])` | `init` 仅 **string**（可带 `?`）；省略/null/undefined → 空。**不支持** record/sequence init |
| `append` / `set` / `get` / `getAll` / `has` / `delete` / `sort` / `toString` | 标准语义；`has`/`delete` 可选第二参数 value |
| `size` | getter |
| `entries` / `keys` / `values` | iterator（`next()` → `{value, done}`） |
| `forEach(cb, thisArg?)` | `cb(value, name, searchParams)` |
| `Symbol.iterator` | 同 `entries` |

### `TextEncoder` / `TextDecoder`

标准 Web 子集（UTF-8）；经 `ClassBuilder` 安装。细节以 `src/globals/text_*_binding.*` 为准。

---

## `vacps:log`

| API | 说明 |
| --- | --- |
| `trace/debug/info/warn/error(msg: string)` | 同步写入 spdlog |
| `flush(): Promise<void>` | **async**：`create_async_function` + `Runtime::Async::run_blocking` → `vacps::log::flush()` |

---

## `vacps:host`（进程信息，极薄）

**不是** HTTP / SQL / process / fs 的入口。无 `sleep`。监听地址等产品策略由 script 经 `getenv` 自行读取（见 `script/src/config.ts`）。

| API | 说明 |
| --- | --- |
| `version()` | agent 版本（CMake `PROJECT_VERSION`） |
| `platform()` | 平台三元组，如 `linux-x86_64-musl` |
| `dataDir()` | Host 注入的数据目录（CLI `--data-dir` → `Application::Options::data_dir`，默认 `"data"`） |
| `nowMs()` | Unix 毫秒（`system_clock` epoch ms；有限 number） |
| `getenv(name)` | live `std::getenv`；未设置 → `undefined`；已设置空串 → `""`；非 string / 空名抛错 |

---

## `vacps:crypto`（OpenSSL，同步）

| API | 说明 |
| --- | --- |
| `randomBytes(n)` | `RAND_bytes` |
| `sha256` / `sha256Hex` | EVP SHA-256 |
| `toHex` / `fromHex` | 十六进制编解码 |
| `base64Encode` / `base64Decode` | 标准 Base64（`=` padding） |
| `base64UrlEncode` / `base64UrlDecode` | Base64url（`-_`，encode 无 padding） |
| `ed25519SeedFromPrivateKey(b64url)` | raw 32B seed 或 PKCS#8/DER → 32B seed |
| `ed25519PublicFromPrivate(priv32)` | seed → 32 字节公钥 |
| `ed25519Sign(priv32, msg)` | 原始 32 字节 seed → 64 字节签名 |
| `ed25519Verify(pub32, msg, sig64)` | 验签 |

---

## `vacps:store`（SQLite，class `Store`）

**ModuleCatalog** 注册 `vacps:store`；模块**只导出** class `Store`（无自由 `open()`，无 `begin`/`commit`/`rollback`）。
域实现：`vacps::storage::Store`（`src/storage`）；JS 绑定经 `ClassBuilder` + `store_convert`（`src/modules`）。
TS 声明：`script/types/vacps-store.d.ts`。

每次 `Store.open` 创建**独立**连接与 JS 实例（非进程单例）。绑定对象为每个 Store 持有独立 `Runtime::Async::SerialWorker` strand；完整 SQLite 作业按 JS 提交顺序 FIFO 执行，不靠 mutex 竞争决定顺序。所有会碰连接的方法均为 **async**（`static_async_function` / `async_method` → `Runtime::Async`，worker 只跑纯 C++）。

### 静态 / 只读属性

| API | 说明 |
| --- | --- |
| `Store.open(path, options?)` | `Promise<Store>`。`options.mode`：`'read-only'` \| `'read-write'` \| `'read-write-create'`（默认 create） |
| `readonly path` | open 时传入的路径 |
| `readonly closed` | `close()` 完成后为 `true`（原子标志，不取连接锁） |

### 异步实例方法

| API | 说明 |
| --- | --- |
| `exec(sql)` | `Promise<void>` — 可多语句脚本（无绑定参数） |
| `run(sql, params?)` | `Promise<RunResult>` — 单语句 DML/DDL + 可选绑定；`{ changes, lastInsertRowid }` |
| `query(sql, params?, options?)` | `Promise<Row[]>` — **固定三参形态**；`options?: { maxRows?, maxBytes? }` |
| `transaction(steps)` | `Promise<TransactionResult[]>` — `BEGIN IMMEDIATE` + steps + `COMMIT`；任一步失败 → `ROLLBACK` |
| `close()` | `Promise<void>` — 幂等；可等待、可 run_blocking、可向 JS 报告错误 |

**`query` 签名固定为** `query(sql, params?, options?)`：第二参始终是 params 数组（或省略/`null`/`undefined` → 空绑定）；第三参才是 `QueryOptions`。不存在 `query(sql, options)` 解码路径。

### SQL 值类型

| 方向 | 允许 |
| --- | --- |
| 参数 / 绑定（`SqlParam`） | `null` \| 有限 `number` \| `bigint`（signed int64） \| `string` \| `ArrayBuffer` \| `Uint8Array`（及 TypedArray → blob 拷贝） |
| 结果单元格（`SqlValue` 编码） | `null` \| `number` \| `bigint` \| `string` \| `ArrayBuffer` |
| `RunResult.changes` / `lastInsertRowid` | safe-integer 范围 → `number`；更宽的 signed int64 → `bigint` |

拒绝：`undefined`、boolean、非有限 number、超出 JS safe integer 的整型 number（须改用 `bigint`）、超出 signed int64 的 `bigint`。

- 整型 **number** 须在 JS safe integer 范围内 → SQLite INTEGER（`int64`）；非整数有限 number → REAL。
- **bigint** 仅在 signed int64 内接受 → INTEGER；越界 → 同步 RangeError。
- 结果 INTEGER / rowid：safe-integer → Number，否则 BigInt，保证全 int64 往返。
- `transaction([])` 在 decode 期同步拒绝（steps 不得为空）。

### `query` 限额

| 选项 | 默认 | 语义 |
| --- | --- | --- |
| `maxRows` | `10_000` | 返回行数上限；超出 → 失败 |
| `maxBytes` | 无（不限制） | 近似载荷预算（列名 + 单元格字节）；物化过程中超额 → 失败 |

transaction 内 `type: 'query'` 步骤可带同名 `maxRows` / `maxBytes`（语义相同）。

### `transaction` / `expectedChanges`

```ts
interface TransactionStep {
  sql: string;
  params?: readonly SqlParam[];
  type?: 'run' | 'query';           // 省略 → 'run'
  expectedChanges?: ExpectedChanges; // 仅 run
  maxRows?: number;                  // 仅 query
  maxBytes?: number;                 // 仅 query
}

type ExpectedChanges =
  | { exactly: number }
  | { atLeast: number }
  | { atMost: number };
```

- 整段作为同一连接 FIFO lane 上的一个不可交错作业执行；无独立 begin/commit/rollback API。
- **空 steps 数组**（`transaction([])`）在绑定 decode 期同步拒绝。
- 每个 **run** 步在 `sqlite3_changes` 后立即校验 `expectedChanges`；不匹配 → 整段 rollback，后续步骤不执行。
- **交叉字段校验（绑定 decode 同步拒绝）**：
  - `expectedChanges` 出现在 `type: 'query'` → 错误；
  - `maxRows` / `maxBytes` 出现在 run 步 → 错误；
  - `ExpectedChanges` 必须**恰好一个**键：`exactly` | `atLeast` | `atMost`（值为非负 safe integer）。
- 逐步结果：run → `RunResult`，query → `Row[]`，与 steps 等长。

### 所有权（摘要）

JS opaque 为堆上 `ClassHolder` → `shared_ptr<StoreNative>`；module-private `StoreNative` 唯一拥有 domain `Store` 与该实例的 `SerialWorker`。async 方法帧 / worker 侧保留 `shared_ptr<StoreNative>`，不依赖 JS 包装续命。
显式 `close()` 是业务关闭路径（awaitable / run_blocking / 可报告）。
`ClassBuilder` finalizer **只** `delete` holder（释放一次 `shared_ptr<StoreNative>`），**不**把 `close()` 当业务方法调用；其唯一拥有的 `~Store` 为 `noexcept` best-effort RAII 兜底（释放连接，不触 QuickJS）。详见 [`NATIVE_RESOURCE_OWNERSHIP.md`](./NATIVE_RESOURCE_OWNERSHIP.md)。

---

## Promise / 异步约定（当前）

方向性门面（挂在同一 Runtime / Runtime::Impl，由 Runtime 拥有；持 non-owning Runtime::Impl&；互不替代）：

| 门面 | 方向 | 职责 |
| --- | --- | --- |
| `Runtime::Async` | JS → native `Task` → JS Promise | **唯一** C++→JS Promise 入口与公共 `run_blocking` |
| `Runtime::Callbacks` | native event → JS callback → await sync/thenable | 调用 borrowed JS 函数并经 `await_value` 结算同步值或 thenable；**不**自建第二套 Promise |
| `Runtime::Script` | host → 模块求值 / 导出调用 | owner-thread ESM eval / invoke |

同步 binding（`create_function` / ClassBuilder 同步方法）在**当前 owner-thread QuickJS turn 内直接** decode/invoke/encode；**无** per-callback Runtime 门闸。

- 进程事件循环：Runtime 主 `io_context`（owner 线程）。
- 反向（等 JS Promise / thenable）：`Runtime::await_value`（`Runtime::Callbacks::call_and_await` 委托此路径）。
- 正向（C++ → JS Promise）：**仅** `Runtime::Async`（`promise` / `promise_void` / binding `create_async_function` / `ClassBuilder::async_method` / `static_async_function`）。
- Worker / `run_blocking`：**只**纯 C++；禁止携带 `qjs::OwnedValue` / `JSContext*` / `PromiseCapability`。
- `Runtime::Callbacks` **不**拥有 callback 根；长期根由 binding 状态（如 `ServerNative`）持有，经 `ClassJsEdges` mark/release 暴露给 QuickJS GC。

---

## `vacps:fs`（双后端 File + 命名空间路径操作）

**ModuleCatalog** 注册 `vacps:fs`。域实现：`vacps::fs`（`src/fs`）；JS 绑定经 `ClassBuilder<FileHandle>` + `fs_convert`（`src/modules/fs.cpp`）。
TS 声明：`script/types/vacps-fs.d.ts`。

- **双后端：** 进程级 `io_uring` probe 成功时，`File.open` 在 worker 上 `open(2)`，回到 main executor 后 `random_access_file::assign`；数据路径 `co_await async_read_some_at` / `async_write_some_at`。probe 失败（如 Docker seccomp）走 POSIX `pread`/`pwrite`/`write`，经 `Runtime::Async::run_blocking`。
- **序列化：** JS 模块 `FileHandle` 每句柄持有模块私有 `FileOperationQueue`，串行完整逻辑操作（含 close）；域 `File` 不拥有队列，要求外部串行访问。不跨 `co_await` 持有 `std::mutex`。
- **API：** 仅 `File.open(path, { mode, permissions? })`；bytes-first（`read`/`write`/`readAt`/`writeAt`）；文本用 JS `TextEncoder`/`TextDecoder`。append 句柄拒绝 `writeAt`；append 写为内核 `O_APPEND` + `write(2)`。
- **策略边界：** 原生纯 I/O；相对路径相对 `data_dir`；无路径 allowlist（C++ 与 JS 模块 surface 均不做根目录白名单）。

### 导出

| API | 说明 |
| --- | --- |
| `File.open(path, options)` | `Promise<File>` |
| `file.read` / `readAt` / `write` / `writeAt` | bytes；`writeAt` 对 append 拒绝 |
| `file.truncate` / `stat` / `flush` / `close` | 控制路径；`close` 幂等 |
| `mkdir` / `remove` / `rename` / `stat` / `exists` / `readDirectory` | 命名空间路径操作 |

---

## `vacps:http`（outbound `request` + inbound `Server`）

**ModuleCatalog** 注册 `vacps:http`。导出 **`request`** 与显式 greenfield class **`Server`**。
域实现：`vacps::http::Client` + `vacps::http::Server`（`src/http`，纯 transport）；JS 绑定经 binding DSL + `http_convert`（`src/modules/http.cpp` / `http_convert.hpp` / `http_server_native.hpp`）。
这是 **QuickJS-native DSL** surface，**不是**完整 N-API / `napi_compat` / Node addon 兼容层。
TS 声明：`script/types/vacps-http.d.ts`。

### 导出

| API | 说明 |
| --- | --- |
| `request(options)` | `Promise<HttpResponse>` — pooled outbound HTTP/HTTPS |
| `class Server` | `new Server(options, onRequest)`；`listen()` / `close()`；只读 `listening` / `address` |

---

### Outbound `request`

- **绑定 DSL：** `create_async_function`；`Converter<ClientRequest>` 在 **Promise 创建前同步**解码 options；非法 options 同步抛错。
- **执行路径：** `Runtime::Async` 协程直挂 host Asio / module-scoped `Client`（非 worker `run_blocking`）。
- **连接复用：** 按 `(scheme, host, port)` origin 复用 HTTP/1.1 持久连接；每条连接同时只有一个请求，并保留 Beast read buffer。新建物理连接才做 DNS / TCP / TLS；不伪造无 TTL 语义的 DNS cache。
- **并发与 FD 上限：** 每个 origin 最多 16 条 active 连接，全局最多 64 条 active 连接；全局最多保留 32 条 idle 连接。复用时丢弃 idle 超过 30s 的连接；idle 满额时先清理过期项，再淘汰全局最久未使用的连接。空 origin pool 会随最后一个请求回收。超额请求在 Asio channel 上异步等待，不创建额外 socket。
- **请求 body：** `string` / `ArrayBuffer` / `TypedArray`；响应 `body` 为 `ArrayBuffer`。
- **timeoutMs：** 整段请求**一个**绝对 wall budget；范围 `[1, 3600000]`，默认 `30000`；超时 → `ETIMEDOUT`。
- **取消：** `std::stop_token`（runtime/shutdown → `ECANCELED`）。
- **maxResponseBytes：** 读响应时 Beast `body_limit`；范围 `[1, 67108864]`（64 MiB），默认 8 MiB；超额失败且不全量缓冲。
- **TLS：** HTTPS `verify_peer` + SNI + hostname verification（TLS ≥ 1.2）。CA 路径由 **host composition** 注入（`RuntimeModuleComposition::ca_bundle` ← CLI `--ca-bundle` / `Application::Options::ca_bundle`）；空则 platform defaults；fail-closed。**无** JS `caBundle` 选项。
- **TLS context：** 首个 HTTPS 请求时 lazy 初始化，CA bundle 每个 `Client` 只解析和加载一次；后续新 TLS 连接共享 context。
- **stale 连接：** 复用连接的 I/O 失败会丢弃该连接，不在 transport 内自动重试；避免在请求字节可能已到达对端时重放 POST/PUT/PATCH。
- **重定向：** **不**跟随；3xx 原样返回给调用方。

#### `request` options

| 字段 | 说明 |
| --- | --- |
| `url` | 必填；非空绝对 `http`/`https` URL |
| `method?` | 默认 `"GET"` |
| `headers?` | plain object；值须为 string；只读 own string keys |
| `body?` | `string` \| `ArrayBuffer` \| `TypedArray` |
| `timeoutMs?` | 整数 `1..3600000`；默认 `30000` |
| `maxResponseBytes?` | 整数 `1..67108864`；默认 8 MiB |

#### 响应

`{ status: number, headers: Record<string, string>, body: ArrayBuffer }`。
`headers` 为 null-prototype 对象；重复名 last-wins。

---

### Inbound class `Server(options, onRequest)`

显式构造（**不是**工厂 static open）：

```ts
const server = new Server(
  { host?: string, port: number, /* limits / timeouts … */ },
  (req) => ({ status: 200, body: "ok" }) // or Promise / thenable
);
const addr = await server.listen(); // { host, port } — host is raw numeric
// server.listening / server.address
await server.close();
```

| API | 说明 |
| --- | --- |
| `new Server(options, onRequest)` | `options` 必填 plain object（**`port` 必填**，`0` = ephemeral）；`onRequest` 必填 function；非法 `host` 等在构造时同步 TypeError（Promise 创建前） |
| `listen(): Promise<ListenAddress>` | bind + accept loop；返回有效 `{ host, port }`（`host` 为 raw 数值地址，IPv6 **不**加方括号） |
| `close(): Promise<void>` | 幂等；等待 accept/sessions/handler drain；**不** stop executor |
| `readonly listening` | phase == Listening |
| `readonly address` | Listening 时为 `{ host, port }`，否则 `undefined` |

#### `Server` options（camelCase）

| 字段 | 说明 |
| --- | --- |
| `host?` | **数值** IPv4/IPv6 bind literal（`asio::ip::make_address`）；**禁止** DNS/hostname（如 `"localhost"` 在 `new Server` 同步 TypeError）；默认 `"127.0.0.1"` |
| `port` | **必填**；整数 `0..65535`（`0` = ephemeral） |
| `maxRequestBytes?` | 请求 body 上限；默认 1 MiB；上限 64 MiB |
| `maxHeaderBytes?` | header block 上限；默认 64 KiB |
| `maxResponseBytes?` | handler 响应 body 上限；默认 8 MiB；上限 64 MiB |
| `ioTimeoutMs?` | 单次 read/write idle；`1..3600000`；默认 `30000` |
| `handlerTimeoutMs?` | 单次 `onRequest` **wall-clock deadline**（启动协作取消）；`1..3600000`；默认 `30000`。**不是**硬完成上界：`close()` 仍等待 handler drain，非协作 C++ handler 不会被强制销毁 |
| `backlog?` | 若提供须为 `1..65535` |
| `reuseAddress?` | bool；默认 `true` |

`ListenAddress.host` 是 bound endpoint 的 **raw 数值地址**（`address::to_string`）；IPv6 **不加** `[]`。拼 URL 时由调用方格式化（例如 IPv6 → `"[" + host + "]:" + port`）。

#### `onRequest` 与 binary DTO

- 入站请求（binary-first）：
  `{ method, url, httpVersion, headers, body: ArrayBuffer, remoteAddress }`
  - `url` = raw HTTP request-target（path + query）
  - `httpVersion` = 常规字符串（如 `"1.1"`），不是 transport ×10 整数
  - `body` 为 `ArrayBuffer`
  - JS `headers` 对象可能 last-wins 折叠重复名；transport DTO 保留到达顺序
- 回调返回（同步值或 Promise/thenable）：
  `{ status, headers?, body? }`；`body` 接受 `string` / `ArrayBuffer` / `TypedArray`
- 绑定经 **`Runtime::Callbacks::call_and_await`**：native event → JS 函数 → await 同步结果或 thenable（委托 `await_value`；不经 `Runtime::Async` 再包一层 Promise 子系统）。

#### 纯 transport 边界（域 `vacps::http::Server`）

- **调用方 executor**：不拥有 / 不 `stop()` `io_context`；accept / sessions / handler 均在调用方 owner executor 上。
- **one-shot 生命周期：** `Created → Listening → Closing → Closed`（bind 失败可回 `Created` 重试；Listening 后不可二次 `listen`）。
- **私有 sessions**；binary DTOs；无 product routes、无 JSON 错误策略。
- **transport-owned headers**（大小写不敏感，handler 不得设置）：`Connection`、`Content-Length`、`Transfer-Encoding`、`Upgrade`、`Trailer`、`TE`、`Keep-Alive`、`Proxy-Connection`。
- **固定最小错误响应：** `400` / `413` / `500` / `504`，body 为纯文本且带 `Content-Type: text/plain; charset=utf-8`；handler 失败 / 非法响应 / transport-owned header 违规 → `500`；handler wall deadline 触发协作取消 → `504`；body 超限 → `413`；畸形请求 → `400`。
- **取消：** handler `stop_token`（server close / session cancel / `handler_timeout` wall deadline）；runtime stop 协作取消。`handler_timeout` **不**强制销毁非协作 handler；`async_close` 仍 drain。
- **无** Host 级 Server registry；产品监听地址与路由策略在 script。

#### 生命周期与 GC（`ServerNative` + `ClassJsEdges`）

```text
JS Server instance
└── ClassHolder → shared_ptr<ServerNative>   # ClassBuilder async frames 需要 shared
    ├── on_request : qjs::OwnedValue          # callback root（native 拥有）
    ├── Runtime::Callbacks                      # non-owning Impl&
    └── optional<http::Server>                # domain handle（值类型；State 自持有）

Transport ServerHandler ──weak_ptr<ServerNative>──▶  (无 Native ↔ Server 自根环)
```

- **callback root** 由 native 状态拥有，经 `ClassJsEdges` **mark** 暴露给 QuickJS GC，**release** 时 `JS_FreeValueRT`（仅 VM edge 记账）。
- Transport handler **只**捕获 `weak_ptr<ServerNative>`；不形成 self-root / Native↔Server 循环。
- **显式 JS `close()`** 是业务生命周期：await domain drain 后在 owner 线程 `drop_on_request()`。产品/Application shutdown **MUST** 调用它；Runtime **不**提供 handle-cleanup registry 或隐藏业务 close。
- **Finalizer** 只跑 `ClassJsEdges::release` + 删除 holder；**不**调用业务 `close()`。domain `~Server` 仅 **post 非阻塞 `dispose()`**（不 join、不 stop executor）。
- Natural Asio drain 在仍有 live Server/accept/session/handler 工作（含未 `close` 的业务资源）时不会 FreeContext；不协作可诚实阻止进程 shutdown 完成。

---

## `vacps:process`（子进程）

导出 class `Process` + 自由函数 `run`。域层见 [`PROCESS_RUNTIME.md`](./PROCESS_RUNTIME.md)。
TS：`script/types/vacps-process.d.ts`。

| API | 说明 |
| --- | --- |
| `new Process(command, args?, options?)` | 未 spawn；options 同步解码；产品 composition 必须提供 `Runtime::Async` / `ProcessRuntime` |
| `start(): Promise<void>` | 在 `main_executor` 上 spawn（`setpgid` 进程组） |
| `write(data): Promise<number>` | 串行 stdin 写；`string` / `ArrayBuffer` / `TypedArray` |
| `wait(): Promise<ProcessResult>` | 进程退出 **且** stdout/stderr drain 完成后返回 capture（handle 仍 open 时）；本身不释放 buffer |
| `terminate(signal?): Promise<void>` | 信号进程组；在请求发出后 resolve（非等待退出）。`signal` 仅 `SIGTERM`/`SIGINT`/`SIGKILL`（缺省 SIGTERM）；未知字符串 **同步** TypeError |
| `close(): Promise<void>` | `async_close`：SIGKILL + 取消管道，await 真实 reap/drain 后释放 slot/buffer（使 native capture 失效；并发 outstanding `wait` 可能失败）。幂等；忽略 injected stop |
| `run(command, args?, options?)` | create→start→wait→close；默认 stdin `ignore` |

`ProcessOptions`（诚实窄面）：`cwd?`、`timeoutMs?`（0/omit=无）、`stdin?: 'pipe' | 'ignore'`、`maxStdoutBytes?` / `maxStderrBytes?`（0..64MiB，0 不保留）。提供 `env` 或 `stdout`/`stderr` 模式键 → 同步 TypeError。无 `pid`/`running`/`read`。

`ProcessResult`：`{ exitCode, timedOut, stdout, stderr }` 仅此四字段。

默认：`Process` 类 stdin pipe/open；`run` stdin ignore/closed。所有域工作在 `detail::Runtime::Impl::main_executor`；stop_token 经 dispose 桥到 owner executor（不在 callback 线程改域状态）。

以 `ModuleCatalog` 构造函数与 `CMakeLists.txt` 为准。
