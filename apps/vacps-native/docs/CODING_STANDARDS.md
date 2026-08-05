# vacps-native 开发规范（canonical）

## 0. 效力、范围与冲突

| 项 | 规则 |
| --- | --- |
| **效力** | 本文件是 `apps/vacps-native/` **唯一规范开发标准**（normative）。所有人工与代理改动 **MUST** 遵守。 |
| **范围** | 递归适用于 `apps/vacps-native/**`（C++、binding、script、tests、docker/CMake、本目录文档）。 |
| **代理合同** | 自动化代理另见**本项目级** [`../AGENTS.md`](../AGENTS.md)（即 `apps/vacps-native/AGENTS.md`，**不是**仓库根 `AGENTS.md`）。`AGENTS.md` 规定作业流程，**不**削弱本文件的技术约束。 |
| **架构现状** | 分层、模块 surface、资源所有权、进程运行时的**当前事实**见链接文档；本文件写**目标规则**与写法，**不**重复易变清单。 |
| **冲突** | 产品/协议语义以已发布契约与架构文档为准；代码写法、错误模型、构建与代理流程以**本文件**为准。实现尚未完全合规时，**MUST** 按本文件的目标规则演进，**MUST NOT** 把“现状未做到”写成永久例外。 |

### 0.1 管辖原则（Governing principle）

> **API 只在已文档化的前置条件（preconditions）成立时保证正确行为。**

- **MUST** 在信任边界校验外部/不可信数据，并处理**可预期**的运行失败（I/O、超时、取消、资源不可用等）。
- **MUST NOT** 为内部程序员误用（错误线程/上下文/生命周期/所有权、禁止的 use-after-close、非法 opaque 等）构建复杂可恢复路径。
- 内部 Narrow 契约的前置条件是**调用方义务**；被调实现 **MUST NOT** 用 `if` / `assert` / `abort` / `terminate` 重复判定这些条件。违约后无行为保证，可自然终止/崩溃/UB；**MUST NOT** 伪装成可恢复的 `Result` / JS `InternalError` 继续运行。
- 每个 API 的失败语义 **MUST** 服从其已选择的 **Wide / Narrow** 合同（§0.2），不得与合同矛盾。

### 0.2 API 合同设计（Wide / Narrow）— 规范性

在实现任何公共或跨层 API **之前**，作者 **MUST** 显式选择并文档化 **Wide** 或 **Narrow** 合同。合同决定“坏输入 / 非法状态”是**期望结果**还是**调用方/程序员错误**。

#### 定义

| 合同 | 接受的输入 | 语义上无效的业务数据 | 前置条件 / 违约语义 |
| --- | --- | --- | --- |
| **Wide** | 参数类型几乎可表示的**一切**输入；可依赖**普遍**语言/对象有效性（例如引用指代存活对象），**优先**由类型编码 | **期望结果**：按边界与合同报告（校验错误 / `Result` error / JS 异常 / Promise rejection 等） | **MUST NOT** 再附加有意义的业务/状态/线程/生命周期 precondition。若调用方还须建立**类型域之外**的此类条件 → **分类为 Narrow** |
| **Narrow** | **仅**满足显式 precondition 的输入 | 若该条件已列为 precondition，则**不是**可恢复业务错误 | 违反显式 precondition = **调用方/程序员错误**；实现不重复检查，且无恢复保证 |

- **Wide contract**：函数接受其参数类型几乎能表示的全部输入。可依赖**普遍**语言/对象有效性要求（例如引用指代存活对象），**优先**用类型编码；**MUST NOT** 再要求调用方建立有意义的业务/状态/线程/生命周期 precondition。**语义上无效的业务数据**是期望结果，须按边界与合同报告（`Result` / validation error / JS exception / Promise rejection 等），不得崩溃或 silent UB。若 API 需要类型域之外的额外语义/状态/线程/生命周期条件，**MUST** 归类为 **Narrow**（或拆成独立 API）。
- **Narrow contract**：函数只接受满足**显式前置条件**的输入；违反 precondition 是 caller/programmer error，**无**恢复保证。实现体不再动态复查前置条件；误用造成的自然 terminate、crash / undefined result 均可接受。

#### 强制规则

1. **每个 API MUST 在实现前选择 Wide 或 Narrow**，并在适用的公开头文件 / API 文档中写明。
2. **MUST NOT** 使用混合合同：同一 API **要么** Wide **要么** Narrow。Wide **MUST NOT** 通过“另附业务/状态/线程/生命周期 precondition”偷渡 Narrow 语义；需要调用方建立类型域之外的此类条件时，**MUST** 选择 Narrow（或拆成两个 API）。
3. **优先 Wide** 于外部 / 不可信边界：JS binding、CLI、协议/网络解码、配置加载、跨信任域入口（外部 JS 边界通常为 **Wide**）。
4. **优先 Narrow** 于内部函数：调用方已建立不变量（owner 线程、活 context、已校验结构、已持锁等）。
5. **强类型 MAY 编码 precondition**（例如非空 handle 类型、`not_null`、阶段枚举、immovable owner）。类型已静态排除的状态不必再写成动态可恢复错误；Wide 上的普遍对象有效性也**优先**这样编码，而不是写成业务 precondition。
6. 公开头文件 / API 文档（在适用处）**MUST** 使用紧凑模板字段：

