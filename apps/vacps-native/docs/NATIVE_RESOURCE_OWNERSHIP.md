# JavaScript Native Resource Ownership

## 核心原则

> JavaScript 创建并拥有业务资源；C++ 实现能力与资源对象；Host 只管理共享基础设施与 JS 引擎生命周期，不管理 JavaScript 创建的业务对象集合。

当前已编译的 JS surface 含无状态自由函数、全局值类型，以及有状态 native class **`vacps:store` → `Store`**、**`vacps:fs` → `File`**、**`vacps:http` → `Server`**、**`vacps:process` → `Process`**（另有 `vacps:crypto` / `vacps:host` / `vacps:log`，`URL` / `Text*`）。下列规则约束 **Host / Runtime / Binding** 边界，并直接适用于已接入 catalog 的有状态 native class。

---

## 一、当前组合根（Host）

生产组合根是 **`Application`**。

```text
Application                      # 生产组合根
├── Runtime                      # 门面；Runtime::Impl 独有；拥有稳定 Async/Callbacks/Script
├── ProcessRuntime               # main_executor + ProcessBudget（outlive Runtime）
├── ModuleCatalog                # immovable；loader opaque = this
│   └── RuntimeModuleComposition # JSRuntime opaque（required Runtime capability / ProcessRuntime references + data_dir 等）
└── EntryModule                  # 非拥有 JSModuleDef*；initialize/shutdown 导出
```

`RuntimeModuleComposition` 持有必需的非拥有 `Runtime::Async&` / `Runtime::Callbacks&` / `ProcessRuntime&`；`Env` / `NativeSlot` 在同步与异步 binding 共用表示时可携带非拥有 capability pointer。Async/Callbacks 是 Impl 内稳定对象，随 Runtime 寿命有效；纯同步路径的 `Env` 可仅持 `JSContext*`。

析构顺序（声明逆序）：signals / EntryModule → **Runtime**（FreeContext + Impl/能力）→ **ProcessRuntime** → **ModuleCatalog**。
ProcessRuntime/budget、catalog 必须 outlive `JSRuntime` 拆解。

**禁止** Host：

- 枚举 / registry 所有 JS 创建的业务对象；
- 在 SIGTERM 时替 JS 猜测 File/Store/Server/Process 关闭顺序；
- 覆盖 `JS_SetContextOpaque`（固定为 `vacps::Runtime*`）；
- 保存 Async/Callbacks 的值副本（能力由 Runtime::Impl 拥有；组合根只持非拥有指针）。

优雅停止：

```text
信号或 request_stop
→（入口已就绪）EntryModule::shutdown 导出
     JS MUST 显式 close 业务资源（Server/Store/File/Process 等）
→ Application cancel/reset 自有 signal_set，再 Runtime::request_stop
→ begin_shutdown：phase=stopping；runtime-wide stop_source.request_stop()；释放 daemon work_guard
→ main_io_.run() natural outstanding-work drain（引擎仍打开；Promise/reverse-await/job-pump 正常结算）
→ run 自然返回后 close QuickJS / FreeContext → join workers
```

非协作操作或 JS 未关闭的业务资源可诚实阻止 shutdown 完成。无 AsyncScope、ReverseAwaitTracker、JS-handle cleanup registry、Promise abandon、shutdown deadline 或 late-completion drain。

---

## 二、有状态 native class（模型）

每次 JS 创建资源时，创建独立的 C++ 对象。Binding 使用 `ClassBuilder<T>`：JS opaque 为堆上 **`ClassHolder`**，内持 **`std::shared_ptr<T>`**：

```text
JavaScript object
└── opaque ClassHolder (heap)
    └── shared_ptr<T>          # 例如 shared_ptr<storage::Store> / ServerNative
```

资源不由 Host 预创建，也不通过全局整数 ID 暴露。正常关闭顺序由 JS 表达：

```ts
export async function shutdown(): Promise<void> {
  await server.close();
  await store.close();
}
```

Host 只调用统一生命周期入口，不维护 `FileRegistry` / `StoreRegistry` / `stopAll()` 之类业务表。

