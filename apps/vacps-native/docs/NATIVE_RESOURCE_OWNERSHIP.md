# JavaScript Native Resource Ownership 规范

## 核心原则

> JavaScript 创建并拥有业务资源；C++ 只实现能力和资源对象；Host 只管理共享基础设施与 JavaScript 引擎生命周期，不管理 JavaScript 创建的业务对象集合。

适用于：

```text
File
Store
Server
Process
以及未来所有有状态 native class
```

不适用于无状态函数或普通值对象，例如：

```text
crypto.sha256()
URL
URLSearchParams
TextEncoder
```

---

## 一、所有权模型

每次 JavaScript 创建资源时，创建独立的 C++ 对象：

```text
JavaScript object
└── opaque holder
    └── shared_ptr<C++ resource>
```

例如：

```text
JS File
└── shared_ptr<fs::File>

JS Store
└── shared_ptr<storage::Store>

JS Server
└── shared_ptr<http::Server>

JS Process
└── shared_ptr<process::Process>
```

资源对象不是由 Host 预先创建，也不通过全局整数 ID 暴露给 JavaScript。

---

## 二、Host 不维护业务对象 Registry

禁止为 JavaScript 创建的对象增加：

```text
FileRegistry
StoreRegistry
ServerRegistry
ProcessRegistry
NativeObjectRegistry
ResourceManager::stopAll()
```

Host 不应：

- 枚举所有 JavaScript 创建的对象；
- 替 JavaScript 决定业务资源的关闭顺序；
- 在 SIGTERM 时逐个寻找 File、Store、Server、Process；
- 依靠全局 registry 保持对象存活；
- 把对象生命周期重新集中到 ApplicationRuntime。

资源由 JavaScript 创建，因此正常关闭顺序也由 JavaScript 表达：

```ts
export async function shutdown(): Promise<void> {
  await server.close();
  await process.close();
  await store.close();
  await file.close();
}
```

Host 只负责调用这个统一生命周期入口：

```text
收到退出信号
→ 调用 JS shutdown()
→ 等待返回的 Promise
→ 关闭 QuickJS
→ 销毁共享基础设施
```

**说明：** `ProcessRuntime` 只提供 executor + 技术预算（`ProcessBudget`），**不**持有 `unordered_map` 进程表。每个 JS `Process` 对应 `shared_ptr<process::Process>`，直接拥有 child/pipes/timers。见 [`PROCESS_RUNTIME.md`](./PROCESS_RUNTIME.md)。

---

## 三、每个资源必须提供显式关闭 API

所有持有外部资源的 class 都必须提供显式生命周期方法。

```ts
interface ClosableResource {
  readonly closed: boolean;
  close(): Promise<void>;
}
```

具体对象：

```ts
await file.close();
await store.close();
await server.close();
await process.close();
```

`close()` 必须：

- 幂等；
- 有明确完成语义；
- 拒绝后续新操作；
- 等待或取消此前已提交的操作；
- 不会因为调用两次而报资源错误；
- Promise resolve 时，对象已达到公开 API 所承诺的关闭状态。

每个对象自己管理自己的子资源。

例如：

```text
Server
├── acceptor
└── sessions

Process
├── child process
├── stdin
├── stdout
└── stderr

Store
└── SQLite connection

File
└── file descriptor
```

`Server::close()` 可以管理属于该 Server 的 sessions，但 Host 不管理所有 Server。

---

## 四、QuickJS finalizer 只是对象生命周期兜底

QuickJS finalizer 的职责：

```text
JS 对象不可达
→ 释放 opaque holder
→ 释放 C++ resource 引用
→ C++ 析构执行最低限度清理
```

典型实现：

```cpp
struct FileHandle {
  std::shared_ptr<fs::File> file;
};

void file_finalizer(JSRuntime*, JSValue value) noexcept {
  auto* handle = static_cast<FileHandle*>(
      JS_GetOpaque(value, file_class_id));

  delete handle;
}
```

finalizer 不负责优雅关闭编排。

禁止在 finalizer 中：

```text
执行 JavaScript
创建 Promise
等待协程
调用 JS shutdown()
访问 ScriptRuntime service locator
阻塞等待网络或子进程
依赖其他 JS 对象
```

finalizer 只释放 C++ 所有权。

---

## 五、C++ 析构函数必须完成最低限度强制清理

显式 `close()` 负责正常优雅关闭。

析构函数负责遗漏 `close()` 时的强制清理：

```text
显式 close()
    完整、可等待、可报告错误

析构函数
    noexcept、不可等待、best effort、确保 OS 资源不泄漏
```

### File

```cpp
File::~File() {
  close_fd_noexcept();
}
```

### Store

```cpp
Store::~Store() {
  sqlite3_close_v2(db_);
}
```

### Server

```cpp
Server::~Server() {
  close_acceptor_noexcept();
  cancel_sessions_noexcept();
}
```

### Process

```cpp
Process::~Process() {
  // Defined policy: kill still-running child via Registry::close (SIGKILL if needed).
  terminate_or_detach_noexcept();
  close_pipes_noexcept();
}
```

析构行为必须提前定义清楚，特别是 Process：

```text
析构时 kill（当前实现：Registry::close → kill 未退出子进程）
```

不能含糊。

---

## 六、异步操作必须延长资源对象生命周期

异步操作不能捕获裸 `this`：

```cpp
[this] {
  return read_impl();
}
```

应捕获共享状态：