```text
// Contract: Wide | Narrow
// Preconditions: ...
// Errors: ...
// Threading: ...
// Lifetime: ...
```

7. Code review / pre-submit **MUST** 核对合同已选择且与实现、测试一致（见 §11）。

#### 与失败分类的关系

- 失败分类（§1）**服从**已选合同，而不是相反。
- 同一表面条件在不同合同下归类不同：例如“业务上非法的枚举字符串”在 **Wide** 边界是校验/业务错误；在已保证只传入合法枚举的 **Narrow** 内部 API 上则是程序员错误。
- **MUST NOT** 在 Narrow API 上为“调用方本不该传入的值”实现完整产品级恢复路径，除非正在把该 API **重新定义为 Wide** 并更新文档。

---

## 相关架构文档（勿在本文件复制易变清单）

| 文档 | 内容 |
| --- | --- |
| [`RUNTIME_LAYERING.md`](./RUNTIME_LAYERING.md) | Runtime 门面、能力束、分层与源码树 |
| [`NATIVE_MODULES.md`](./NATIVE_MODULES.md) | 当前 `vacps:*` / globals JS surface |
| [`NATIVE_RESOURCE_OWNERSHIP.md`](./NATIVE_RESOURCE_OWNERSHIP.md) | JS 业务资源 vs C++ RAII / finalizer |
| [`PROCESS_RUNTIME.md`](./PROCESS_RUNTIME.md) | 子进程域与 `ProcessRuntime` |

---

## 1. 三路失败分类（Failure taxonomy）

所有公共 API、binding 与域库 **MUST** 按下列三类处理失败，且 **MUST** 与该 API 的 **Wide / Narrow** 合同（§0.2）一致。校验在**信任边界做一次**；调用链下游 **MUST NOT** 重复堆砌防御性检查。

### 1.1 外部 / 边界输入错误（通常 Wide）

| | |
| --- | --- |
| **来源** | 不可信 JS 参数、用户/配置/协议输入、Converter 可判定的类型/范围错误 |
| **合同** | 典型 **Wide** 边界；语义无效业务数据是**期望结果**，不是崩溃理由 |
| **处理** | **MUST** 映射为 `TypeError` / `RangeError`，或项目约定的**结构化校验错误**（在 binding/域边界一致）；纯 C++ Wide API 则用 `Result` / 校验错误类型 |
| **时机** | **MUST** 在副作用之前完成 decode/validate |
| **Narrow 对照** | 若 API 为 **Narrow** 且文档已要求调用方只传已校验值，则同类条件归 §1.3，**MUST NOT** 再假装成边界校验错误 |

### 1.2 可预期运行结果与失败（Expected operational / domain outcomes）

| | |
| --- | --- |
| **来源** | I/O 失败、超时、取消、对端关闭、资源暂时不可用、以及合同下作为**域结果**建模的业务结局（例如子进程退出状态、校验失败、域名否定结果等） |
| **正常域结果** | 合同将某结局定义为**期望的成功路径数据**时（例如返回子进程 exit code/signal），**MUST** 以**正常返回值**或 **fulfilled Promise**（或等价成功完成）交付，**MUST NOT** 仅因“非零/否定”就改走错误通道 |
| **合同内失败** | **仅当**所选合同将该结局定义为失败时，**MUST** 使用域 / runtime / binding 的 `Result` 错误（或 `vacps::Result` / `std::expected`）、JS 异常、或 **Promise rejection**（或等价的 `Task` 错误完成） |
| **禁止** | **MUST NOT** 用异常做常规业务控制流；**MUST NOT** 静默吞掉并返回魔法值；**MUST NOT** 把一切“可预期结局”一律写成 rejection / `Result` error |
| **子进程退出码** | **MUST NOT** 把“子进程非零退出”**普遍**写成 operational error。是否为错误取决于 **API 合同**：常见是期望的成功返回数据（exit code/signal）；仅在该 API 明确将某类退出定义为失败时才走错误路径。按合同建模，禁止一刀切。 |

### 1.3 内部契约违反（Internal contract violations / programmer misuse）

| | |
| --- | --- |
| **来源** | 错误 QuickJS 线程/context、错误生命周期阶段、所有权/opaque 违约、在禁止处 use-after-close、在 worker 持有 JS 句柄、**违反 Narrow API 的显式 precondition** 等**程序员错误** |
| **处理** | **MUST** 在 API 文档中写明 precondition，并由调用方建立；被调实现 **MUST NOT** 动态复查；违约后无行为保证 |
| **禁止** | **MUST NOT** 将此类错误建模为可恢复 `Result` 或向 JS 抛 `InternalError` 后继续当作健康运行时 |
| **与 Wide 的边界** | 无效业务数据**仅**在 **Wide**（或文档明确的期望失败）下是 §1.1/§1.2；同一条件在 **Narrow** 下可以是 §1.3 |