**`vacps:store`：** 仅导出 class `Store`；`Store.open` → 新 `shared_ptr<Store>` 经 class-aware wrap 交给 JS。无进程级 Store 单例。

**`vacps:http` `Server`：** 显式 `new Server(options, onRequest)` → module-private `ServerNative`（非公开域类型）经 class-aware wrap 交给 JS。见下文「Server 生命周期」。

---

## 三、显式关闭 API

持有外部资源的 class 必须提供：

```ts
interface ClosableResource {
  readonly closed: boolean;
  close(): Promise<void>;
}
```

`close()`：

- 幂等；明确完成语义；拒绝后续新操作；
- **可等待**（返回 Promise）、**可 run_blocking**（worker 上跑纯 C++ 释放）、**可向 JS 报告错误**；
- 等待或取消已提交工作；二次调用不因“已关”而抛资源错误。

每个对象管理自己的子资源（acceptor/sessions、child/pipes、sqlite 连接、fd）。

对 `Store`：显式 `close()` 是业务关闭路径——在 Store 锁下标记 `closed` 并释放 `Database`/sqlite 连接；经 `ClassBuilder::async_method` 进入 `Runtime::Async`。

对 `Server`：显式 `close()` 是业务关闭路径——await domain `async_close` drain 后，在 owner 线程 `drop_on_request()`；经 `ClassBuilder::async_method` 进入 `Runtime::Async`。

对 `Process`：显式 `close()` 是业务关闭路径——await domain `async_close`（SIGKILL 进程组 + 取消管道 + 真实 reap/drain 屏障）后释放 slot/buffer；经 `ClassBuilder::async_method` 进入 `Runtime::Async`。finalizer/`dispose` 仅 post 非阻塞 kill/cancel，不伪造 exit/eof。详见 [`PROCESS_RUNTIME.md`](./PROCESS_RUNTIME.md)。

---

## 四、QuickJS finalizer 只丢 VM edge + holder（不是业务 close）

```text
JS 对象不可达
→ ClassBuilder finalizer
→（若 ClassJsEdges<T>::enabled）ClassJsEdges::release   # JS_FreeValueRT 等 VM edge 记账
→ delete ClassHolder                                     # 放下 shared_ptr<T>
→（若无 async 帧 / 其它持有者）~T  noexcept best-effort RAII
```

`ClassBuilder` / `ClassHolder` finalizer：

- 可选先跑 **`ClassJsEdges::release`**（仅释放 native 拥有的 `JSValue` / `OwnedValue` 边；官方 QuickJS 在 class finalizer 里 `JS_FreeValueRT` 属 VM bookkeeping）；
- 再 **只**销毁 holder、放下其中的 `shared_ptr<T>`；
- **不**把 `close()` 当作业务方法调用；
- **不**创建 Promise、不 `JS_Call`、不等待协程、不阻塞；
- **不**访问 Host service locator、不依赖其它 JS 对象。

因此：**不要**写成“GC finalizer 直接调用 `close()`”。Finalizer 与显式 `close()` 是两条路径。

`ClassJsEdges::mark` 在 GC 标记阶段把 native 根上的 callback/self 边暴露给 QuickJS，避免“native 持有 JS 回调但 GC 不可见”的泄漏/悬空。

---

## 五、析构强制清理

| 路径 | 语义 |
| --- | --- |
| 显式 `close()` | 完整、可等待、可 run_blocking、可向 JS 报告错误 |
| `~T`（如 `~Store` / domain `~Server`） | `noexcept`、不可等待、best effort、不泄漏 OS 资源；**不**调用业务 `close()` 方法 |

对 `Store`：`~Store` 在锁下置 `closed` 并 `db_.reset()`——与显式 `close()` 同类的连接释放，但是析构兜底，不是对 JS `close` 的二次编排。

对 domain `http::Server`：析构 / `dispose()` **只 post 非阻塞取消**到 owner executor，不 join、不 `stop()` executor；State 自持有直到 accept loop 与 sessions drain 至 Closed。