```cpp
auto self = shared_from_this();

co_await async_operation([self] {
  return self->read_impl();
});
```

或者：

```cpp
struct FileState;
std::shared_ptr<FileState> state_;
```

异步任务捕获 `state_`。

这样即使：

```text
JS object 被 GC
→ opaque holder 释放
```

正在执行的 native operation 仍可以安全完成。

最后一个 operation 结束后，C++ 对象才析构。

---

## 七、每个资源对象自己定义并发模型

不能依靠 Host 的全局 registry 或全局 mutex 解决对象并发。

### File

同一 File 的 read / write / truncate / flush / close 必须由该 File 自己串行化。

### Store

同一 Store 的 query / run / transaction / close 必须由该 Store 自己串行化。

### Server

同一 Server 的 listen / close / session state 必须由该 Server 自己的 executor/strand 管理。

### Process

同一 Process 的 start / read / write / wait / terminate / close 必须遵守明确状态机。

推荐统一状态：

```text
Created
Opening / Starting
Open / Running
Closing / Stopping
Closed
```

---

## 八、业务关闭顺序属于 JavaScript

例如业务明确要求：

```text
停止接收请求
→ 等待任务完成
→ 关闭数据库
→ 写入最终状态
```

这个顺序只能由业务脚本表达：

```ts
export async function shutdown(): Promise<void> {
  await server.close();
  await taskRunner.stop();
  await store.close();
}
```

C++ 不知道哪个 Server 是 API Server、Process 属于哪个 Task、Store 是否需要在 Process 后关闭等。因此 C++ 不应该猜测业务资源关闭顺序。

---

## 九、Host shutdown 的职责边界

Host 只管理它自己创建的共享基础设施：

```text
io_context
QuickJS runtime/context
native module bindings
filesystem executor
database executor
process backend
logging backend
```

标准 shutdown：

```text
1. 标记 Host stopping
2. 停止周期性调用 JS 的 host timer
3. 调用 JS shutdown()
4. 等待 shutdown Promise
5. drain QuickJS jobs
6. 释放长期 JSValue
7. JS_FreeContext
8. JS_FreeRuntime
9. 停止 native executors
10. 停止 io_context
```

关键约束：

> 所有 native backend 和 executor 必须活到 `JS_FreeContext` 完成之后，因为 QuickJS 释放对象时会执行 finalizer。

不能：

```text
先销毁 executor
→ 再释放 JSContext
→ finalizer 访问已经销毁的 backend
```

---

## 十、shutdown 超时或失败

Host 可以对 JavaScript `shutdown()` 设置总超时，但超时后不进行第二套业务关闭编排。

正确行为：

```text
JS shutdown 成功
→ 正常关闭 QuickJS

JS shutdown 失败或超时
→ 记录错误
→ 强制释放 QuickJS Context
→ finalizer 释放 holders
→ C++ 析构函数强制清理各自 OS 资源
→ 关闭 executors
```

不是：

```text
JS shutdown 失败
→ Host 查找所有 Server
→ 查找所有 Process
→ 查找所有 Store
→ 重新猜一遍业务关闭顺序
```

---

## 十一、禁止资源对象强持有 ScriptRuntime

资源对象不能通过依赖链重新拥有 JavaScript 引擎：

```text
Server
→ Handler
→ shared_ptr<ScriptRuntime>
```

必须使用 `std::weak_ptr<ScriptRuntime>` 或更窄的非拥有接口。

相同规则适用于 File / Store / Process / Server。

Binding 可以在方法调用期间临时访问 Promise bridge，但 domain object 不知道 QuickJS。

---

## 十二、Binding 的职责

Binding 只负责：

```text
JS 参数解析
JS 类型校验
JS Promise 创建
调用 C++ resource
C++ Result 转 JS value/error
opaque holder 管理
```

Binding 不负责业务关闭编排、全局资源登记、应用配置策略、访问控制、路径策略、任务状态、业务重试。

Domain class 不 include QuickJS。

---

## 十三、按对象应用

### File

```text
JS 创建 File → 自己持有 FD / 串行化 I/O
close() 正常关闭；析构关闭 FD
Host 不登记 File
```

### Store

```text
JS 创建 Store → 自己持有 SQLite / 串行化 SQL
close() 正常关闭；析构关闭 connection
Host 不登记 Store
```

### Server

```text
JS 创建 Server → 自己持有 acceptor 和 sessions
close() 停止该 Server；析构强制关闭该 Server
Host 不登记 Server
```

### Process

```text
JS 创建 Process → 自己持有 child/pipes（经 process backend）
close() 按公开语义终止或释放
析构：kill 仍在运行的子进程（Registry::close）
Host 不登记「业务 Process 集合」做 stopAll
```

---

## 十四、最终规范

```text
JavaScript
    拥有业务资源
    决定关闭顺序
    实现 shutdown()

QuickJS
    管理 JS 对象可达性
    不可达时调用 finalizer

Binding
    管理 opaque 和 Promise 转换
    不管理应用资源集合

C++ Resource
    实现资源能力
    实现显式 close()
    析构时强制释放自身资源
    不拥有 ScriptRuntime

Host
    创建共享基础设施
    调用 JS shutdown()
    释放 QuickJS
    最后销毁 executors/io_context
```

一句话：

> 任何 JavaScript 创建的 native 资源，都由 JavaScript 负责正常生命周期，由 C++ 对象负责自身 RAII 兜底；Host 不建立业务资源 registry，也不替 JavaScript 编排资源关闭。
