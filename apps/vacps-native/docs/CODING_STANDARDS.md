# vacps-native 开发规范

适用范围：`apps/vacps-native/`。与设计文档 `temp/native.md` 冲突时，以设计文档的产品语义为准，以本文件为代码写法约定。

目标：可读、可测、可静态链接、少惊喜。语言标准：**C++23**。

---

## 1. 语言与标准库优先

1. **C++23**（`CMAKE_CXX_STANDARD 23`，extensions OFF）。
2. **优先标准库**，再考虑 Boost / 第三方：
   - 字符串：`std::string` / `std::string_view` / `std::format`
   - 错误：`std::expected`、`std::optional`、`std::error_code`
   - 算法与范围：`<algorithm>`、`<ranges>`、`<numeric>`
   - 并发原语：`std::jthread` 等（注意：Agent 主路径仍以 Asio 单线程事件循环为准）
   - 文件系统：`std::filesystem`（带 `error_code` 重载，避免异常版）
3. Boost 用于：**Asio / Beast / Process** 等标准库尚未覆盖或明显更合适的领域；不要为“顺手”再拉 Boost 组件。
4. 第三方库版本 **锁死在 `cmake/Dependencies.cmake`**（URL + SHA256），禁止“随便最新”。

当前锁定（节选）：

| 依赖 | 版本 | 用途 |
|------|------|------|
| Boost | 1.91.0 | Asio / Beast（header-only）+ Process v2（编译 `libs/process/src`） |
| OpenSSL | Alpine apk（`openssl-dev` + `openssl-libs-static`） | TLS / Ed25519 / RAND / SHA-256；全静态 |
| SQLite amalgamation | 3.53.4 (`3530400`) | 本地存储 |
| spdlog | 1.17.0 | 日志 |
| nlohmann/json | 3.12.0 | JSON 编解码 |
| QuickJS | 2026-06-04 | JS runtime（RAII host，主线程 only） |
| GoogleTest | 1.17.0 | C++ 单元测试（`VACPS_BUILD_TESTS`） |

---

## 2. 错误处理：优先 `std::expected`

### 2.1 默认

- 可恢复 / 可向上传递的失败：**`std::expected<T, Error>`**（或项目内 `vacps::Result<T>` 别名）。
- 可有可无：**`std::optional<T>`**（“没有”不是错误时）。
- 系统调用 / 文件系统：**`std::error_code`** 或映射进 `Error`。
- **禁止**用“魔法返回值 + 全局 errno 风格”新造 API；C API 边界立即包一层。

### 2.2 `try` / `catch` 允许的情况

仅在下列边界使用，且应尽快转为 `expected` 或记录后退出：

1. **进程边界**（`main` 顶层、线程入口）防止未捕获异常直接 abort。
2. **第三方只抛异常的 API**（部分 Boost.Beast/Asio completion、stdlib 异常版 API）。
3. **析构 / `noexcept` 边界** 中必须吞掉或记录的异常（尽量避免在析构里做会失败的工作）。

业务逻辑、DB、配置解析、协议编解码：**不要**用异常做控制流。

### 2.3 错误类型

- 使用小型、可移动的 `vacps::Error`（至少含 `message`；后续可加 `code` / `category`）。
- 日志里写清上下文；不要只 log 再丢原因。
- 对外 HTTP JSON 错误体使用稳定字段：`error.code`、`error.message`（见路由约定）。

---

## 3. 所有权与拷贝

1. **能不拷就不拷**：
   - 只读视图用 `std::string_view` / `std::span`
   - 入参大对象：`const T&` 或按值 + `std::move`（sink 参数）
   - 出参优先返回值（RVO/NRVO）；避免 out-param 除非 C API 或缓冲复用
2. **sink 参数**（函数会取得所有权）：按值传递并 `std::move` 进成员。
3. **禁止**无必要的 `std::shared_ptr`；优先 `std::unique_ptr`、值语义、或明确共享模型（如 Beast session 的 `shared_from_this` 是合理例外）。
4. 容器传递：能 `std::move` 就 move；range 算法里注意迭代器失效。
5. 字符串拼接优先 `std::format`；固定结构 JSON 用 **nlohmann/json**，不要手写转义。

---

## 4. 算法与结构