### 1.4 边界与异常（C ABI / noexcept）

- 下列边界上 **C++ 异常 MUST NOT 逃逸**：实际的 `extern "C"` 入口；以及作为函数指针交给 QuickJS 的 **C 回调**、**finalizer**、**模块 init** 等。这**不**意味着每个 QuickJS 回调的 C++ 声明都必须逐字写成 `extern "C"`——关键是该调用约定/ABI 边界期望无异常穿过。
- 合同内可报告失败 **MUST** 在边界内转为 `Result` / JS 异常 / Promise reject（期望域结果则按 §1.2 正常返回/fulfill），**不得**靠 C++ 异常穿出上述边界。
- **未预期**的 C++ 异常 **MUST NEVER** 穿越上述边界；在 noexcept / C ABI 边界上终止是**可接受**的。
- **MUST NOT** 要求“一律 catch-all → 映射为 `InternalError` → 继续跑”的毯式策略；那会把内部 bug 伪装成业务错误。
- Native `std::bad_alloc` 与 QuickJS C API 分配失败是不同路径：后者走引擎 `JS_EXCEPTION` / pending exception。正向 Promise / 共享异步异常边界上对 native OOM 的具体 fail-fast 行为以 `runtime_async.hpp`、`runtime/error.hpp`、`binding/error.hpp` 与 `RUNTIME_LAYERING.md` 的已实现说明为准；本文件不把尚未全库审计的边界写成全局 MUST。

---

## 2. C++23 语言与风格

### 2.1 语言

1. **MUST** 使用 **C++23**（`CMAKE_CXX_STANDARD 23`，extensions **OFF**）。
2. **SHOULD** 优先标准库，再使用已引入的 Boost / 第三方：
   - 字符串：`std::string` / `std::string_view` / `std::format`
   - 错误：`std::expected` / 项目 `Result`、`std::optional`、`std::error_code`
   - 范围：`<algorithm>` / `<ranges>` / `<numeric>`
   - 文件：`std::filesystem` 的 `error_code` 重载（避免异常版做控制流）
3. Boost **仅**用于 Asio / Beast / Process v2 等已采纳领域；**MUST NOT** 顺手再拉未立项的 Boost 组件。

### 2.2 所有权与 RAII

1. 所有句柄（socket、文件、sqlite、JS runtime/value 等）**MUST** 有 RAII；禁止散落裸 `new` / `fopen`。
2. 所有权优先级：**值语义 → `std::unique_ptr` →（仅在共享寿命真正需要时）`std::shared_ptr`**。Beast session 的 `shared_from_this` 等是合理例外。
3. **MUST NOT** 在 `co_await` 两侧通过裸 `this` 假设对象仍存活；协程帧 **MUST** 显式续命（例如 `shared_ptr` 成员、稳定 owner 引用 + 文档化 lifetime，或项目既有 token/weak 模式）。
4. JS 值拥有层**只有** canonical **`vacps::qjs::OwnedValue`** 与 **`vacps::qjs::ScopedCString`**。**MUST NOT** 再造第二套 JS 值 owner。
5. **Finalizer vs 显式 close**：
   - 显式 `close()` / `shutdown`：**业务**关闭路径（flush、drain、杀进程、释放连接等）。
   - `ClassBuilder` finalizer：**VM/生命周期兜底**（释放 holder / `shared_ptr`、可选 `ClassJsEdges::release`）；**MUST NOT** 假装代替 JS 编排完整业务关闭语义（详见 `NATIVE_RESOURCE_OWNERSHIP.md`）。
6. 被 C API / JS opaque 持有地址的 owner 类型 **MUST** immovable（且通常 noncopyable），保证地址稳定。
7. 析构 **MUST** 为 `noexcept`；析构中失败只记日志，不抛。

### 2.3 错误层转换

| 层 | 表示 |
| --- | --- |
| 域库纯 C++ | `Result` / `error_code` / 小型 `vacps::Error`（合同决定哪些是数据/错误） |
| Runtime 异步 | `Task` 错误完成 → **仅**经 `Runtime::Async` settle 为 Promise reject |
| Binding 同步 | Wide 边界输入错误 → JS `TypeError`/`RangeError`；合同内运行失败 → 抛映射后的 JS 异常 |
| Binding 异步 | 见 §4：Wide 准入/合同内可恢复失败可 pre-Promise 抛；合同内失败 post-Promise reject；Narrow 违约 ≠ 友好 JS 错误 |
| 日志/HTTP 对外 | 稳定字段；**MUST NOT** 向客户端泄露内部路径/SQL/堆栈 |

