# Runtime 分层模型

产品目标二进制：`docker/build.sh release --native-only`（或完整 `release`）。

## 1. 总览

Native 侧以 **`vacps::Runtime`** 为公共门面：内部唯一实现对象 **`Runtime::Impl`**（`unique_ptr` 独有；**无** 全 Runtime 的 `shared_ptr` / 共享 god-object）持有 Asio + QuickJS、有界 job pump、阶段机、runtime-wide cooperative stop、Asio natural outstanding-work drain、双向 Promise，以及稳定的方向性能力 Async/Callbacks/Script。

- **公共门面**：`vacps::Runtime`（**不是** `vacps::runtime::Runtime`）；noncopyable / nonmovable，地址稳定。
- **方向性能力**（由 `Runtime::Impl` 直接拥有；`async()` / `callbacks()` / `script()` 返回引用；互不替代）：
  - **`Runtime::Async`**：JS → native `Task` → JS Promise / worker `run_blocking`；可为有序业务资源创建 `SerialWorker` strand；**唯一** C++→JS Promise 与异步生命周期拥有者。
  - **`Runtime::Callbacks`**：native event → JS callback → await 同步值或 thenable（委托 `await_value`；不自建第二套 Promise；不拥有 callback 根）。
  - **`Runtime::Script`**：host 模块求值 / 导出调用。
- **同步 binding**：`create_function` / ClassBuilder 同步方法在 **当前 owner-thread QuickJS turn 内直接** decode/invoke/encode。真正的 Runtime 入口已建立 owner-thread / live-context / lifecycle 不变量；**无** per-callback Runtime 门闸或同步能力 facade。
- **实现对象**：`Runtime::Impl`（`unique_ptr` 独有；能力对象与 Impl 同寿）。**永不**经 `Runtime::core()` / `state()` 或其它产品 accessor 暴露。Promise capability 由 `co_spawn` completion handler 独占，`run_blocking` 通过 Asio completion 传回 typed outcome；仅 `AwaitState` 等确有跨 JS callback 共享寿命的操作局部状态使用 `shared_ptr`。
- **`Application` + `EntryModule`**：生产进程组合根。Host 拥有 **`ModuleCatalog`**（须活过 `Runtime` / `JSRuntime` 拆解）。Async/Callbacks/Script 由 **`Runtime::Impl`** 拥有；向 `Env`、`NativeSlot`、`RuntimeModuleComposition` 传递非拥有 `Runtime::Async*` / `Runtime::Callbacks*`（指向 Impl 内稳定对象，随 Runtime 寿命有效）。
- **Binding DSL**（`vacps::binding`）+ **`qjs::OwnedValue`**：类型安全导出；JS 值唯一拥有层在 `vacps::qjs`。有状态 class 可经 **`ClassJsEdges`** 向 GC mark/release native 拥有的 JS 边。
- **Catalog 当前注册**：`vacps:crypto`（同步）、`vacps:host`（`version` / `platform` / `dataDir` / `nowMs` / `getenv`）、`vacps:log`（含 async `flush` → `Runtime::Async::run_blocking`）、`vacps:store`（class `Store`）、`vacps:fs`（class `File` + 路径操作）、`vacps:http`（outbound `request` + inbound class `Server`）。
- **Globals**：`URL` / `URLSearchParams` / `TextEncoder` / `TextDecoder`。
- **仍属 `vacps::runtime`**：`Error` / `Result` / `Task` / `JsEncode` 及低层工具（语义上属于 runtime 子系统，非门面嵌套类型）。

## 2. 源码树与层