1. 手写循环前先想：`std::ranges::*` / `std::find_if` / `std::transform` 等是否更短、更安全。
2. 早期 return，减少嵌套。
3. 单一职责：HTTP 路由、DB、进程、协议解析分层；header 里只放需要 inline 的薄封装。
4. 魔法数：命名常量。
5. **C++ 几乎无业务代码**：任务状态机、协议映射、领域表读写编排放在 QuickJS JS；C++ 只提供 DB 连接/`vacps:store` 能力、HTTP、进程、密码学等基础设施。
5. **不**在热路径做无界分配（读 body、日志字段）时注意缓冲复用；先正确后优化。

---

## 5. JSON

1. 编解码统一 **nlohmann/json 3.12.0**（`#include <nlohmann/json.hpp>`）。
2. 序列化：`json{...}.dump()`；解析：`json::parse` + 显式字段检查，失败返回 `expected`。
3. 不要对不可信输入用 `operator[]` 默认插入后再假设类型；用 `at` / `contains` / `get_to` 并处理异常或改用 `find`。
4. HTTP 响应 `Content-Type: application/json; charset=utf-8`。

---

## 6. 日志（spdlog 1.17.0）

1. 进程内统一 `vacps::log::*`；不要直接 `std::cerr` 打业务日志（CLI help/version 除外）。
2. 默认输出 **stderr**，级别由 `VACPS_LOG_LEVEL` / `--log-level` 控制。
3. 级别语义：
   - `trace` / `debug`：开发诊断
   - `info`：生命周期、监听、关键状态
   - `warn`：可恢复异常、降级
   - `error`：请求/会话失败、需关注
   - `critical`：进程无法继续
4. **不要**把正常客户端断开（EOF / end_of_stream）打成 error。
5. 敏感信息（密钥、token、完整私钥材料）禁止入日志。

---

## 7. 异步与线程（Asio）

1. v1 以 **单 `io_context` + 主线程** 为默认；不要随意加线程。
2. 协程：`asio::awaitable` + `co_spawn`；completion 优先 `as_tuple(use_awaitable)` 拿 `error_code`，再映射错误。
3. 跨线程投递必须 `asio::post` / strand；共享状态要说清楚。
4. 长时间 CPU / 阻塞（大 SQLite、压缩）后续用约定好的 offload，不得阻塞 accept 循环（见设计文档）。
5. **QuickJS**：同一 `JSRuntime` **禁止**多线程并发；所有 `vacps::js::*` 调用固定在 Asio 主线程。`Value` 仅 move；复制必须 `duplicate()`。Pending job 用 `Runtime::drain_jobs()`，禁止递归 drain。
6. **JS Promise ↔ Asio**：`Host::await_settled` 在 Promise pending 且无 microtask 时 **`co_await wait_progress()`**（取消 progress timer 唤醒），**禁止** spin 忙等。异步原语（`process.run`、HTTP client 等）完成路径必须 `Host::notify_progress()`。
7. **子进程**：只用 **Boost.Process v2**；禁止手写 `fork`/`fcntl`/`poll` 跑业务子进程。
8. **Host / modules**：Context opaque = `Host*`（无 `HostState`）。`vacps:host` 仅进程信息；HTTP/`store`/`process`/`fs` 各自模块。
9. **Native modules**：不暴露裸 `sqlite3*`；领域 SQL 写在 JS。

---

## 8. 资源与 RAII

1. 所有句柄（sqlite3、socket、文件、JS runtime）必须有 RAII；禁止裸 `new`/`fopen` 散落。
2. 特殊成员：持有资源的类型遵守 rule of five/zero；拷贝若无意义则 `= delete`。
3. 析构 **noexcept**；失败只记日志。

---

## 9. 构建与产物

1. 默认产物：Linux **x86_64 musl 静态 ELF**（Docker Alpine + Clang 22 + lld）。
2. 改 `Dockerfile` 才 `--rebuild-image`；日常只编代码。
3. 依赖通过 FetchContent 引入，**带 SHA256**。
4. 不在仓库提交 `build/` 产物与 `_deps` 下载缓存。
5. 警告：`-Wall -Wextra -Wpedantic`；新增警告尽量当错误修。

---

## 10. API 与安全默认

1. 默认监听 **`127.0.0.1`**；非 loopback 需 `VACPS_ALLOW_REMOTE_BIND=true`（已有逻辑保持 fail-closed）。
2. 路径与数据目录：`VACPS_DATA_DIR`，SQLite 文件名约定清晰。
3. 对外错误信息不泄露内部路径/SQL/堆栈；详细信息只进日志。

