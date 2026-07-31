# Runtime 分层模型（快照）

> C++ 启动时创建共享运行环境并注册类型；JavaScript 调用构造函数或异步工厂时，才创建对应的 C++ 业务对象实例。

## 分层

| 层 | 内容 |
| --- | --- |
| ApplicationRuntime | 共享运行环境：`io_context`、`ScriptServices`、BootstrapConfig、ShutdownCoordinator、TickLoop（**无 C++ 路径策略层**） |
| QuickJS | ScriptRuntime、PromiseBridge、ModuleCatalog、GlobalApiInstaller、各类 Binding |
| JS 运行时创建 | `fs::File`、`storage::Store`、`http::Server`、`process::Process`、URL / Text\* |

共享环境：`ApplicationRuntime` 持有 `ScriptServices`（`data_dir` / `ca_bundle` / `environment` / `thread_pool` / `process::Registry` / `use_asio_file`）；`ScriptRuntime` 只持 engine + `shared_ptr<ScriptServices>`，经 `services()` 暴露给 bindings。

## Filesystem：C++ 纯 I/O

- **没有** C++ `PathSandbox` / allowlist / openat2 专属 policy 层（已删除，不要再加）。
- 路径策略只在 JS：`script/src/runtime/path-guard.ts`（MCP / 工具边界 allowlist）。
- C++：`resolve_path`（相对路径拼 `data_dir`）+ 纯 open/read/write。
- **`fs::File`** = 打开文件的 **handle API**：
  - open flags = Boost.Asio `file_base` bitmask（Linux 上与 open(2) / Node `O_*` 同值），**不是**字符串 OpenMode
  - 双后端：`probe_io_uring()` 成功 → Asio `random_access_file`；否则 pool + 私有 FD RAII
- 命名空间操作（mkdir / readDirectory / rename / exists / remove / stat）走 path 级 free helpers（`async_*` 卸载到 `thread_pool`）。内容 I/O 仅 `File`。

## BootstrapConfig（不是产品 Config bag）

C++ **唯一**允许的集中读 env：`BootstrapConfig::fromEnvironment()`。

| 归属 | 内容 |
| --- | --- |
| **typed 字段（C++ 进程/引擎）** | `data_dir`、`log_level`、`script_path`、`ca_bundle`、JS heap/stack/time budget |
| **EnvironmentSnapshot** | 全量 environ；`host.getenv()` 与 JS `loadConfig()` 产品策略都从这里读 |
| **禁止塞进 typed 字段** | `LISTEN_*`、`ALLOW_INSECURE_*`、产品 FS roots、CP URL/密钥等（JS 策略） |

不要把 `main.cpp` + `loadConfig()` 再镜像成一份 C++ 万能配置结构。

## 命名

- **不使用 `Native` 后缀**。业务类：`vacps::fs::File`、`vacps::storage::Store`、`vacps::http::Server`、`vacps::process::Process`。
- Binding：`vacps::quickjs::bindings::FileBinding` 等。关系：`JS File ↔ FileBinding ↔ fs::File`。

## 创建规则（create-at-JS-call）

启动时**只**创建共享环境 + 绑定定义，**不**预建 File / Store / Server / Process。

```ts
const file = await File.open(path, O_RDWR | O_CREAT, 0o644);
// or File.open(path, { flags: O_RDONLY })
const store = await Store.open(path, options);
const server = new Server(options);
const process = new Process(command, args?, options?);
```

JS 对象即资源句柄；不靠全局 ID registry（除非未来有跨对象寻址需求）。

## 产品 surface（vacps:fs 已定稿）

- **File** + `File.open`（flags = `O_*` / Asio bitmask）
- 命名空间：`mkdir` / `remove` / `rename` / `stat` / `exists` / `readDirectory`
- `O_*` 常量

无 free content helpers。其它模块（Store / Server / Process）与 HTTP transport 分层见 `NATIVE_MODULES.md`。