```text
src/
├── bootstrap/  # 进程级初始化 + 唯一全局 C++ new/delete override TU
├── qjs/        # OwnedValue、ScopedCString（vacps::qjs）
├── binding/    # Binding DSL（header-only，vacps::binding；ClassJsEdges）
├── runtime/    # vacps::Runtime 门面 + Async/Callbacks/Script
│   └── detail/ # Runtime::Impl（内部实现；非产品 API）
├── host/       # command_line、Application::Options、EntryModule
├── globals/    # URL / URLSearchParams / TextEncoder / TextDecoder
├── modules/    # ModuleCatalog + crypto/host/log/timer/store/fs/http/process
├── http/       # outbound client + inbound server transport（纯 C++）
├── app/ …      # 其它域库与进程入口
└── main.cpp
```

| 层           | 类型                                                                         | 职责                                                                                                                   |
| ------------ | ---------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| 入口         | `main.cpp` → `vacps-agent`                                                   | argv→`parse_command_line`；help/version 直接退出；run → process_init → `Application(Options)` → `initialize` + `run()` |
| Host         | `parse_command_line`、`Application`、`EntryModule`                           | CLI→Options；组合 Runtime、catalog、信号、入口 ESM                                                                     |
| 门面         | `vacps::Runtime`                                                             | 生命周期、post_to_owner、evaluate、await_value、executors、稳定能力引用                                                |
| 实现         | `Runtime::Impl`                                                              | 主 `io_context`、worker pool、phase、job pump、关机、能力对象                                                          |
| 引擎         | `JsEngine`                                                                   | `JSRuntime`/`JSContext`；专属 mimalloc backing heap；求值与 pending job                                                |
| 能力         | `Runtime::Script` / `Async` / `Callbacks`                                    | 模块求值；Promise / run_blocking；native→JS callback await                                                             |
| 同步 binding | 当前 QuickJS turn 内直接执行                                                 | `create_function` / ClassBuilder；无 per-callback Runtime 门闸                                                         |
| Promise      | `PromiseCapability`、`Runtime::await_value`                                  | 正向 native Promise；反向 JS thenable（Callbacks 复用此路径）                                                          |
| 原语         | `qjs::OwnedValue`、`ScopedCString`                                           | 唯一 JS 值 / CString 拥有层                                                                                            |
| DSL          | `ModuleBuilder`、`ClassBuilder`、`create_function` / `create_async_function` | 直接使用 `qjs::OwnedValue`                                                                                             |
| 模块         | `ModuleCatalog` + crypto/host/log/timer/store/fs/http/process                | 见上                                                                                                                   |
| 域库         | crypto / url / text / fs / http / …                                          | 纯 C++；未进 catalog 则无 JS 导出                                                                                      |

依赖：Runtime 与 Binding 均直接依赖 `vacps::qjs`。禁止在 run_blocking / worker 边界携带 JS 拥有 RAII。

`Runtime::Impl` **拥有**稳定能力对象（Async/Callbacks/Script），保证能力在整个 Runtime 寿命内有效。**不**拥有业务资源、模块目录或全局 API 安装器。能力仅持非拥有 `Impl*`，无共享所有权环。完整 Runtime 实现**没有** `shared_ptr` 共享所有权。

## 3. 已实现行为

### 3.1 Runtime / Runtime::Impl

- 内部生命周期：`created → initialized → running → stopping → closed`。
- API：`initialize` → `run(startup)` → `request_stop`；`async()` / `callbacks()` / `script()`（引用）；`post_to_owner` / `context` / `evaluate` / `await_value` / main executor。
- **无**公共 `core()` / 内部状态 accessor。
- **主线程**唯一驱动 `main_io` 并触碰 QuickJS；**worker 仅纯 C++**。
- `JsEngine` 经 `JS_NewRuntime2` 为 QuickJS 创建独立 mimalloc heap。进程的普通 C++
  `new/delete` 经 `bootstrap/mimalloc_new_delete.cpp` 进入 mimalloc；TSan 配置不编入该
  TU，由 sanitizer runtime 的强符号拦截器接管。`MI_OVERRIDE=OFF`，不覆盖 C
  `malloc/free`。QuickJS 关闭顺序固定为 `JS_FreeContext` → `JS_FreeRuntime` →
  `mi_heap_delete`，确保其专属 heap outlive 所有 QuickJS 分配。