### 2.4 命名

| 项 | 约定 |
| --- | --- |
| 类型 | `PascalCase` |
| 函数 / 变量 | `snake_case` |
| 常量 | `kCamel`；宏少用 `ALL_CAPS` |
| 命名空间 | `vacps` / `vacps::runtime` / `vacps::binding` / `vacps::qjs` / … |
| 文件名 | **`snake_case`**（如 `runtime_async.hpp`、`process_runtime.cpp`） |

- 名称 **MUST** 表达语义；**MUST NOT** 堆砌冗余作用域词。
- **MUST NOT** 无正当理由使用 `Core` / `Impl` / `Manager` / `Adapter` / `Interface` 等后缀（既有 `Runtime::Impl` 等历史名不作为新代码样板随意扩散）。
- 嵌套配置类型 **SHOULD** 写成 `Application::Options`、`Foo::Options` 形式，而不是平行的 `ApplicationOptions` 森林——除非它是独立公共协议类型。

### 2.5 文件放置与兼容

1. 文件 **MUST** 放在**拥有该职责的层**（见 `RUNTIME_LAYERING.md` 源码树）：`qjs/`、`binding/`、`runtime/`、`host/`、`modules/`、域目录、`script/` 等。
2. **MUST NOT** 在正式兼容承诺之前添加兼容包装、无用别名、或双轨重复实现。
3. **MUST NOT** 为“以后也许兼容 Node/N-API”预先铺垫 API。
4. Header **MUST** 尽量**自洽**（self-contained）：包含使用本头文件所必需的声明/头；**MUST NOT** 依赖“碰巧被其它头先 include”的传递顺序。

### 2.6 Include、格式、注释与具体编码风格

**文本与格式（C++ / 项目源，Markdown 另有说明）：**

| 项 | 规则 |
| --- | --- |
| 编码 | **UTF-8** |
| 换行 | **LF only**（不使用 CRLF） |
| 文件尾 | **MUST** 以最终换行（final newline）结束 |
| 缩进 | **空格**，**2-space** indent；**MUST NOT** 使用 Tab 缩进 |
| 行尾空白 | **MUST** trim trailing whitespace（Markdown 中有意的硬换行例外除外） |
| 行宽 | **优先**保持约 **100 列**可读性；为对齐、长字符串、宏或清晰度 **MAY** 正当超列或提前断行 |
| 大括号 | **MUST** 匹配既有 C++ 代码的 opening-brace 风格（与周围文件一致） |
| clang-format | **当前仓库无**项目级 clang-format 配置；**MUST** 匹配周围代码与本表规则；**MUST NOT** 借机做无关的大规模重排/全树格式化 |

**C++ 具体风格：**

1. Header：**`#pragma once`**。
2. Include 顺序：**本模块 → 项目内 → 第三方 → 标准库**（各组内保持稳定、可读）。
3. **`[[nodiscard]]`**：**MUST** 用于忽略会导致错误被吞的返回值（尤其 `Result` / `status` / 分配/注册结果等）。
4. **`const`**：在语义上不修改的地方 **MUST** 使用（含 const-correct 成员、只读引用/指针）。
5. **`auto`**：**仅**在类型显而易见或写出反而损害清晰度时使用；**MUST NOT** 为炫技而模糊接口类型。
6. 注释写**为什么**、**合同（Wide/Narrow）**与**前置条件**；不写复述代码的废话。
7. 标识符、注释、提交说明中的代码面向文本：**English**。
8. TypeScript/JSON 的 Prettier：**single quotes**、**trailing commas**、**printWidth 100**（见 monorepo `.prettierrc.json`）。
9. 用户明确要求前 **MUST NOT** `git commit`。

### 2.7 日志（spdlog）

1. 进程内统一 `vacps::log::*`；业务路径 **MUST NOT** 直接 `std::cerr`（CLI help/version 除外）。
2. 默认 stderr；级别由 CLI `--log-level`（`trace|debug|info|warn|error|critical|off`）。
3. 正常客户端断开（EOF / end_of_stream）**MUST NOT** 打成 `error`。
4. **MUST NOT** 记录密钥、token、私钥材料。

---

## 3. Asio 与 QuickJS 并发模型

目标规则如下。**标准描述的是期望契约**；**MUST NOT** 声称当前源码已完全合规。

### 3.1 线程与亲和性