---

## 11. 代码风格（简表）

| 项 | 约定 |
|----|------|
| 命名 | 类型 `PascalCase`；函数/变量 `snake_case`；常量 `kCamel` 或 `ALL_CAPS` 宏少用 |
| 命名空间 | `vacps` / `vacps::http` / `vacps::log` |
| Header | `#pragma once`；include 顺序：本模块 → 项目 → 第三方 → 标准库 |
| 文件 | 实现放 `.cpp`；模板/薄包装可 header-only |
| 注释 | 写“为什么”和约束；不写复述代码的废话 |
| 提交 | 用户明确要求前不擅自 `git commit` |

---

## 12. 库文档：必须用 Context7

涉及 **库 / 框架 / SDK / API / CLI / 云服务** 的写法、配置、版本差异、迁移、官方示例时，**必须**走 Context7 MCP，禁止只靠模型记忆。包括（不限于）：QuickJS、Boost.Asio/Beast、spdlog、nlohmann/json、SQLite、GoogleTest、TypeScript、esbuild、CMake/FetchContent 行为等。

**不要用 Context7 的场合**：纯业务逻辑重构、从零写脚本、调试本仓库业务语义、代码审查、通用语言概念。

### 12.1 固定步骤

1. **`resolve-library-id`**：`libraryName` 用官方常见名（如 `QuickJS`、`Boost.Asio`）；`query` 写清要查什么。
2. **选 ID**：优先精确名、官方源、High/Medium 信誉、snippet 多、benchmark 高；用户点了版本则尽量用带版本的 ID。
3. **`query-docs`**：每个**单一概念**单独查一次（例如「模块加载」与「Promise job」分开）；不要一条 query 塞路由+鉴权+缓存。
4. **落地代码时以本次查出的 API/示例为准**；与训练记忆冲突时以 Context7 / 仓库内 `_deps` 头文件为准。

### 12.2 与本仓库的关系

| 主题 | 优先查 | 辅证 |
|------|--------|------|
| QuickJS 模块 / Eval / Promise / Job | Context7 `QuickJS` | `build/.../_deps/quickjs-src/quickjs.h`、`qjs.c` |
| Asio 协程 / timer / strand | Context7 Boost.Asio | 已链接的 Boost 头 |
| 其它锁定依赖 | Context7 对应库 | `cmake/Dependencies.cmake` 版本 |

### 12.3 反模式

- 为“方便”在 C++ 里 **内嵌业务/引导 JS**（如假模块名 + `globalThis` 注册），而不用官方模块 API（`JS_EVAL_FLAG_COMPILE_ONLY` → `JS_EvalFunction` → `JS_GetModuleNamespace`）。
- 用过时记忆写 API（例如旧版标志位、已改名符号），且未对照 Context7 / 当前头文件。
- 查库时跳过 resolve，凭感觉拼 library id。

改依赖相关代码时，在 PR/说明里可简短注明「已用 Context7 核对某某库/主题」。

---

## 13. 自检清单（改代码时）

- [ ] 失败路径是 `expected` / `optional` / `error_code`，而不是沉默 `bool`？
- [ ] 有没有多余拷贝？大对象是否 move / view？
- [ ] JSON 是否走 nlohmann，字段是否与协议一致？
- [ ] 日志级别是否合适？有无敏感数据？
- [ ] 是否引入无必要的线程或阻塞主循环？
- [ ] 涉及第三方库 API 时是否已用 **Context7**（§12）核对？
- [ ] 静态构建是否仍能通过 `docker/build.sh release`？

---

## 14. 示例

```cpp
// 好：可组合的错误
vacps::Result<Config> load_config(std::string_view path);

// 好：sink + move
void Session::set_name(std::string name) { name_ = std::move(name); }

// 好：JSON
nlohmann::json body{{"ok", true}, {"version", version()}};
return body.dump();

// 好：进程边界 catch
try {
  return run_agent(cfg);
} catch (const std::exception& e) {
  vacps::log::error("fatal: {}", e.what());
  return EXIT_FAILURE;
}

// 差：业务里 throw 当分支
// 差：手写 JSON 字符串拼接
// 差：shared_ptr 传来传去却无共享生命周期需求
```