- 有界 job pump；仅 QuickJS 确有 pending job 时 post，并合并重复调度。`post_to_owner` 任意线程投递到 JS owner 线程（仅 `initialized`/`running` 接新工作）。posted callable **不得**抛异常（owner handler 为 `noexcept`）。
- owner 建立后 Runtime 析构必须在 owner 线程（Narrow 寿命合同）。
- **Wide/Narrow（摘要）**：owner/context/lifecycle 前置条件由调用方建立。Narrow 实现体不重复检查，也不把同一亲和性包装成可恢复 `Result`。
  - `initialize`：Narrow（`created`、只调一次）；`Result` 仅表示引擎/分配失败。
  - `run`：Narrow（`initialized`、同 owner 线程、只调一次）；整数返回值表示运维级启动/主循环/关机失败，不是程序员误用。
  - `context`：Narrow（owner 线程、引擎打开、`initialized|running|stopping`）；不把 off-owner/post-close `nullptr` 当作产品恢复路径。
  - `evaluate`：Narrow（owner + 活 phase）；`Result` 表示 JS 求值/job 调度失败。
  - `await_value` / `Callbacks::call_and_await`：Narrow（owner/live/running + 匹配的 live `OwnedValue`）；前置条件不动态检查。JS reject/超时/调用方取消/runtime shutdown 取消仍为运维结果。
  - `Runtime::Async::promise` / `run_blocking`：在已建立的 owner turn 内调用；不复查亲和性前置条件，无亲和性 `Result` 门闸。引擎在 natural drain 完成前保持打开；协程与 completion handler 在 outstanding work 存活期间 settle 一次。
  - **保留的生命周期分支**（不是冗余 owner-thread 门闸）：`request_stop` 任意线程幂等、`post_to_owner` 在 admission 与执行之间观察到 stopping 时丢弃 callable、worker 取消与异常编组、runtime stop token 与调用方 stop token 对 reverse await 的协作取消。

### 3.2 原语与能力

- **`qjs::OwnedValue`**：唯一 `JSValue` 拥有类；empty（`ctx_ == nullptr`）与带 context 的 `JS_UNDEFINED` 可区分。
- **`qjs::ScopedCString`**：`JS_ToCStringLen` RAII。
- `Runtime::Script`：owner 上 ESM `evaluate_module` / `invoke_export`。
- **同步 binding**：`create_function` / ClassBuilder 同步路径在当前 owner-thread QuickJS turn 内直接 decode/invoke/encode；不经 Runtime 同步能力 facade。
- **`Runtime::Async`**：唯一 C++→JS Promise 入口与公共 `run_blocking`。Start/Encode 在 JS 线程；worker 只携纯 C++。`make_serial_worker()` 为需要 FIFO 的单个业务资源创建独立 worker strand；它不拥有 Runtime，复制后仍指向同一条 lane。
- **`Runtime::Callbacks`**：native event→JS 函数→await sync/thenable。`call_and_await` 仅借用 callable 做同步 `JS_Call`，不跨挂起保留根；参数 `OwnedValue` 在首次挂起前释放。thenable 结算 / 超时 / 调用方与 runtime `stop_token` 取消全部走既有 `await_value`。不维护 JS-handle shutdown cleanup registry。

### 3.3 Promise