1. **一个 owner 线程**驱动主 `io_context` 并触碰 QuickJS；同一 `JSRuntime` **MUST NOT** 多线程并发使用。
2. **Owner-thread / context / lifecycle 前置条件**由 Runtime 入口的调用方建立。Narrow 实现体不再重复检查这些前置条件。
3. **同步 binding 回调**（Converter/`create_function`/Class 方法等由 QuickJS 在 owner 上同步调用的路径）在**当前 QuickJS owner-thread turn 内直接执行**。进入该 turn 时，线程与活 context 等不变量**已经**由 Runtime 入口建立。
4. **MUST NOT** 引入“Runtime 同步门闸”或在每个同步 binding 回调里重复堆砌同样的 owner/context/phase 防御性检查（与 §1“信任边界一次”一致）。程序员误用 owner/context/lifetime 是 **Narrow** 违约（§1.3），**MUST NOT** 翻译成每个回调上的可恢复 `Result` / 友好 JS `InternalError`。
5. 跨线程投递 **MUST** 使用 `asio::post` / 既有 `Runtime::post_to_owner`（或等价 owner 投递）；**MUST NOT** 从非 owner 直接调用 QuickJS API。

### 3.2 Promise 与 run_blocking

1. **`Runtime::Async` 是唯一的** C++→JS Promise 生命周期拥有者与 settle 入口，并提供内部 `run_blocking`（worker 上的纯 C++ 阻塞工作）。需要单资源 FIFO 时使用其 per-resource `SerialWorker` strand；不得用 worker mutex 竞争冒充提交顺序。
2. Workers **MUST** 为纯 C++：**MUST NOT** 携带 `JSContext*` / `JSValue` / `qjs::OwnedValue` / `ScopedCString` / `PromiseCapability`。
3. Worker 完成后 **MUST** 经 Asio completion 回到 owner 再碰 JS（协程内直接
   `co_await post(fn, worker_executor)`，使用默认 deferred completion token；不得额外套
   `use_awaitable` 桥接协程）。
4. **MUST NOT** 阻塞 owner 循环（accept、job pump、JS 执行）。长 CPU / 阻塞 I/O 走 `Runtime::Async::run_blocking` 或域内 Asio 异步路径（如 Process 域既有规则）。
5. 取消与寿命：**MUST** 有显式规则（runtime-wide 与调用方 `stop_token`、shutdown phase、`shared_ptr` 续命；见资源与 Process 文档及 `RUNTIME_LAYERING.md` natural drain）。正常关机：**唯一** runtime-wide `std::stop_source` 请求协作取消，释放 daemon work_guard，由 `main_io_.run()` 按 Asio outstanding work 自然排空；QuickJS **仅**在 drain 之后关闭。**MUST NOT** 实现 AsyncScope / ReverseAwaitTracker / JS shutdown cleanup registry / Promise 或 reverse-await abandon / shutdown deadline / late-completion drain，或在正常路径调用 `io_context::stop()` / `restart()` / `poll()` 来“赶”关机。JS **MUST** 在 shutdown 导出中显式关闭业务资源；失败或不协作 **MAY** 诚实阻止 shutdown 完成。排队中的单个 post **不能**被 Asio 移除；dequeued 时若已 stop 则跳过 callable。运行中仅当 callable 接受 `stop_token` 时可协作取消。**MUST NOT** 在 `co_await` 后假设未续命的栈对象仍有效。
6. 提交到 `thread_pool` 的外层函数 **MUST** 为 `noexcept` 并将任务异常存为 `exception_ptr` 数据（Asio：worker 上未捕获异常 → `std::terminate`）。

### 3.3 反向 await 与回调

- `Runtime::await_value` / `Runtime::Callbacks`：事件驱动 then 反应；**MUST NOT** 轮询 `JS_PromiseState` 或 spin。
- 在 owner 外携带 live `OwnedValue` 调用 owning API → **内部契约违反**（§1.3），不是普通 `Result`。

### 3.4 子进程

- 域库子进程 **MUST** 使用 **Boost.Process v2**（经项目 `ProcessRuntime` / `vacps:process` 路径）。
- **MUST NOT** 手写 `fork`/`fcntl`/`poll` 跑业务子进程。
- 退出码 / signal 的错误与否 **MUST** 按该 API 的 Wide/Narrow 合同建模（§0.2、§1.2），禁止一律当作 operational error。

### 3.5 Opaque 约定（现状锚点）

以架构文档为准，当前约定包括：

- `JS_SetContextOpaque` = `vacps::Runtime*`（若保留该槽；Host/modules **MUST NOT** 覆盖）
- `JS_SetRuntimeOpaque` = `RuntimeModuleComposition*`
- Module loader opaque = `ModuleCatalog*`（catalog immovable）

---

## 4. Binding DSL

1. **Converter** 在 JS 信任边界做校验与类型转换（典型 **Wide**）；**MUST** 在任何副作用（打开文件、listen、spawn、写库）**之前**完成 decode。
2. **同步**路径：在 owner 上、**当前 QuickJS turn 内直接执行**（§3.1）；直接 decode/invoke/encode，无 per-callback Runtime 门闸；输入错误抛 `TypeError`/`RangeError`；合同内失败按 §1.2 映射为 JS 异常；期望域结果正常返回。
3. **异步**路径：
   - **Promise 创建前**可同步判定的 **Wide** 输入/准入错误，或其它**合同定义为可恢复**且在创建 Promise 前可检测的失败 → **同步抛**，不创建 Promise；
   - **Promise 创建后**的合同内失败 → **reject**（仅经 `Runtime::Async`）；
   - **违反 Narrow precondition**（含线程/生命周期/所有权等）仍是 **§1.3 程序员误用**：**MUST NOT** 改写成友好的同步 JS 异常/校验错误来“恢复”。
