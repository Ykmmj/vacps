# Terminal Runtime

`terminal.*` 是在线交互会话，不是后台任务的另一种名字。

| Surface                       | 生命周期                 | I/O                  | 持久化 / 重试 | 适用场景                     |
| ----------------------------- | ------------------------ | -------------------- | ------------- | ---------------------------- |
| `command.exec` / `shell.exec` | 单次请求等待最终结果     | 独立 stdout/stderr   | 否            | 短命令                       |
| `tasks.*`                     | 队列拥有，允许跨请求运行 | 持久化任务输出       | 是            | 无人值守、调度、重试、审计   |
| `terminal.*`                  | 在线临时会话             | PTY 合并流，双向交互 | 否            | shell、REPL、需要 TTY 的程序 |

共同的“启动进程”只是实现细节，不能合并三者的生命周期契约。

## 分层

```text
MCP terminal.*
  → control-worker BackendClient
    → native script /terminals/*
      → TerminalSessions（ID、容量、idle/retention）
        → vacps:terminal Terminal（QuickJS binding）
          → vacps::terminal::Terminal（Asio + UNIX 98 PTY）
```

- C++ 域层只拥有 PTY、子进程组、Asio I/O、退出状态与有界字节缓冲。
- QuickJS binding 只做类型转换和 Promise/协程桥接。
- script 产品层拥有 `terminal_id`、会话容量、空闲回收、HTTP DTO 和 UTF-8 文本边界。
- control-worker 只做 schema、签名代理和 MCP tool 暴露。

## PTY 建立

Linux 后端使用 UNIX 98 PTY：

1. `posix_openpt` 创建 nonblocking/CLOEXEC master。
2. `grantpt`、`unlockpt`、`ptsname_r` 后打开 slave。
3. child 中 `setsid`、`TIOCSCTTY`、`TIOCSPGRP`，将 slave `dup2` 到 fd 0/1/2。
4. parent 将 master 交给 `asio::posix::stream_descriptor`，异步读取和串行写入。
5. child 是独立 process group；signal/close 面向整个 group，而不是只处理 leader。

stdout/stderr 在 PTY 层天然合并。任何声称能在同一 PTY 中可靠恢复两条独立流的 API 都是不真实的。

## 数据和游标

- 域层保存原始字节，以单调递增的绝对 byte offset 定址。
- 缓冲区有固定上限；旧数据被淘汰后，落后的读取返回 `dropped=true` 和新的 `available_from`。
- 产品 `terminal.read` 只返回 UTF-8 `content`；`next_cursor` 仍是十进制字节偏移。
- 非 EOF 读取不会把多字节 UTF-8 字符切成两个响应；不完整尾部留给下次从同一字节位置读取。
- `max_bytes` 最小为 4，确保任意一个 UTF-8 code point 都能取得进展。
- ANSI escape sequence 属于终端文本的一部分，不在 transport/domain 层删除。

当前 MCP 数据面使用 cursor + bounded long-poll。未来若 UI 需要低延迟连续渲染，可以在相同 TerminalSessions 生命周期之上增加 WebSocket transport；不改变 PTY 域对象，也不引入另一套 process session。

## 生命周期

```text
Created → Starting → Running → Exited | Signaled | TimedOut
                              ↘ Closing → Closed
```

- `waitForExit` 完成条件是 child 已 reap 且 PTY 已 EOF，避免丢掉退出前最后一段输出。
- `resize` 使用 `TIOCSWINSZ`，由内核向前台进程组产生 `SIGWINCH`。
- `close` 先发 `SIGHUP`，宽限期后发 `SIGKILL`，再等待 reap/EOF。
- 显式关闭后 session 立即从产品 registry 移除；未关闭的已完成 session 只短期保留。
- Agent 重启不会恢复 terminal；需要恢复、调度或重试的执行必须使用 `tasks.*`。

## 资源边界

- 最多 64 个产品 terminal session。
- terminal 和 process 共享 `ProcessBudget` 的进程数与全局缓冲预算。
- 单会话滚动缓冲默认 4 MiB，可配置 64 KiB..16 MiB。
- 单次 read/write 最大 1 MiB；read long-poll 最大 60 秒。
- 默认 idle timeout 30 分钟；产品层定时关闭无人使用的活会话。

## 契约

- C++ 内部接口是 narrow contract：owner executor、状态和参数范围由调用方保证，不重复做业务防御。
- QuickJS/HTTP/MCP 是 wide boundary：在产生副作用前验证路径、尺寸、枚举和数值范围。
- 不提供 environment 注入、持久化、自动重连、自动重试或伪造的 stdout/stderr 分离。

参考：[Linux PTY overview](https://man7.org/linux/man-pages/man7/pty.7.html)、
[`posix_openpt(3)`](https://man7.org/linux/man-pages/man3/posix_openpt.3.html)、
[`TIOCSWINSZ(2const)`](https://man7.org/linux/man-pages/man2/TIOCSWINSZ.2const.html)、
[`boost::asio::posix::basic_stream_descriptor`](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/posix__basic_stream_descriptor.html)。