对 domain `process::Process`：析构 / `dispose()` **只 post 非阻塞**进程组 SIGKILL + 取消管道/定时器到 owner executor；不 join、不 `stop()` executor；不在完成前回写伪造的 exit/eof。State 自持有直到 `async_execute` reap 与 stdout/stderr drain 完成。

---

## 五·b、Server 生命周期（callback root + weak transport）

```text
JS Server
└── ClassHolder → shared_ptr<ServerNative>   # ClassBuilder async frames 需要 shared
    ├── on_request : qjs::OwnedValue          # callback root（native 拥有）
    ├── Runtime::Callbacks*                   # non-owning → Runtime::Impl 能力
    └── optional<http::Server>                # domain handle（值类型；State 自持有）

http::ServerHandler ──weak_ptr<ServerNative>──▶   # 无 self-root / 无 Native↔Server 环
```

规则：

1. **Callback root 由 native 状态拥有**，经 `ClassJsEdges` mark/release 暴露给 QuickJS GC；`Runtime::Callbacks` **不**拥有根。
2. **Transport handler 只捕获 `weak_ptr<ServerNative>`**；升级失败 / 已释放 → 不再调 JS。不把 `shared_ptr<ServerNative>` 或 JS 对象塞进 handler 以免自根环。
3. **显式 JS `close()`** = 业务生命周期（await drain + drop callback）。
4. **Finalizer** = 仅 release VM edge + drop holder；domain 析构只 post dispose。
5. **无 Host/Runtime JS-handle cleanup registry**。正确 JS/Application shutdown（显式 `Server.close()` 等）是前置条件；natural Asio drain 保证仍有 live 操作时不会 FreeContext。失败/不协作可诚实阻止 shutdown。

---

## 六、异步延长生命周期

异步操作（含 `Store.open` / `exec` / `run` / `query` / `transaction` / `close`，`Server.listen` / `Server.close`，以及 `Process.start` / `write` / `wait` / `terminate` / `close`）在 **call-entry** unwrap 后把 **non-null `shared_ptr<T>` 移入方法协程帧**；run_blocking/worker 只碰 C++，靠该 `shared_ptr`（或等价共享 state）续命。Process 域工作不经 `Runtime::Async::run_blocking`。

- **禁止**裸 `this` 跨 `co_await` / 跨 worker 边界。
- JS 对象被 GC、finalizer 已 drop holder 后，只要帧内仍持有 `shared_ptr`，进行中的 native operation 仍须安全完成；最后一份 `shared_ptr` 释放时才跑 `~T`。
- Inbound HTTP：domain `Server` 的 accept/session 状态机自持有共享 State；binding 侧 handler 只用 weak native，避免把 JS 生命周期钉在 transport 上。

---

## 七、并发模型在对象自身

不靠 Host 全局 registry/mutex：

- 同一 File / Store / Server / Process 的操作由该对象自己串行化或状态机约束。

`Store`：域内 `mutex_` 包住每一次 Database/sqlite 使用；多 worker 上的 run_blocking 作业也不会在同一连接上交错 SQL。

---

## 八、业务关闭顺序属于 JavaScript

C++ 不知道哪个 Server 是 API Server、哪个 Store 必须在 Process 之后关。
业务顺序只写在 script 的 `shutdown()` 里（例如先停 HTTP，再 `await store.close()`）。

---

## 九、Host shutdown 边界

Host 拥有：

```text
Runtime / io_context / QuickJS
ProcessRuntime + ProcessBudget
（Async/Callbacks/Script 为 Runtime::Impl 拥有的稳定能力，Host 只持非拥有指针）
ModuleCatalog / globals 安装
日志等进程级后端
```

约束：所有可能被 finalizer 触碰的 native 后端，必须活到 `JS_FreeContext` 完成之后。
`ProcessRuntime`、`ModuleCatalog` 因此排在 Runtime 之后销毁。能力随 Runtime/Impl 寿命存活；FreeContext 期间 composition 指针仍有效。