- **正向**：`PromiseCapability`（不离 JS 线程）。引擎在 Asio natural drain 完成前保持打开，因此 capability 在操作存活期间可 settle 恰好一次；无 Promise `abandon` / 无引擎提前关闭下的“存活探测”分支。`JS_NewPromiseCapability` 失败返回 `JS_EXCEPTION`。Start/Encode 的 `Result` 失败与协程内非分配异常经唯一 Asio completion / settle 路径 **reject**。同步 native setup（task-state 分配、callable 物化、`co_spawn` 发起）异常不在 `promise()` 内捕获重标为 allocation reject（`promise`/`promise_void` 为 `noexcept`，意外同步失败 fail-fast）。在该正向 Promise 路径上，native `std::bad_alloc` 经 `error_from_exception_ptr` / 相关 binding 边界 fail-fast，不转化为 Promise rejection / JS InternalError；QuickJS C API 分配失败仍为 `JS_EXCEPTION`，与 native OOM 不同路径。
- **反向**：`Runtime::await_value` — then 反应 + 可选超时；每个 pending reverse await 同时监听 **调用方** `JsAwaitOptions::stop`（若提供）与 **Runtime::Impl 的 runtime-wide shutdown `std::stop_token`**。不轮询 `JS_PromiseState`。无 `ReverseAwaitTracker`、无 abandoned 状态。`Runtime::Callbacks` 复用此路径，不另建 Promise 子系统。owner/live/running 与匹配 live `OwnedValue` 是不动态检查的 Narrow 调用方前置条件（非 `Error::invalid_state` 恢复）。JS reject / 超时 / 取消（含 runtime shutdown 取消）仍为运维 `Result`。

### 3.4 Normal shutdown（Asio natural drain）

权威 outstanding-work 记账是 Asio 自身。Runtime **不**维护任务计数器、pending registry、deadline waiter、Promise pool，或 second-run / late-completion drain。

1. `begin_shutdown()`（owner 线程、同步、幂等）：`running → stopping`；sticky stop intent；**唯一** runtime-wide `std::stop_source::request_stop()`；**仅**释放 daemon `work_guard`（唯一人工 keepalive）。不 `co_spawn` 关机协程，不 `main_io_.stop()` / `restart()` / `poll()`，不关闭引擎。
2. `main_io_.run()` 继续运行：已排队/运行中的 `Runtime::Async` 操作完成或协作取消；`run_blocking` 完成从 worker 回到 main_io；reverse await 收到 runtime（及可选调用方）取消并结束；Promise settle 调度有界 job-pump turn。产品/域资源须已由 JS/Application 显式关闭。
3. 当 Asio 报告无 outstanding work 时，`main_io_.run()` **自然返回**。
4. 内部不变量：QuickJS job queue 应已空（漏调度 job-pump 是 bug，不是用 deadline 同步 drain 掩盖）。
5. 然后才 `close_engine()` / FreeContext → `phase = closed` → `worker_pool.join()`。

非协作操作、未关闭的业务资源、或 Application 未取消的 `signal_set` 等待，都是真实 outstanding work，**可诚实阻止** `run()` 返回。不设 force-close deadline，不在 drain 中途关闭 QuickJS。

### 3.5 Application / EntryModule

- `initialize`：`runtime.initialize` → `ProcessRuntime(main_executor)` → 直接构造 `ModuleCatalog`（注入必需的 `Runtime::Async&` / `Runtime::Callbacks&` / `ProcessRuntime&`）→ `install_loader` → `install_global_apis`。
- **不**覆盖 `JS_SetContextOpaque`（`vacps::Runtime*`）。`install_loader` 将 **`RuntimeModuleComposition*`** 设为 `JS_SetRuntimeOpaque`，loader opaque 为 **catalog `this`**（catalog **immovable**）。
- 析构：signals / EntryModule → Runtime（Impl/能力）→ ProcessRuntime → ModuleCatalog。
- EntryModule：接受 `vacps::Runtime&`；非拥有 `JSModuleDef*`；经 `Runtime::Script` + `await_value` 调入口 ESM 的 `initialize`/`shutdown` 导出。
- 停止：信号/`Application::request_stop` →（若入口就绪）`entry.shutdown`（JS 显式关闭业务资源）→ Application 在最终 stop 前 cancel/reset 自有 `signal_set` → `Runtime::request_stop`。`Application::request_stop` 经 **`Runtime::post_to_owner`** 编排（handlers 仅捕获 construction-time callback lifetime token 的 `weak_ptr`，不捕获 raw `this` / JSValue）；sticky `stop_requested_` 在 `Runtime::run` 前已置位时会 `Runtime::request_stop` 以确定性跳过 startup。未取消的 signal wait 是真实 outstanding work，会阻止 `main_io_.run()` 自然返回。