4. **`ClassBuilder`**：opaque/`ClassHolder` 所有权、finalizer 与显式 `close` 的分工见 §2.2 与 `NATIVE_RESOURCE_OWNERSHIP.md`。
5. 有状态实例需要跨异步帧续命时，使用既有 `shared_ptr<T>` holder 模型；**MUST NOT** 发明第二套 lifetime 协议。
6. **MUST NOT** 引入 Node / N-API 兼容层，除非产品明确立项要求。
7. 导出给 JS 的 API **MUST** 文档化 Contract（通常 Wide）、Errors、Threading、Lifetime（§0.2 模板）。

---

## 5. TypeScript（`script/`）

1. **MUST** 保持 `script/tsconfig.json` 的严格标志（`strict`、`verbatimModuleSyntax`、`noUncheckedIndexedAccess`、`exactOptionalPropertyTypes`、`useUnknownInCatchVariables`、`noImplicitOverride`、`noImplicitReturns`、`noFallthroughCasesInSwitch` 等）。
2. **ESM only**（`package.json` `"type": "module"`）；类型导入 **MUST** 使用 `import type`。
3. **MUST NOT** 无书面理由使用 `any`；外部输入用 `unknown` + 收窄（或 zod / `@vacps/contracts`）。
4. 格式：**Prettier** — single quotes、trailing commas、`printWidth` 100。
5. **产品逻辑、路由、策略、业务资源关闭顺序**由 JS 拥有（`initialize` / `shutdown` 等）；C++ 不替 JS 猜产品关闭顺序。
6. 运行时是 **QuickJS**，**MUST NOT** 使用 Node-only API（`node:*`、`fs`、`process.exit` 等）于产品脚本路径。
7. 测试放在 **`script/tests`**（Vitest）；native 声明在 **`script/types`**，**MUST** 跟踪真实 JS surface，不声明未实现 API。
8. 与 monorepo `@vacps/contracts` 的契约字段保持一致。

---

## 6. 架构边界

| 侧 | 职责 |
| --- | --- |
| **C++** | Runtime、binding、transport、域能力（crypto/fs/http/store/process/…）；以及为**正确性所必需**的 transport / resource / domain **状态机** |
| **JS** | 产品策略、路由、工作流、业务编排与业务资源关闭顺序 |

补充规则：

1. **Native `vacps:fs`** 是**纯 I/O**能力：允许绝对路径；**没有**路径 allowlist / 文件系统沙箱。
2. **`applyPatch` 等工作区范围**是**功能级 JS 规则**，**不是**通用 FS 沙箱。**MUST NOT** 在缺少需求时发明全局安全策略。
3. **MUST NOT** 发明未文档化的安全/bind/监听策略或“安全默认”。实际产品 bind 默认与策略属于 **JS / 产品文档**，不在本文件臆造。
4. 当 transport / resource / domain 状态机对机制正确性为必要时，**C++ MUST** 实现它们（连接生命周期、协议读写、进程会话、队列/背压、关机阶段等）；实现结构 **MAY** 按合适方式选择。**JavaScript** 拥有产品策略、路由、工作流与业务编排。**MUST NOT** 声称“C++ 几乎没有状态机”或把必要的域状态机推给 JS。
5. 领域**产品表语义**与面向用户的协议映射仍放在 JS；C++ 状态机服务于机制正确性，不窃取产品策略所有权。
6. **MUST NOT** 向 JS 暴露裸 `sqlite3*` 等 C 句柄。

---

## 7. 依赖

### 7.1 允许的依赖（实际在用）

版本与获取方式 **MUST** 以 `cmake/VacpsDeps.cmake`、CMake options、以及 `Dockerfile` / 镜像内容为准（URL + SHA256 或 apk 钉死）。**MUST NOT** 在代码或文档中把未使用的库写成依赖。

| 依赖 | 用途（摘要） |
| --- | --- |
| **Boost** Asio / Beast / Process v2 | 事件循环、HTTP、子进程 |
| **QuickJS** | JS 引擎（owner 线程） |
| **mimalloc** | QuickJS 专属 backing heap + 全局 C++ `new/delete`（TSan 配置由 sanitizer 拦截器接管）；不覆盖 C `malloc/free` |
| **OpenSSL** | TLS / 加密原语 |
| **SQLite** amalgamation | 本地存储 |
| **spdlog** | 日志 |
| **Ada** | WHATWG URL |
| **simdutf** | TextEncoder / TextDecoder |
| **liburing** | `vacps:fs` Asio 文件后端（epoll 仍为网络默认 reactor） |

