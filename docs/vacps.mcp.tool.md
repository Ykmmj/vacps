# VACPS MCP Tool 规范（Schema v3）

Status: **Schema v3 hard break** — 首个可用版本契约。  
Naming: always **vacps**.  
Wire: **nested JSON**, **snake_case** keys, **snake_case** enums.

```json
{ "schema_version": "3.0" }
```

## 发布原则

```text
不接受旧字段
不保留旧 Tool
不返回 deprecated warning
不做 legacy normalize
不根据 clientInfo 暴露不同版本
不维护 v2/v3 双 Schema
```

- 客户端 / Agent / 控制面 **同发同升**
- 旧调用直接 **validation / unknown tool** 失败

---

## 统一结果信封

每个有效 `tools/call`：

```json
{
  "content": [{ "type": "text", "text": "<compact JSON of structuredContent>" }],
  "structuredContent": {
    "ok": true,
    "schema_version": "3.0",
    "request_id": "uuid",
    "trace_id": "uuid",
    "generated_at": "ISO-8601",
    "warnings": []
  },
  "isError": false
}
```

失败：

```json
{
  "isError": true,
  "structuredContent": {
    "ok": false,
    "schema_version": "3.0",
    "error": {
      "code": "…",
      "message": "…",
      "category": "validation|not_found|conflict|…",
      "retryable": false,
      "retry_after_ms": null,
      "details": {}
    }
  }
}
```

### 错误三层

| 层 | 何时 | isError |
|----|------|---------|
| 协议 / 校验 | 未知 tool、参数非法 | true |
| 业务 | backend 不可达、冲突、不存在 | true |
| 进程结果 | 命令非 0 / 超时 | **false**（用 status / exit_code） |

---

## 最终 Tool 集（41）

```text
vacps.backends.list
vacps.backends.get_status
vacps.capabilities.get

vacps.command.exec
vacps.shell.exec

vacps.process.start_command
vacps.process.start_shell
vacps.process.read
vacps.process.write
vacps.process.terminate

vacps.files.stat
vacps.files.read
vacps.files.list
vacps.files.glob
vacps.files.grep
vacps.files.mkdir
vacps.files.write
vacps.files.edit
vacps.files.move
vacps.files.delete
vacps.files.apply_patch

vacps.git.status
vacps.git.diff
vacps.git.apply

vacps.tasks.create_command
vacps.tasks.create_shell
vacps.tasks.create_agent
vacps.tasks.get
vacps.tasks.list
vacps.tasks.output.read
vacps.tasks.cancel
vacps.tasks.retry
vacps.tasks.delete
vacps.tasks.cleanup.preview
vacps.tasks.cleanup.run

vacps.schedules.create
vacps.schedules.get
vacps.schedules.list
vacps.schedules.update
vacps.schedules.delete
vacps.schedules.run_now
```

### 已删除（调用必须失败）

```text
vacps.tasks.create
vacps.process.start
```

---

## Annotations

每个 Tool 显式四 Hint（不依赖默认值）。

| 类别 | readOnly | destructive | idempotent | openWorld | Tools |
|------|----------|-------------|------------|-----------|-------|
| 只读 | true | false | true | false | backends.*, capabilities.get, process.read, files.stat/read/list/glob/grep, git.status/diff, tasks.get/list/output.read, schedules.get/list |
| 命令执行 | false | true | false | true | command.exec, shell.exec, process.start_*, tasks.create_*, tasks.retry, schedules.run_now |
| 本地修改 | false | true | false | false | process.write, files.write/edit/move/delete/apply_patch, git.apply, schedules.create/update |
| mkdir | false | false | true | false | files.mkdir |
| 幂等破坏 | false | true | true | false | process.terminate, tasks.cancel, schedules.delete |

---

## 关键输入形状

### Task `kind`（唯一类型字段）

```text
command | shell | agent
```

禁止：`type`、`shell.mode`、`shell.program` 等旧联合。

#### `vacps.tasks.create_command`

```json
{
  "backend_id": "backend-01",
  "program": "npm",
  "arguments": ["test"],
  "working_directory": "/srv/app",
  "timeout_seconds": 600,
  "environment": {},
  "labels": {},
  "output": {
    "capture_stdout": true,
    "capture_stderr": true,
    "preview_max_bytes": 8192,
    "retention_seconds": 604800,
    "hard_max_bytes": 104857600
  },
  "idempotency_key": "task-cmd-001"
}
```

#### `vacps.tasks.create_shell`

```json
{
  "backend_id": "backend-01",
  "command": "npm ci && npm run build",
  "shell": "/bin/bash",
  "load_user_environment": true,
  "timeout_seconds": 1800
}
```

#### `vacps.tasks.create_agent`

```json
{
  "backend_id": "backend-01",
  "prompt": "Diagnose deployment",
  "profile": "diagnostic",
  "max_steps": 50,
  "timeout_seconds": 1800,
  "permissions": { "shell": true, "network": true, "file_write": false }
}
```

### Process（无 mode / oneOf）

#### `vacps.process.start_command`

```json
{
  "backend_id": "backend-01",
  "program": "node",
  "arguments": ["server.js"],
  "working_directory": "/srv/app",
  "tty": false,
  "timeout_ms": 3600000,
  "stdout_hard_max_bytes": 104857600,
  "stderr_hard_max_bytes": 104857600,
  "idempotency_key": "process-command-001"
}
```

#### `vacps.process.start_shell`

```json
{
  "backend_id": "backend-01",
  "command": "npm run dev",
  "shell": "/bin/bash",
  "load_user_environment": true,
  "tty": true,
  "timeout_ms": 3600000
}
```

### Schedule

#### `vacps.schedules.create`