### 3.6 Binding 与模块

- 同步路径：`create_function` / `ClassBuilder` 在当前 owner-thread QuickJS turn 内直接执行（无 per-callback Runtime 门闸）。
- Async：经非拥有 `Runtime::Async*` 进入唯一 Promise 入口（注册时非空 `Runtime::Async*` 为 Narrow 组合前置条件；缺失 wiring 不是友好的 JS `InternalError`）。Promise 创建前的 **Wide** 输入/准入错误抛 JS 异常且不建 Promise；之后由 `Runtime::Async` reject。
- Inbound `Server`：module-private `ServerNative` 拥有 `onRequest` 根 + domain `http::Server`；经 `Runtime::Callbacks` 调 JS。
- `vacps:process`：域工作仅 `main_executor`。见 [`PROCESS_RUNTIME.md`](./PROCESS_RUNTIME.md)。
- `vacps:timer`：`steady_timer` 直接运行在 `main_executor`；不投递 worker。产品周期循环由 JavaScript 持有并在 `shutdown()` 中停止/等待。
- Globals：`ClassBuilder`；WebIDL ToString 用 `binding::try_coerce_string`。

### 3.7 启动配置

C++ 进程旋钮**仅**来自 CLI（`host::parse_command_line` → `Application::Options`）：
`--script`、`--data-dir`、`--log-level`、`--ca-bundle`、`--js-heap-limit-bytes`、`--js-stack-limit-bytes`、`--js-time-budget-ms`、`--lifecycle-timeout-ms`。
不读 `VACPS_DATA_DIR` / `VACPS_LOG_LEVEL` / `VACPS_SCRIPT` / `VACPS_CA_BUNDLE` / `VACPS_JS_*`。
未在 CLI 给出的引擎字段保持 `runtime::EngineOptions` / `Runtime::Options` 默认值。
`data_dir` / `ca_bundle` 经 composition 注入（**`vacps:host` `dataDir()`**；`vacps:http` TLS）。
产品策略（listen、auth、控制面密钥、FS roots 等）仍由 script 经 **`host.getenv()`** 读环境变量。
`Application::run()` 无脚本参数，加载 `Options::script_path`（main 在未传 `--script` 时解析默认路径候选）。

## 4. 有意未做

| 项                                              | 说明                                                 |
| ----------------------------------------------- | ---------------------------------------------------- |
| `napi_compat` / 完整 N-API v9 / Node addon 兼容 | **不存在**；surface 保持 QuickJS-native binding DSL  |
| HTTP product routes / JSON 错误体               | 属 script；native `vacps:http` 仅为 binary transport |

已编译 catalog：`vacps:crypto` / `host` / `log` / `timer` / `store` / `fs` / `http` / `process`。以 `CMakeLists.txt` 与 `ModuleCatalog` 构造函数为准。

## 5. 所有权与线程