- **没有** nlohmann/json，也 **MUST NOT** 重新引入，除非经过正式依赖评审并写入 `VacpsDeps`。
- **MUST NOT** “随便最新”；改版本 = 改钉扎输入并走完整验证。

### 7.2 Context7（库文档）

涉及 **库 / 框架 / SDK / API** 的写法、配置、版本差异、官方示例时，**MUST** 使用 Context7 MCP，禁止只靠模型记忆。

包括但不限于：QuickJS、Boost.Asio/Beast/Process、spdlog、SQLite、OpenSSL、Ada、simdutf、TypeScript、esbuild、CMake/FetchContent。

**不要用 Context7**：纯本仓库业务重构、代码审查、通用语言概念、与第三方 API 无关的编辑。

步骤：

1. `resolve-library-id`（官方常见名 + 清晰 query）
2. 选择高信誉 / 官方 / 版本匹配的 ID
3. `query-docs`：**单一概念**每次一查
4. 落地以本次文档与仓库 `_deps` 头文件为准

---

## 8. 测试与验证（与风险成比例）

1. 验证 **MUST** 与改动风险成比例；公共契约、生命周期、并发与资源释放优先。
2. JavaScript 单元行为使用 `script/tests`；native 集成行为编译产品二进制后，通过真实
   JavaScript 入口运行验证。项目不维护独立 C++ 单元测试目标。
3. **MUST NOT** 添加**唯一目的**是“从内部契约违反 / Narrow precondition 违反中恢复”的
   验证路径（§1.3 允许崩溃，不要求可恢复行为验证）。
4. 涉及 **内存 / 寿命 / 并发** 的改动 **MUST** 在相关配置下编译并运行相关 JavaScript：
   - **ASan + UBSan**（`docker/build.sh asan`）
   - 和/或 **TSan**（`docker/build.sh tsan`）
5. 完整业务验证见 §9 的 `release`；**不得**用 compile-only 或 `--native-only` 冒充全量验证。
6. **`--native-only`** 只证明产品二进制可编译/链接，**不是** full validation。

---

## 9. 编译与构建（不可协商）

| 规则 | 要求 |
| --- | --- |
| **唯一编译入口** | **MUST** 仅通过 `apps/vacps-native/docker/build.sh` |
| **禁止** | 主机本地直接 CMake/Ninja、临时 `clang++`、或其它 ad hoc 编译当作正式验证 |
| **并行度** | **最多 4 核**；`CMAKE_BUILD_PARALLEL_LEVEL=4` 或更低（脚本默认更保守并硬封顶 4） |
| `release` | **完整验证**（script 构建/测试 + native 构建 + 生命周期 smoke） |
| `asan` / `tsan` | 内存/寿命/并发相关改动的要求配置 |
| `--native-only` | **仅编译**产品二进制；**MUST NOT** 报告为 full validation |
| 主机运行 | 允许运行**由该脚本产出**的二进制（如 `--version` 或本地冒烟） |
| 镜像 | **仅** Docker/工具链变更时 `--rebuild-image`；日常代码改动复用镜像 |
| 产物 | **MUST NOT** 提交 `build/`、`_deps` 缓存等 |

警告级别：`-Wall -Wextra -Wpedantic`（及 CMake 中已有 flag）；新增警告 **SHOULD** 当错误清理。

---

## 10. 可选 Pi + Grok 委派与强制提示词模板

**Codex** 对核心框架工作直接负责计划、架构、实现与审查。核心框架包括 Runtime、QuickJS 集成、Binding DSL 基础、allocator / engine 所有权、并发与异步控制流、shutdown / lifetime 语义，以及影响这些领域的构建系统改动；这些工作 **MUST NOT** 委派给 Pi/Grok 实现。

Pi/Grok 不是默认实现路径，只可用于 Codex 已经固定架构与合同后的、显式委派且边界清晰的非核心任务，例如机械性改动、叶子模块或文档同步。Codex **MUST** 在验证前审查所有委派 diff。任何实际委派给 Pi/Grok 的编码/编辑任务 **MUST** 在提示词中显式包含下列门闸。**缺门闸 = 不得编辑。**