```json
{
  "backend_id": "backend-01",
  "name": "Nightly backup",
  "trigger": {
    "type": "cron",
    "expression": "0 2 * * *",
    "timezone": "UTC"
  },
  "policy": {
    "concurrency": "forbid",
    "misfire": "run_once",
    "max_catchup_runs": 1
  },
  "enabled": true,
  "task": {
    "kind": "command",
    "program": "./backup",
    "arguments": [],
    "working_directory": "/srv/app",
    "timeout_seconds": 3600
  },
  "idempotency_key": "schedule-create-001"
}
```

禁止顶层：`cron`、`timezone`、`task_template`。

#### `vacps.schedules.update`

```json
{
  "schedule_id": "uuid",
  "expected_revision": 3,
  "changes": {
    "name": "Updated backup",
    "enabled": false,
    "trigger": { "expression": "0 3 * * *" },
    "policy": { "concurrency": "queue" },
    "task": {
      "kind": "command",
      "program": "./backup",
      "arguments": ["--full"],
      "timeout_seconds": 3600
    }
  }
}
```

`changes` 只允许：`name` | `enabled` | `trigger` | `policy` | `task`。

#### `vacps.schedules.get`

```json
{ "schedule_id": "uuid" }
```

无 `idempotency_key`。

### 只读 Tool 无 `idempotency_key`

```text
backends.list / get_status
capabilities.get
process.read
files.stat / read / list / glob / grep
git.status / diff
tasks.get / list / output.read / cleanup.preview
schedules.get / list
```

### Task 保留 / 清理（Phase 0–1）

| Tool | 作用 |
|------|------|
| `tasks.list` | 过滤：`environment`、`source`、`labels`、`terminal`、`hide_test`、`include_deleted` 等；默认隐藏软删除 |
| `tasks.delete` | 终态任务软删（默认）/ 硬删；活跃 → `task_not_terminal` |
| `tasks.cleanup.preview` | 按 filter 统计可清理终态任务 |
| `tasks.cleanup.run` | 批量软删；`expected_matched_count` 防范围漂移 |

测试任务建议标签：

```json
{
  "environment": "test",
  "suite": "mcp-regression",
  "purpose": "acceptance-test"
}
```

控制面 cron 仅自动软删 **过期测试任务**（`environment=test` 或 `retention_class=test`），并对超过 24h 软删宽限的任务硬删。生产自动保留期需后续 Dry Run 后再开。

### 文件分页字段

| Tool | 列表字段 |
|------|----------|
| `files.list` | **`entries`** only |
| `files.glob` | **`matches`** only |
| `files.grep` | **`matches`** only |

### Task 输出

`vacps.tasks.output.read` 只返回：

```json
{
  "encoding": "utf-8",
  "content": "…",
  "stream_version": "sha256:…",
  "offset": 0,
  "next_offset": 18,
  "eof": true
}
```

禁止输出字段：`data`。

流冲突：

```json
{
  "error": {
    "code": "stream_version_conflict",
    "category": "conflict",
    "details": {
      "expected_stream_version": "sha256:…",
      "current_stream_version": "sha256:…",
      "restart_offset": 0
    }
  }
}
```

---

## SHA-256

统一 pattern：

```text
^sha256:[a-f0-9]{64}$
```

用于：`expected_sha256`、`request_hash`、`stream_version`、`expected_stream_version`、`before_sha256`、`after_sha256`。

---

## 业务数值边界（非 JS 安全整数）

```text
timeout_seconds: 1..86400
timeout_ms: 1..3600000
retention_seconds: 60..2592000
hard_max_bytes: 0..1073741824
max_steps: 1..1000
revision: 1..2147483647
```

---

## 幂等语义

副作用操作可带 `idempotency_key`：

```text
同 key + 同请求 hash → replay
同 key + 不同请求    → idempotency_conflict
无 key               → 自然语义（如 delete already_absent）
```

---

## 必须失败的旧调用

| 旧调用 | 结果 |
|--------|------|
| `vacps.tasks.create` + `type`/`shell.mode` | unknown tool |
| `vacps.process.start` + `mode` | unknown tool |
| schedule `{ cron, task_template }` | invalid_arguments / validation |
| 只读 tool 传 `idempotency_key` | validation（additionalProperties / unknown field） |

---

## 实现源

单源 Zod → tools/list + runtime parse：

```text
apps/control-worker/src/mcp/
  server.ts                 # tool 注册 + handler
  schema/
    common.ts               # 版本常量 + $defs 再导出
    annotations.ts
    envelope.ts
    defs.ts
    backends.ts
    command.ts
    shell.ts
    process.ts
    files.ts
    git.ts
    tasks.ts
    schedules.ts
    registry.ts             # publicToolJsonSchemas（全量 tools）
    index.ts
  tool-schemas.ts           # 兼容 re-export
  task-schedule-adapters.ts # 兼容 re-export
```

D1 schedule 行：`task_json`（V3 task 载荷）；`cron`/`timezone` 为 trigger 的列存储。

共享 wire：

```text
packages/contracts  (kind + snake_case task/schedule)
apps/vacps          (agent HTTP 同形状执行)
```

Agent 任务 dispatch 示例：

```json
{
  "kind": "command",
  "backend_id": "backend-01",
  "program": "uname",
  "arguments": ["-a"],
  "working_directory": "/tmp",
  "timeout_seconds": 60,
  "task_id": "uuid",
  "source": "mcp"
}
```

---

## 部署后 Host 缓存

```text
重启 MCP Server / 重建 Session
刷新 tools/list（tool_schema_revision / schema_version 3.0）
Connector / App 重新拉取 Tool 定义
```

`TOOL_SCHEMA_REVISION` 当前：`2026-07-29-schema-v3`  
MCP protocol meta：`0.5.0`