| 规则                           | 内容                                                                                                        |
| ------------------------------ | ----------------------------------------------------------------------------------------------------------- |
| JS 线程                        | `initialize` 成功后的调用线程；之后所有 QuickJS API 仅此线程                                                |
| Runtime 析构                   | owner 建立后必须在 owner 线程销毁                                                                           |
| QuickJS allocator              | `JsEngine` 独占 mimalloc heap；outlive `JSRuntime`；不使用 `mi_heap_destroy` 隐藏 live allocation           |
| C++ allocator                  | 唯一 bootstrap TU 覆盖全局 `new/delete`（TSan 除外）；C `malloc/free` 保持 musl                             |
| Worker / run_blocking          | 禁止 `JSContext*` / `JSValue` / `qjs::OwnedValue` / `ScopedCString` / `PromiseCapability`                   |
| `post_to_owner`                | 成功则 callable 在 owner 销毁；失败则在调用线程销毁；callable 不得抛异常                                    |
| JSContext opaque               | `vacps::Runtime*`（若保留该槽；Host/modules 不得覆盖）                                                      |
| JSRuntime opaque               | catalog 拥有的 `RuntimeModuleComposition*`                                                                  |
| ModuleCatalog                  | Host 拥有；非 process-static；**noncopyable + immovable**；outlive `JSRuntime`                              |
| Runtime 能力                   | Runtime::Impl 拥有稳定 Async/Callbacks/Script；应用/测试组合根只持非拥有指针；无 adapter / 无 optional 副本 |
| ProcessRuntime                 | Host 拥有；`main_executor` + `ProcessBudget`；outlive Runtime/QuickJS teardown；catalog 持非拥有指针        |
| Env / NativeSlot / Composition | 非拥有 `Runtime::Async*` / `Runtime::Callbacks*` / `ProcessRuntime*`（纯同步路径可仅持 `JSContext*`）       |
| EntryModule                    | 非拥有 `JSModuleDef*`；API 接受 `vacps::Runtime&`                                                           |
| 无业务 registry                | Host 不维护 File/Store/Server 表；无 JS-handle shutdown cleanup registry                                    |
| Runtime stop                   | 唯一 runtime-wide `std::stop_source`；async/reverse-await 内部取 `stop_token`                               |
| Daemon work_guard              | 唯一人工 keepalive；`begin_shutdown` 时释放；之后靠真实 outstanding work                                    |
| `request_stop`                 | 任意线程、幂等；sticky intent + post `begin_shutdown`；调用线程不碰 QuickJS；不 `main_io_.stop()`           |

## 6. 关机顺序

```
SIGINT/SIGTERM 或 Application::request_stop
  → (entry 已 initialize) EntryModule::shutdown
       JS/Application MUST 显式关闭业务资源（Server/Store/File/Process 等）
  → Application 在最终 Runtime stop 前 cancel/reset 自有 signal_set
  → Runtime::request_stop
  → begin_shutdown:
       phase = stopping
       shutdown_stop_source.request_stop()
       daemon_work_.reset()
  → main_io_.run() 继续（natural outstanding-work drain）
       Runtime::Async / run_blocking / reverse await / job-pump 在引擎仍打开时结算
  → main_io_.run() 自然返回（无 outstanding work）
  → close_engine / FreeContext → phase = closed
  → worker_pool.join()
  → Host: entry.reset
  → 析构：Runtime（Impl/能力）→ ProcessRuntime → ModuleCatalog
```

- Asio outstanding work 为权威；daemon work_guard 是唯一人工 keepalive，stop 时释放。
- 引擎仅在 natural drain 之后关闭。
- 无 `AsyncScope`、`ReverseAwaitTracker`、JS cleanup registry、Promise/reverse-await abandon、shutdown deadline、或 late-completion drain。
- 正常关机路径 **不** 调用 `main_io_.stop()` / `restart()` / `poll()`。
- JS 未关闭业务资源或操作不协作时，shutdown **可诚实不完成**；不设 force-close 掩盖。
- 不强杀非协作 worker；其完成仍经 main_io 回到 owner。

## 7. 路线图

1. 基线：`vacps::Runtime` + `Runtime::Impl`（含 Async/Callbacks/Script）、双向 Promise、binding DSL 同步路径直接执行、crypto/host/log/timer/store/fs/http/process、globals。
2. 其余域能力按需迁入 ModuleCatalog + binding DSL。
3. 完整 N-API / `napi_compat` **不在**当前目标。

## 8. 构建

- 产物：`vacps-agent` → `vacps-agent-linux-x86_64`。
- 推荐：`docker/build.sh release --native-only`。