### 10.1 必含门闸（复制使用）

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
```

### 10.2 其它流程规则

- 用户/上游提示若**省略**本门闸：代理 **MUST** 在编辑前停止，重述并应用门闸。
- **MUST NOT** 在未请求且无已发布兼容契约时创建兼容脚手架或双轨实现。
- **MUST NOT** 擅自 `git commit` / push。
- 保护 dirty worktree：只改任务范围内的文件。

---

## 11. 提交前清单（pre-submit）

### 11.1 完整清单

- [ ] **合同**：每个新/改 API 已选择并文档化 **Wide 或 Narrow**（§0.2）；**无混合合同**（Wide 无额外业务/状态/线程/生命周期 precondition；需要则 Narrow 或拆分 API）
- [ ] **失败路径**符合三路分类（§1）且**服从**所选合同；无效业务数据仅在 Wide 下可恢复；**合同定义为期望域结果**的结局走正常返回值 / fulfilled Promise，不误用 rejection / `Result` error
- [ ] **信任边界校验一次**；无下游重复防御噪音；同步 binding **无**多余 per-callback Runtime 门闸
- [ ] **程序员误用 / Narrow 违约**：无恢复保证路径；未做成可恢复 `Result` / 健康路径 `InternalError`；未把 Narrow 违约改写成友好同步 JS 错误
- [ ] 无 C++ 异常穿过 QuickJS 回调/finalizer/模块 init 与 `extern "C"` 边界；合同内失败为 `Result` / JS 异常 / reject；期望域结果可为正常值 / fulfill
- [ ] **所有权 / 线程 / 寿命**：RAII、`OwnedValue`/`ScopedCString` only；无非法 `shared_ptr`/裸 `this` 跨 `co_await`；QuickJS 仅 owner 线程；worker 无 JS 句柄
- [ ] Promise 仅经 `Runtime::Async`；同步 binding **无** per-callback Runtime 门闸
- [ ] Binding：先 decode 后副作用；async 仅对 Wide 准入/合同内可恢复失败 pre-Promise 同步抛；Narrow 违约不转为友好 JS 错误；post-Promise 经 `Runtime::Async` reject
- [ ] JS：严格 TS、无 Node-only API、产品策略/路由/工作流由 JS 编排；C++ 保留必要域状态机
- [ ] **无兼容脚手架**（无已发布契约则不加双轨/别名包装）
- [ ] **层 / 文件 / 命名**正确；`snake_case` 文件名；无冗余 `Core`/`Manager`/…
- [ ] 未发明 FS 沙箱/额外安全策略；**无 nlohmann/json**；无未立项依赖
- [ ] 风格：UTF-8、LF、final newline、2-space、无脏 trailing whitespace；匹配周围代码
- [ ] **测试**与风险成比例；库 API 已用 **Context7**（如适用）
- [ ] 构建仅 `docker/build.sh`，**≤4 核**；按风险选择 `release|asan|tsan`
- [ ] 未把 `--native-only` 当成 full validation
- [ ] **验证报告**含命令、结果、跳过项
- [ ] **无无关编辑**；无未请求的 `git commit`

### 11.2 精简 pre-submit 核对（每次改动必过）

1. Contract chosen & documented (Wide|Narrow only; no mixed contract; preconditions/errors/threading/lifetime as applicable)
2. Boundary validation once; taxonomy follows contract; expected domain outcomes are normal values/fulfills when the contract says so
3. No recovery paths for programmer misuse / Narrow violations (including no friendly pre-Promise JS errors for Narrow breaches)
4. Ownership, thread affinity, lifetime correct
5. No compatibility scaffolding without a released contract
6. Correct layer, file placement, and naming
7. Verification proportionate to risk; native integration runs real JavaScript through the product binary
8. Context7 used for dependency/API questions when applicable
9. Docker-only build via `docker/build.sh`, **max 4 cores**
10. Verification report written; unrelated dirty work preserved; no drive-by edits

---

## 12. 简短示例

```cpp
// Contract: Wide
// Preconditions: none beyond type-representable arguments
// Errors: Result error on I/O or invalid config content
// Threading: any
// Lifetime: caller owns returned Config
vacps::Result<Config> load_config(std::string_view path);

// Contract: Narrow
// Preconditions: called on QuickJS owner thread; ctx is the live runtime context
// Errors: none for thread/context misuse (caller precondition; unchecked)
// Threading: owner only
// Lifetime: view valid for the current turn only
[[nodiscard]] std::string_view current_module_name(JSContext* ctx) noexcept;

// Good: sink parameter
void Session::set_name(std::string name) { name_ = std::move(name); }

// Good: nested options
struct Application {
  struct Options {
    std::string data_dir{"data"};
  };
};

// Good: exit status as data when the API contract says so
struct ProcessOutcome {
  int exit_code;
  bool exited_normally;
};

// Boundary: map expected errors; do not catch-all bugs into InternalError
// Internal contract (wrong thread with live OwnedValue): unchecked precondition

// Bad: business control flow via throw
// Bad: shared_ptr everywhere without shared lifetime
// Bad: run_blocking capturing qjs::OwnedValue or JSContext*
// Bad: host-local cmake as "validation"
// Bad: second JS value owner type beside qjs::OwnedValue
// Bad: per-callback Runtime gate or owner/context checks after Runtime entry already established
// Bad: claiming C++ must not implement transport/resource state machines
```

```ts
// Good: product owns shutdown order
export async function shutdown(): Promise<void> {
  await server.close();
  await store.close();
}

// Bad: Node fs in product script
// Bad: any without justification
```
