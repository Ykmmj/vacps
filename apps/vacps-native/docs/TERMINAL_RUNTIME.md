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
- script 产品层拥有 `terminal_id`、会话容量、空闲回收、HTTP DTO、UTF-8 文本边界、VT screen 与键盘编码。
- control-worker 只做 schema、签名代理和 MCP tool 暴露。

## PTY 建立

Linux 后端使用 UNIX 98 PTY：

1. `posix_openpt` 创建 nonblocking/CLOEXEC master。
2. `grantpt`、`unlockpt`、`ptsname_r` 后打开 slave。
3. child 中 `setsid`、`TIOCSCTTY`、`TIOCSPGRP`，将 slave `dup2` 到 fd 0/1/2。
4. parent 将 master 交给 `asio::posix::stream_descriptor`，异步读取和串行写入。
5. child 是 session leader；显式 signal 每次通过 `TIOCGPGRP` 查询并作用于当前前台 process group。
6. close/timeout 同时处理当前前台 group 与初始 leader group；升级阶段关闭 master，避免其他 job-control group 持有 slave 导致永久等待。

stdout/stderr 在 PTY 层天然合并。任何声称能在同一 PTY 中可靠恢复两条独立流的 API 都是不真实的。

## 数据和游标

- 域层保存原始字节，以单调递增的绝对 byte offset 定址。
- 缓冲区有固定上限；旧数据被淘汰后，落后的读取返回 `dropped=true` 和新的 `available_from`。
- 产品 `terminal.read` 只返回 UTF-8 `content`；`next_cursor` 仍是十进制字节偏移。
- 非 EOF 读取不会把多字节 UTF-8 字符切成两个响应；不完整尾部留给下次从同一字节位置读取。
- `max_bytes` 最小为 4，确保任意一个 UTF-8 code point 都能取得进展。
- ANSI escape sequence 属于终端文本的一部分，不在 transport/domain 层删除。

当前 MCP 数据面使用 cursor + bounded long-poll。未来若 UI 需要低延迟连续渲染，可以在相同 TerminalSessions 生命周期之上增加 WebSocket transport；不改变 PTY 域对象，也不引入另一套 process session。

## 交互语义

- `expect` 从调用方 cursor 开始，先扫描滚动缓冲区已有文本，再 long-poll 新文本；literal/regex 都可跨 read chunk 匹配。
- timeout 或主进程退出时，先扫描完当时已经观察到的 buffer boundary 再返回。主进程状态不再依赖 PTY EOF。
- `send_keys` 的公开边界是 `KeyEvent { key, ctrl?, alt?, shift? }`。固定 escape sequence 只是 encoder 内部数据，不是 API 能力边界。
- DECCKM (`CSI ? 1 h/l`) 决定方向键、Home、End 使用 CSI 还是 SS3；修饰导航键使用 xterm modifier parameter。
- Backspace 来自当前 PTY termios `VERASE`，不写死为 `0x7f`。
- 产品默认 `TERM=xterm-256color`，允许调用方通过 environment 覆盖；screen parser 会回答常见 DA/DSR/CPR/window-size 查询。
- `screen` 是无样式的可见文本模型，覆盖常见 CSI、滚动区、origin/insert/wrap、alternate screen、tab stop 与 DEC line drawing；颜色和字体不进入 API。

## 生命周期

```text
process: Created → Starting → Running → Exited | Signaled | TimedOut
session: Open → Closing → Closed
pty:     Open → EOF
```

- process 状态在 child reap 时立即完成；它不等待 PTY EOF。
- `waitForExit`/产品 retention 完成条件仍是 child 已 reap 且 PTY 已 EOF，避免丢掉退出前最后一段输出。
- `resize` 使用 `TIOCSWINSZ`，由内核向前台进程组产生 `SIGWINCH`。
- `close` 先向前台/leader group 发 `SIGHUP`，宽限期后发 `SIGKILL` 并关闭 master，再等待 leader reap。
- 退出字段与 close 动作字段是两个事实域：`process_state` / `signal` / `timed_out` 描述进程最终如何结束；`escalated` / `final_signal` 只描述本次 `close` 是否等到自己的宽限期并执行升级。若 hard timeout 在 close 等待宽限期时先发送 `SIGKILL`，结果应为 `timed_out + SIGKILL`，同时 `escalated=false`、`final_signal=null`。
- `read/write/waitForExit` 的 stop token 只取消该次 Asio operation；不会调用 `dispose()`，也不会因为一个 HTTP/MCP 等待被取消而销毁共享 terminal。
- 显式关闭后 session 立即从产品 registry 移除；未关闭的已完成 session 只短期保留。
- Agent 重启不会恢复 terminal；需要恢复、调度或重试的执行必须使用 `tasks.*`。

## 资源边界

- 最多 64 个产品 terminal session。
- terminal 和 process 共享 `ProcessBudget` 的进程数与全局缓冲预算。
- 单会话滚动缓冲默认 4 MiB，可配置 64 KiB..16 MiB。
- 单次 read/write 最大 1 MiB；read long-poll 最大 60 秒。
- 默认 idle timeout 30 分钟；产品层定时关闭无人使用的活会话。

## 宿主机真实程序回归

修改 PTY、key encoder、expect、screen 或 terminal session 后，在仓库根目录运行：

```bash
pnpm --dir apps/vacps-native/script run test:terminal:host
```

该命令使用 `build/release/vacps-agent-linux-x86_64` 驱动宿主机的
`/usr/bin/vim`、`/usr/bin/less`、`/usr/bin/python3` 和 `/bin/bash`，覆盖：

- Vim alternate screen、Insert/Normal、方向键、保存退出和文件结果；
- less PageUp/PageDown、搜索和退出；
- Python REPL prompt/expect、普通输入和 Ctrl-D；
- Bash Ctrl-Z、`jobs`、`fg`、Ctrl-C 与前台进程组切换；
- resize 后前台进程实际收到 `SIGWINCH`，并用 `stty size` 观察内核尺寸；
- rolling buffer 超限后的 `available_from`、`dropped`、`dropped_bytes`、UTF-8 边界和 cursor 单调推进。

这组测试依赖宿主机真实程序版本，定位为 Linux 本机/专用 runner 的行为回归；普通
Node Vitest 不替代它。测试源为
`script/tests/terminal_interaction_regression.ts`。

## 契约

- C++ 内部接口是 narrow contract：owner executor、状态和参数范围由调用方保证，不重复做业务防御。
- QuickJS/HTTP/MCP 是 wide boundary：在产生副作用前验证路径、尺寸、枚举和数值范围。
- environment 是“继承后覆盖”；不提供持久化、自动重连、自动重试或伪造的 stdout/stderr 分离。

参考：[Linux PTY overview](https://man7.org/linux/man-pages/man7/pty.7.html)、
[`posix_openpt(3)`](https://man7.org/linux/man-pages/man3/posix_openpt.3.html)、
[`TIOCSWINSZ(2const)`](https://man7.org/linux/man-pages/man2/TIOCSWINSZ.2const.html)、
[`boost::asio::posix::basic_stream_descriptor`](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/posix__basic_stream_descriptor.html)。