JS `shutdown()` **MUST** 显式关闭其创建的业务资源。Runtime **不**在 JS 失败或不协作时 force-close 引擎或替 JS 编排第二套业务关闭。未关闭资源与未完成 outstanding work 可诚实阻止 `main_io_.run()` 返回；仅 natural drain 结束后才 FreeContext，随后 finalizer release edge + drop holder，析构 RAII / post dispose 兜底。

---

## 十、资源对象不拥有 Runtime

```text
禁止：Server → Handler → shared_ptr<Runtime>
允许：weak_ptr / 非拥有回调 / 调用期临时取 Runtime::Async* 或 Runtime::Callbacks*
```

Domain class **不** include QuickJS（`vacps::storage::Store` 仅依赖 `Database` / 错误类型；`vacps::http::Server` 仅为 Asio/Beast transport）。Binding 负责参数、Promise、callback root、opaque。

---

## 十一、Binding 职责

```text
JS 参数解析与类型校验
同步：create_function / ClassBuilder 同步方法（当前 owner-thread QuickJS turn 内直接执行；无 per-callback Runtime 门闸）
异步：create_async_function / ClassBuilder::async_method /
     static_async_function → Runtime::Async（唯一 settle 入口）
回调入站：Runtime::Callbacks::call_and_await（native → JS → await sync/thenable）
异步方法：call-entry unwrap + 将 non-null shared_ptr<T> 移入方法协程帧
         （帧内 bind T&/T*/shared_ptr；不依赖 Start 被 Runtime::Async 续命）
自定义 Encode：不可用于 Task<void>；失败统一映射 runtime::Error
qjs::OwnedValue 表达 JS 值
opaque ClassHolder 管理（finalizer：可选 ClassJsEdges::release + delete holder）
ClassJsEdges：mark/release native 拥有的 JS 边（callback root 等）
```

Binding **不**负责业务关闭编排、全局资源登记、路径/访问策略、任务状态。

`vacps:store` 绑定侧额外约定：固定 `query(sql, params?, options?)`；`TransactionStep` 交叉字段（`expectedChanges` ↔ query，`maxRows`/`maxBytes` ↔ run）与空 `transaction([])` 在 decode 期同步拒绝；SQL INTEGER 全 int64 经 Number（safe integer）/ BigInt 往返。

`vacps:http` `Server` 绑定侧额外约定：构造期 root `onRequest`；handler 仅 weak native；显式 `close` 后 drop root；domain 为纯 transport（无 routes/JSON）。无 Runtime handle-cleanup 回退路径。

---

## 十二、Promise / 线程（与所有权交叉）

| 规则 | 内容 |
| --- | --- |
| JS 线程 | Runtime owner 唯一触碰 QuickJS |
| Worker | 只跑纯 C++；禁止 JS 拥有 RAII |
| 正向 Promise | 仅 `Runtime::Async` |
| 反向 await | `Runtime::await_value`（`Runtime::Callbacks` 复用）；同时监听调用方与 runtime-wide shutdown `stop_token`；owner/context/value 归属是不动态检查的 Narrow 调用方前置条件 |
| native→JS 回调 | `Runtime::Callbacks::call_and_await`；根由 binding 状态 + `ClassJsEdges` 管理；同上 owning 线程契约 |
| normal shutdown | 释放 daemon work_guard + runtime stop；`main_io_.run()` natural drain；引擎仅在 drain 后关闭；无 abandon / cleanup registry / deadline force-close |

详见 [`RUNTIME_LAYERING.md`](./RUNTIME_LAYERING.md)。

---

## 十三、一句话

> JavaScript 负责业务资源的正常生命周期；C++ 对象负责自身 RAII 兜底；`ClassBuilder` finalizer 经可选 `ClassJsEdges::release` 只做 VM edge 记账并丢 `ClassHolder`/`shared_ptr`，不替 JS 调用业务 `close()`；Host 拥有 Runtime 与 ModuleCatalog（Async/Callbacks/Script 由 Runtime::Impl 拥有），不建立业务资源 registry，也不替 JavaScript 编排资源关闭。
