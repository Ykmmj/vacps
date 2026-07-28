# VACPS 控制平台需求与架构设计

> 版本：v1.0  
> 状态：待实现  
> 核心技术：Cloudflare Workers + Remote MCP + Web UI + D1 + BullMQ + Redis + LangGraph.js + Pi + SQLite  
> 部署原则：Cloudflare 作为控制面；每台 VPS 运行一个轻量 Agent Backend；不使用 Docker；第一版允许执行任意命令。

---

## 1. 项目目标

构建一个面向多台 VPS 的轻量 Agent 控制平台，用户可以：

- 在 ChatGPT 中通过 MCP 选择 VPS、执行任意 Shell 命令或下发自然语言 Agent 任务；
- 在 Cloudflare Workers 提供的 Web UI 中添加、编辑和删除 VPS Backend；
- 查看 VPS 在线状态、系统信息、任务队列、运行日志和执行结果；
- 创建即时任务和 Cron 定时任务；
- 由 BullMQ 负责任务排队、定时、并发控制和运行状态；
- 由 LangGraph 管理单个任务的生命周期、状态保存以及后续审批扩展；
- 由 Pi 负责理解自然语言目标并自主调用 Shell；
- 第一版默认使用 `full` 权限，后期通过 Agent Profile 增加权限分级。

本项目不建设通用低代码平台，不设计复杂 DAG 编辑器，不提前实现多租户、计费、复杂 RBAC 和容器沙箱。

---

## 2. 核心设计原则

### 2.1 保留必要骨架，不提前平台化

第一版保留以下四个核心部分：

1. **Cloudflare Workers**：MCP、Web UI、管理 API 和节点注册。
2. **BullMQ**：异步任务、定时任务、队列、并发和状态。
3. **LangGraph**：单次任务的生命周期和可恢复状态。
4. **Pi / ShellExecutor**：实际执行自然语言任务或任意命令。

不增加独立的中央 Control API、Temporal、Kubernetes、消息总线或插件平台。

### 2.2 任意命令是底层原语

系统底层只需要一个统一的命令执行能力：

```text
exec(command, cwd, env, timeout)
```

第一版默认允许执行 Backend Linux 用户有权执行的任意命令。

后期权限分级不改变执行器，只在执行器前增加 Policy：

```text
Command Request
      ↓
Policy.authorize()
      ↓
ShellExecutor.execute()
```

### 2.3 Pi 是唯一的任务推理 Agent

- ChatGPT 负责理解用户请求并选择 MCP 工具；
- LangGraph 只负责流程和状态；
- Pi 负责 VPS 内部的分析、计划和命令调用；
- ShellExecutor 只负责确定性命令执行。

不让 LangGraph 再运行一个与 Pi 重复的规划 Agent。

### 2.4 一个任务使用同一个全局 ID

Cloudflare 生成 `taskId`，并在所有组件中复用：

```text
D1 task.id
= BullMQ jobId
= LangGraph thread_id
= VPS SQLite task.id
```

这样日志、状态、重试和排错都能通过同一个 ID 关联。

### 2.5 运行时状态与永久数据分离

| 存储           | 负责内容                                                         |
| -------------- | ---------------------------------------------------------------- |
| Cloudflare D1  | VPS 注册、任务索引、定时任务定义、Profile 元数据                 |
| Redis / BullMQ | 队列、延迟任务、Job Scheduler、锁、运行时状态                    |
| VPS SQLite     | 完整任务记录、命令记录、日志索引、LangGraph checkpoint、最终结果 |
| VPS 文件系统   | 大体积 stdout、stderr、Artifact 和工作目录                       |

Redis 不是永久数据库。D1 和 VPS SQLite 才是需要备份的数据。

---

## 3. 总体架构

```text
                         ChatGPT
                            │
                     Remote MCP
                            │
                            ▼
┌──────────────────────────────────────────────────────────┐
│                 Cloudflare Workers 控制面                │
│                                                          │
│  /mcp                Remote MCP Server                   │
│  /api/*              Web 管理 API                        │
│  /                   Web UI / Static Assets              │
│                                                          │
│  D1                                                      │
│  ├── backends                                            │
│  ├── tasks                                               │
│  ├── schedules                                           │
│  └── profiles                                            │
│                                                          │
│  Cron Handler                                            │
│  └── Backend 状态检查与 Schedule 对账                    │
└───────────────────────────┬──────────────────────────────┘
                            │ HTTPS
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
┌───────────────────┐ ┌───────────────────┐ ┌───────────────────┐
│ VACPS LA      │ │ VACPS Tokyo   │ │ VACPS EU      │
│                   │ │                   │ │                   │
│ HTTP Backend API  │ │ HTTP Backend API  │ │ HTTP Backend API  │
│ BullMQ Producer   │ │ BullMQ Producer   │ │ BullMQ Producer   │
│ BullMQ Worker     │ │ BullMQ Worker     │ │ BullMQ Worker     │
│ LangGraph         │ │ LangGraph         │ │ LangGraph         │
│ Pi Runtime        │ │ Pi Runtime        │ │ Pi Runtime        │
│ ShellExecutor     │ │ ShellExecutor     │ │ ShellExecutor     │
│ SQLite            │ │ SQLite            │ │ SQLite            │
└─────────┬─────────┘ └─────────┬─────────┘ └─────────┬─────────┘
          │                     │                     │
          └─────────────────────┼─────────────────────┘
                                ▼
                         Redis Cloud
```

### 3.1 关键约束

- Cloudflare Worker 不直接连接 Redis。
- 所有 BullMQ 操作都由 VACPS Backend 执行。
- 每台 VPS 使用独立 BullMQ Queue。
- Cloudflare Worker 通过 HTTPS 调用目标 VPS Backend。
- VPS Backend 可以通过 Cloudflare Tunnel 暴露，不开放原始服务端口。
- Web UI、MCP 和管理 API 放在同一个 Cloudflare Worker 项目中。

---

## 4. 核心概念

### 4.1 Backend

Backend 表示一台安装了 VACPS 的服务器。

```ts
interface Backend {
  id: string;
  name: string;
  baseUrl: string;
  region?: string;
  tags: string[];
  enabled: boolean;
  createdAt: string;
  updatedAt: string;
}
```

示例：

```json
{
  "id": "vps-la-01",
  "name": "Los Angeles VPS",
  "baseUrl": "https://la-agent.example.com",
  "region": "us-west",
  "tags": ["production", "full"],
  "enabled": true
}
```

### 4.2 Task

Task 表示一次立即执行的 Shell 或 Agent 任务。

```ts
type TaskType = 'shell' | 'agent';
```

Shell 任务：

```json
{
  "backendId": "vps-la-01",
  "type": "shell",
  "command": "uname -a && uptime && df -h",
  "cwd": "/home/agent",
  "profile": "full",
  "timeoutSeconds": 300
}
```

Agent 任务：

```json
{
  "backendId": "vps-la-01",
  "type": "agent",
  "prompt": "检查 nginx 异常，发现明确问题后直接修复并输出报告",
  "cwd": "/srv/project",
  "profile": "full",
  "timeoutSeconds": 1800
}
```

### 4.3 Schedule

Schedule 表示由 BullMQ Job Scheduler 周期生成 Task 的定义。

```ts
interface Schedule {
  id: string;
  backendId: string;
  name: string;
  cron: string;
  timezone: string;
  enabled: boolean;
  taskTemplate: TaskTemplate;
}
```

### 4.4 Agent Profile

Profile 表示任务权限配置。

第一版只启用：

```text
full
```

后期可扩展：

```text
readonly
ops
restricted
custom
```

Profile 从第一版就存在于 Task Schema 中，但 `full` 的 Policy 实现直接放行。

---

## 5. 组件职责

### 5.1 Cloudflare Workers

Cloudflare Workers 是统一控制面，负责：

- Remote MCP；
- Web UI；
- Web 管理 API；
- Backend 注册信息；
- Backend 连通性检测；
- 任务创建和状态代理；
- 定时任务定义管理；
- Schedule 对账；
- 用户身份认证；
- 请求审计。

Cloudflare Workers 不负责：

- 运行 Pi；
- 执行 Shell；
- 持有 BullMQ Worker；
- 保存完整任务日志；
- 管理 LangGraph checkpoint。

### 5.2 D1

D1 保存控制面数据：

```text
backends
tasks
schedules
profiles
audit_events
```

D1 中的 Task 只保存索引和摘要，完整日志仍在目标 VPS。

### 5.3 VACPS Backend

每台 VPS 运行同一个 `vacps` 程序，内部包含：

```text
HTTP API
BullMQ Queue Producer
BullMQ Worker
LangGraph TaskRunner
Pi Runtime
ShellExecutor
Policy Engine
SQLite Store
```

第一版以单进程运行：

```text
RUN_MODE=all
```

后续可不改代码地拆分：

```text
RUN_MODE=api
RUN_MODE=worker
```

### 5.4 BullMQ

BullMQ 负责：

- 接收任务；
- 异步排队；
- 延迟任务；
- Cron Job Scheduler；
- 并发控制；
- Worker 消费；
- 运行状态；
- 可选重试；
- 任务清理。

BullMQ 不负责：

- Agent 推理；
- 命令权限；
- 任务内部流程；
- 永久日志。

### 5.5 LangGraph

LangGraph 管理一个 Task 从开始到结束的状态。

第一版 Graph：

```text
START
  ↓
prepare
  ↓
authorize
  ↓
execute
  ↓
verify
  ↓
finalize
  ↓
END
```

第一版的 `authorize` 使用 `FullAccessPolicy`，因此不会增加用户操作步骤。

后期可在同一个 Graph 中加入：

```text
authorize
   ├── allow → execute
   ├── deny  → finalize
   └── approval_required → interrupt → resume → execute
```

LangGraph 不管理 Cron，也不代替 BullMQ。

### 5.6 Pi Runtime

Pi 负责 Agent 类型任务：

- 理解 Prompt；
- 检查系统；
- 决定需要执行的命令；
- 调用统一 `exec` Tool；
- 根据结果继续分析；
- 输出最终报告。

Pi 不直接绕过 ShellExecutor。

即使 Profile 为 `full`，Pi 的命令仍经过：

```text
Pi exec Tool
    ↓
CommandPolicy
    ↓
ShellExecutor
```

这样后期增加权限时不需要重写 Pi 集成。

### 5.7 ShellExecutor

ShellExecutor 是系统真实执行命令的唯一入口。

建议实现：

```text
/bin/bash -lc "<command>"
```

必须支持：

- `cwd`；
- 环境变量；
- 超时；
- stdout/stderr 流式记录；
- AbortSignal；
- 进程组终止；
- exit code；
- 命令审计；
- 输出大小限制。

第一版不做命令白名单。

---

## 6. 任务生命周期

统一状态：

```text
created
dispatching
queued
running
waiting_for_approval
succeeded
failed
cancelled
timed_out
dispatch_failed
```

正常流程：

```text
Cloudflare 创建 taskId
        ↓
D1 写入 created
        ↓
调用目标 Backend
        ↓
Backend queue.add(jobId=taskId)
        ↓
D1 更新 queued
        ↓
BullMQ Worker 领取
        ↓
LangGraph running
        ↓
Shell 或 Pi 执行
        ↓
SQLite 保存结果
        ↓
BullMQ completed / failed
        ↓
Cloudflare 查询并更新最后状态
```

---

## 7. LangGraph 节点设计

### 7.1 Graph State

```ts
interface TaskGraphState {
  taskId: string;
  backendId: string;
  type: 'shell' | 'agent';
  profile: string;

  command?: string;
  prompt?: string;
  cwd: string;
  timeoutSeconds: number;

  status: string;
  startedAt?: string;
  finishedAt?: string;

  commands: CommandExecution[];
  output?: string;
  result?: unknown;
  error?: {
    code: string;
    message: string;
  };

  approval?: {
    id: string;
    reason: string;
    payload: unknown;
  };
}
```

### 7.2 `prepare`

职责：

- 校验任务字段；
- 解析和校验 `cwd`；
- 加载 Profile；
- 创建任务工作目录；
- 初始化日志；
- 将状态更新为 `running`。

不调用模型。

### 7.3 `authorize`

职责：

- 检查 Task 级权限；
- 判断 Profile 是否允许 Shell 或 Agent 类型；
- 第一版 `full` 直接返回 `allow`。

接口：

```ts
interface PolicyDecision {
  decision: 'allow' | 'deny' | 'approval_required';
  reason?: string;
}
```

### 7.4 `execute`

根据任务类型路由：

```text
shell → ShellExecutor
agent → PiRuntime
```

Shell 任务直接执行用户传入命令。

Agent 任务调用 Pi；Pi 可多次调用统一 `exec` Tool。

### 7.5 `verify`

第一版支持三种验证模式：

```text
none
exit_code
command
```

默认：

- Shell：`exit_code`；
- Agent：由 Pi 的最终状态和运行结果判断。

可选验证命令：

```json
{
  "verify": {
    "mode": "command",
    "command": "systemctl is-active nginx"
  }
}
```

### 7.6 `finalize`

职责：

- 写入最终状态；
- 保存执行摘要；
- 关闭日志流；
- 更新 SQLite；
- 返回 BullMQ Job Result。

### 7.7 后期审批扩展

需要审批时，在 `authorize` 和 `execute` 之间插入 LangGraph interrupt：

```text
authorize
   ↓ approval_required
interrupt
   ↓ 用户从 ChatGPT 或 Web UI 批准
resume
   ↓
execute
```

第一版不实现审批页面，但数据结构和 Graph 分支保留。

---

## 8. BullMQ 设计

### 8.1 Queue 命名

每台 Backend 使用独立队列：

```text
agent-vps-la-01
agent-vps-tokyo-01
agent-vps-eu-01
```

Backend ID 必须是安全 slug：

```text
[a-z0-9-]{1,64}
```

每台 VPS 只消费自己的 Queue。

### 8.2 Job 数据

```ts
interface AgentJobData {
  taskId: string;
  backendId: string;
  type: 'shell' | 'agent';
  profile: string;
  command?: string;
  prompt?: string;
  cwd: string;
  timeoutSeconds: number;
  verify?: VerifyConfig;
  source: 'mcp' | 'web' | 'schedule' | 'api';
  scheduleId?: string;
}
```

### 8.3 并发

第一版默认：

```text
concurrency = 1
```

每台 Backend 可单独配置：

```text
WORKER_CONCURRENCY=1
```

后期根据机器配置增加。

### 8.4 重试

任意命令可能包含副作用，因此第一版默认：

```text
attempts = 1
```

只有用户明确配置重试时才启用：

```json
{
  "retry": {
    "attempts": 3,
    "backoffSeconds": 10
  }
}
```

定时巡检类只读任务可以默认允许一次重试。

### 8.5 Job 清理

Redis 只保留有限历史：

```ts
removeOnComplete: {
  age: 86400,
  count: 200
}

removeOnFail: {
  age: 604800,
  count: 500
}
```

完整结果保存在 VPS SQLite 和日志文件中。

### 8.6 Job Scheduler

Schedule 创建流程：

```text
Cloudflare D1 保存 Schedule
        ↓
调用对应 Backend /schedulers/upsert
        ↓
Backend queue.upsertJobScheduler()
        ↓
Redis 保存 Scheduler
```

D1 是 Schedule 定义的事实来源。

Cloudflare Cron 每隔数分钟进行对账：

```text
D1 enabled schedules
        ↕
Backend BullMQ Job Schedulers
```

Redis Scheduler 丢失后可自动重建。

---

## 9. Cloudflare MCP 工具

第一版 MCP 只暴露高层能力，不暴露内部 Redis 或 LangGraph。

### Backend

```text
backends.list
backends.get_status
```

Backend 新增、修改、删除默认只在 Web 管理页面和管理 API 中开放。

### Task

```text
tasks.create
tasks.get
tasks.list
tasks.cancel
tasks.retry
```

### Schedule

```text
schedules.create
schedules.get
schedules.list
schedules.update
schedules.delete
schedules.run_now
```

### 第一版 `tasks.create` Schema

```json
{
  "backendId": "vps-la-01",
  "type": "shell",
  "command": "uname -a",
  "cwd": "/home/agent",
  "profile": "full",
  "timeoutSeconds": 300
}
```

或：

```json
{
  "backendId": "vps-la-01",
  "type": "agent",
  "prompt": "检查并修复 nginx",
  "cwd": "/srv/project",
  "profile": "full",
  "timeoutSeconds": 1800
}
```

MCP 创建任务后立即返回：

```json
{
  "taskId": "01K...",
  "status": "queued",
  "backendId": "vps-la-01"
}
```

MCP 不等待长任务执行完成。

---

## 10. Cloudflare Web UI

同一个 Worker 项目提供 SPA。

### 10.1 Dashboard

显示：

- Backend 总数；
- 在线和离线数量；
- 排队任务；
- 运行任务；
- 最近失败任务；
- 待审批任务（后期）；
- Redis 和调度状态摘要。

### 10.2 Backends 页面

列表字段：

```text
名称
Backend ID
区域
标签
在线状态
VACPS 版本
Pi 状态
Redis 状态
Worker 状态
等待任务数
运行任务数
最后检查时间
```

操作：

```text
添加
编辑
删除
测试连接
刷新状态
创建 Shell 任务
创建 Agent 任务
查看该节点任务
```

添加 Backend 表单：

```text
Backend ID
名称
Base URL
区域
标签
是否启用
```

第一版所有 Backend 共用一个 Worker Secret：

```text
BACKEND_SHARED_TOKEN
```

因此表单不要求保存 Token。

后期可改成每个 Backend 独立 Token，并在 D1 中加密保存。

### 10.3 Tasks 页面

显示：

```text
Task ID
Backend
类型
来源
命令或 Prompt 摘要
状态
开始时间
耗时
Profile
```

详情页显示：

- 完整输入；
- BullMQ 状态；
- LangGraph 当前节点；
- Pi 最终报告；
- 所有执行命令；
- stdout；
- stderr；
- exit code；
- 错误；
- 重试记录。

操作：

```text
取消
重试
复制任务
```

### 10.4 Schedules 页面

显示：

```text
名称
Backend
任务类型
Cron
时区
启用状态
上次运行
下次运行
```

操作：

```text
创建
修改
启用
禁用
立即运行
删除
```

### 10.5 Profiles 页面

第一版仅显示：

```text
full
```

后期用于配置：

- 允许任务类型；
- 允许命令规则；
- 禁止命令规则；
- 允许目录；
- 是否允许 sudo；
- 最大超时；
- 是否需要审批。

---

## 11. Cloudflare 管理 API

### 11.1 Backend API

```text
GET    /api/backends
POST   /api/backends
GET    /api/backends/:id
PATCH  /api/backends/:id
DELETE /api/backends/:id
POST   /api/backends/:id/test
GET    /api/backends/:id/status
```

### 11.2 Task API

```text
GET    /api/tasks
POST   /api/tasks
GET    /api/tasks/:id
GET    /api/tasks/:id/logs
POST   /api/tasks/:id/cancel
POST   /api/tasks/:id/retry
```

### 11.3 Schedule API

```text
GET    /api/schedules
POST   /api/schedules
GET    /api/schedules/:id
PATCH  /api/schedules/:id
DELETE /api/schedules/:id
POST   /api/schedules/:id/run
POST   /api/schedules/reconcile
```

---

## 12. VPS Backend API

每台 VPS 提供：

```text
GET    /health
GET    /info
GET    /metrics

POST   /tasks
GET    /tasks/:id
GET    /tasks/:id/logs
POST   /tasks/:id/cancel
POST   /tasks/:id/retry

GET    /schedulers
PUT    /schedulers/:id
DELETE /schedulers/:id
POST   /schedulers/:id/run
```

### 12.1 `/health`

```json
{
  "ok": true,
  "backendId": "vps-la-01",
  "version": "0.1.0",
  "uptimeSeconds": 12345,
  "worker": {
    "running": true,
    "concurrency": 1
  },
  "redis": {
    "connected": true
  },
  "pi": {
    "available": true,
    "version": "..."
  }
}
```

### 12.2 `/metrics`

```json
{
  "cpu": {
    "load1": 0.21
  },
  "memory": {
    "totalBytes": 0,
    "usedBytes": 0
  },
  "disk": {
    "totalBytes": 0,
    "usedBytes": 0
  },
  "queue": {
    "waiting": 0,
    "active": 0,
    "failed": 0
  }
}
```

### 12.3 Backend 身份验证

Cloudflare 到 Backend：

```http
Authorization: Bearer <BACKEND_SHARED_TOKEN>
X-Request-Id: <uuid>
X-Request-Timestamp: <unix-seconds>
```

第一版至少校验 Bearer Token。

后期增加：

- 每节点 Token；
- HMAC 签名；
- 防重放 nonce；
- Cloudflare Access Service Token。

---

## 13. 数据模型

### 13.1 D1 `backends`

```sql
CREATE TABLE backends (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  base_url TEXT NOT NULL,
  region TEXT,
  tags_json TEXT NOT NULL DEFAULT '[]',
  enabled INTEGER NOT NULL DEFAULT 1,
  last_status TEXT,
  last_checked_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
```

### 13.2 D1 `tasks`

```sql
CREATE TABLE tasks (
  id TEXT PRIMARY KEY,
  backend_id TEXT NOT NULL,
  type TEXT NOT NULL,
  source TEXT NOT NULL,
  profile TEXT NOT NULL,
  summary TEXT,
  status TEXT NOT NULL,
  schedule_id TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  finished_at TEXT
);
```

### 13.3 D1 `schedules`

```sql
CREATE TABLE schedules (
  id TEXT PRIMARY KEY,
  backend_id TEXT NOT NULL,
  name TEXT NOT NULL,
  cron TEXT NOT NULL,
  timezone TEXT NOT NULL,
  task_template_json TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1,
  last_run_at TEXT,
  next_run_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
```

### 13.4 VPS SQLite `tasks`

```sql
CREATE TABLE tasks (
  id TEXT PRIMARY KEY,
  bull_job_id TEXT NOT NULL,
  type TEXT NOT NULL,
  profile TEXT NOT NULL,
  input_json TEXT NOT NULL,
  status TEXT NOT NULL,
  graph_node TEXT,
  result_json TEXT,
  error_json TEXT,
  created_at TEXT NOT NULL,
  started_at TEXT,
  finished_at TEXT
);
```

### 13.5 VPS SQLite `commands`

```sql
CREATE TABLE commands (
  id TEXT PRIMARY KEY,
  task_id TEXT NOT NULL,
  sequence INTEGER NOT NULL,
  command TEXT NOT NULL,
  cwd TEXT,
  status TEXT NOT NULL,
  exit_code INTEGER,
  stdout_path TEXT,
  stderr_path TEXT,
  started_at TEXT NOT NULL,
  finished_at TEXT
);
```

---

## 14. 关键业务流程

### 14.1 手动添加 VPS Backend

```text
用户打开 Web UI
      ↓
填写 Backend ID、名称、Base URL
      ↓
Cloudflare POST /api/backends
      ↓
调用 Backend /health 测试
      ↓
成功：写入 D1
失败：提示错误，可选择保存为 disabled
```

### 14.2 Shell 任务

```text
ChatGPT 或 Web UI
      ↓
tasks.create(type=shell)
      ↓
Cloudflare 生成 taskId 并写 D1
      ↓
调用目标 Backend POST /tasks
      ↓
Backend queue.add(jobId=taskId)
      ↓
BullMQ Worker
      ↓
LangGraph prepare → authorize → execute
      ↓
ShellExecutor
      ↓
verify → finalize
      ↓
SQLite 保存结果
```

### 14.3 Agent 任务

```text
ChatGPT 或 Web UI
      ↓
tasks.create(type=agent)
      ↓
Cloudflare → Backend → BullMQ
      ↓
LangGraph execute
      ↓
Pi Runtime
      ↓
Pi 调用 exec Tool
      ↓
Policy → ShellExecutor
      ↓
Pi 根据输出继续运行
      ↓
最终报告
      ↓
LangGraph verify → finalize
```

### 14.4 定时任务

```text
创建 Schedule
      ↓
D1 保存定义
      ↓
Backend upsert BullMQ Job Scheduler
      ↓
到达 Cron 时间
      ↓
BullMQ 生成普通 Job
      ↓
执行相同 LangGraph
```

### 14.5 查看状态

```text
Web UI / ChatGPT
      ↓
Cloudflare 查询 D1 Task 索引
      ↓
调用目标 Backend GET /tasks/:id
      ↓
返回 BullMQ + LangGraph + SQLite 聚合状态
```

### 14.6 取消任务

Queued：

```text
BullMQ remove
→ 状态 cancelled
```

Running：

```text
设置取消标记
→ AbortController.abort()
→ SIGTERM 进程组
→ 超时后 SIGKILL
→ 状态 cancelled
```

---

## 15. Agent 权限扩展设计

第一版：

```text
profile = full
policy = FullAccessPolicy
```

实现接口：

```ts
interface CommandPolicy {
  authorize(input: { taskId: string; profile: string; command: string; cwd: string }): Promise<{
    decision: 'allow' | 'deny' | 'approval_required';
    reason?: string;
  }>;
}
```

后期实现：

```text
ReadOnlyPolicy
OpsPolicy
RestrictedPolicy
CustomPolicy
```

权限检查必须发生在每次命令执行前，而不是只检查任务 Prompt。

后期 Profile 示例：

```json
{
  "id": "ops",
  "allowSudo": true,
  "allowedWorkingDirectories": ["/srv", "/opt", "/var/log"],
  "deniedPatterns": ["rm -rf /", "mkfs", "shutdown", "reboot"],
  "approvalPatterns": ["systemctl restart", "docker rm", "apt upgrade"]
}
```

LangGraph 负责暂停和恢复；CommandPolicy 负责判断每条命令。

---

## 16. 安全边界

本项目明确允许执行任意命令，因此安全重点不是命令白名单，而是控制入口。

最低要求：

1. Backend API 不能无认证公开。
2. Web UI 和管理 API 使用 Cloudflare Access。
3. MCP 使用可验证的用户身份或 OAuth。
4. Redis 只允许 TLS 连接，不向公网暴露自建 6379。
5. VACPS 使用专用 Linux 用户。
6. 是否允许 root 由 VPS sudoers 配置决定。
7. 所有执行命令必须记录。
8. Token、API Key 和模型密钥不得写入日志。
9. Backend 的 `cwd` 必须是绝对路径。
10. 请求体、输出和执行时间必须有限制。

第一版可以给 `agent` 用户配置 NOPASSWD sudo，但这意味着通过平台下发的任务具备完整 root 权限，必须确保 Cloudflare 和 Backend Token 没有泄露。

---

## 17. 故障与恢复

### 17.1 Backend 离线

创建任务时 Backend 不可达：

```text
Task 状态 = dispatch_failed
```

第一版不在 Cloudflare 中保存离线待投递队列。

用户可以在 Backend 恢复后点击重试。

### 17.2 Redis 临时断线

- Backend API 返回队列不可用；
- Worker 自动重连；
- Task 保持 `dispatch_failed` 或 BullMQ 原状态；
- 不丢失已写入 VPS SQLite 的最终结果。

### 17.3 Redis 数据丢失

- BullMQ 队列和 Scheduler 可能丢失；
- D1 中的 Schedule 定义仍存在；
- Cloudflare Schedule Reconcile 重新创建 Job Scheduler；
- 已执行任务的完整结果仍在 VPS SQLite。

### 17.4 Worker 重启

- BullMQ 负责重新判定未完成 Job；
- LangGraph 使用 SQLite checkpoint 恢复；
- Shell 命令是否可恢复取决于命令自身；
- 第一版对中途终止的命令标记失败，不盲目自动重跑。

### 17.5 Cloudflare 暂时不可用

- 已存在的 BullMQ Scheduler 继续触发；
- VPS Worker 继续执行队列中的任务；
- Web UI 和 ChatGPT 暂时无法创建或查询新任务。

---

## 18. 项目代码结构

```text
vacps/
├── apps/
│   ├── control-worker/
│   │   ├── src/
│   │   │   ├── index.ts
│   │   │   ├── mcp/
│   │   │   ├── api/
│   │   │   ├── registry/
│   │   │   ├── schedule-reconcile/
│   │   │   └── auth/
│   │   ├── web/
│   │   ├── migrations/
│   │   └── wrangler.jsonc
│   │
│   └── vacps/
│       ├── src/
│       │   ├── server/
│       │   ├── queue/
│       │   ├── graph/
│       │   ├── pi/
│       │   ├── executor/
│       │   ├── policy/
│       │   └── storage/
│       └── systemd/
│
├── packages/
│   └── contracts/
│       ├── task.ts
│       ├── backend.ts
│       ├── schedule.ts
│       └── api.ts
│
├── package.json
└── pnpm-workspace.yaml
```

只保留三个 Workspace：

```text
control-worker
vacps
contracts
```

---

## 19. 技术选型

| 模块               | 技术                                               |
| ------------------ | -------------------------------------------------- |
| 语言               | TypeScript                                         |
| Cloudflare Runtime | Workers                                            |
| MCP                | MCP TypeScript SDK / Cloudflare Remote MCP Handler |
| Web UI             | React 或轻量 SPA                                   |
| 控制面数据库       | Cloudflare D1                                      |
| Backend HTTP       | Fastify                                            |
| 队列               | BullMQ                                             |
| Redis Client       | ioredis                                            |
| Redis              | Redis Cloud                                        |
| Task Graph         | LangGraph.js                                       |
| Graph Checkpoint   | SQLite                                             |
| 本地数据库         | SQLite                                             |
| Agent Runtime      | Pi CLI/SDK Adapter                                 |
| 参数校验           | Zod                                                |
| 日志               | Pino                                               |
| 进程管理           | systemd                                            |
| 私有入口           | Cloudflare Tunnel                                  |
| 包管理             | pnpm                                               |

---

## 20. 部署单元

### 20.1 Cloudflare

一个 Worker 项目：

```text
control-worker
```

Bindings：

```text
D1
Static Assets
Worker Secrets
Cron Trigger
```

公开路径：

```text
https://agent.example.com/
https://agent.example.com/api/*
https://agent.example.com/mcp
```

### 20.2 每台 VPS

一个代码包、一个 systemd 服务：

```text
vacps.service
```

运行内容：

```text
HTTP API
BullMQ Worker
LangGraph
Pi
ShellExecutor
SQLite
```

环境变量：

```text
BACKEND_ID
BACKEND_SHARED_TOKEN
LISTEN_HOST=127.0.0.1
LISTEN_PORT=3100
REDIS_URL=rediss://...
DATABASE_PATH=/var/lib/vacps/agent.db
LOG_DIR=/var/lib/vacps/logs
WORKER_CONCURRENCY=1
PI_COMMAND=pi
DEFAULT_PROFILE=full
```

通过 Cloudflare Tunnel 发布 Backend：

```text
https://la-agent.example.com
→ http://127.0.0.1:3100
```

---

## 21. 第一版实现范围

必须完成：

- Cloudflare Remote MCP；
- Cloudflare Web UI；
- Backend 手动添加、编辑、删除和连接测试；
- Backend 状态页面；
- Shell 任意命令任务；
- Pi Agent 任务；
- BullMQ Queue；
- BullMQ Job Scheduler；
- LangGraph 五节点基础流程；
- D1；
- VPS SQLite；
- 任务日志；
- 任务取消；
- Schedule CRUD；
- Schedule 对账；
- `full` Profile；
- Cloudflare Access；
- Backend Bearer Token；
- systemd 部署。

第一版不实现：

- 多租户；
- 计费；
- 可视化 DAG 编辑器；
- 复杂 RBAC；
- 命令白名单；
- 人工审批 UI；
- WebSocket 实时终端；
- 容器沙箱；
- 自动发现 VPS；
- 自动升级 Agent；
- PostgreSQL；
- Kubernetes。

---

## 22. 推荐实现顺序

### 阶段 1：VACPS 最小闭环

```text
/health
POST /tasks
BullMQ
LangGraph
ShellExecutor
SQLite
```

验收：能够通过 HTTP 创建 Shell 任务并查询结果。

### 阶段 2：Pi Agent

```text
PiRuntime
exec Tool
FullAccessPolicy
命令审计
```

验收：自然语言任务能够自主调用多条命令并输出报告。

### 阶段 3：Cloudflare 控制面

```text
D1
Backend CRUD
Task API
Web UI
```

验收：可以从页面添加 VPS、查看状态并创建任务。

### 阶段 4：MCP

```text
tasks.*
backends.*
schedules.*
```

验收：ChatGPT 能创建任务、查询状态和读取结果。

### 阶段 5：定时任务与恢复

```text
Job Scheduler
Schedule CRUD
Reconcile Cron
Redis 丢失恢复
```

验收：定时任务可创建、修改、删除，并能从 D1 重建。

---

## 23. 验收标准

### Backend

- 新 Backend 可以在 Web UI 中手动添加；
- 连接测试能返回版本、Pi、Redis 和 Worker 状态；
- Offline Backend 有明确状态；
- Backend 服务重启后可恢复。

### Shell

- 可以执行任意多行 Shell 命令；
- 可以设置 cwd 和超时；
- stdout、stderr 和 exit code 可查询；
- 运行中的命令可以取消。

### Agent

- Pi 可以通过统一 exec Tool 执行命令；
- 每条命令都有记录；
- Agent 最终报告可查询；
- Profile 字段贯穿完整链路。

### BullMQ

- 任务异步执行；
- MCP 创建任务快速返回 taskId；
- 并发可配置；
- 定时任务可以正常触发；
- Redis 历史记录受限清理。

### LangGraph

- 每个 Task 使用 taskId 作为 thread_id；
- Graph 当前节点可查询；
- checkpoint 写入 SQLite；
- Worker 异常后任务状态可判断；
- 后期可插入 approval interrupt 而不修改外部 API。

### Cloudflare

- Web UI、API 和 MCP 使用同一域名；
- D1 保存 Backend 和 Schedule；
- Web UI 可以查看多台 VPS；
- Schedule Reconcile 可以重建 Redis Scheduler；
- Backend Token 不暴露到浏览器。

---

## 24. 最终职责边界

```text
Cloudflare Workers
负责用户入口、MCP、Web UI、Backend 注册和配置

D1
负责控制面的永久定义和索引

VACPS Backend
负责接收目标节点任务并操作本节点队列

BullMQ
负责何时执行、排队顺序、定时和并发

LangGraph
负责一次任务从准备到完成的状态流程

Pi
负责自然语言任务的分析和命令选择

CommandPolicy
负责判断每条命令是否允许

ShellExecutor
负责实际执行任意命令

SQLite
负责本节点完整任务记录、日志索引和 checkpoint

Redis
负责 BullMQ 的运行时状态
```

最小运行路径：

```text
ChatGPT / Web
      ↓
Cloudflare Worker
      ↓
VPS Backend
      ↓
BullMQ
      ↓
LangGraph
      ↓
Shell 或 Pi
```

后期权限扩展路径：

```text
Pi 生成命令
      ↓
CommandPolicy
      ├── allow → ShellExecutor
      ├── deny → 返回错误
      └── approval_required → LangGraph interrupt
```

---

## 25. 官方参考资料

- Cloudflare Remote MCP Server  
  https://developers.cloudflare.com/agents/model-context-protocol/guides/remote-mcp-server/

- Cloudflare Workers Static Assets  
  https://developers.cloudflare.com/workers/static-assets/

- Cloudflare Workers SPA Routing  
  https://developers.cloudflare.com/workers/static-assets/routing/single-page-application/

- Cloudflare Workers Storage Options  
  https://developers.cloudflare.com/workers/platform/storage-options/

- BullMQ 官方文档  
  https://docs.bullmq.io/

- BullMQ Workers  
  https://docs.bullmq.io/guide/workers

- BullMQ Job Schedulers  
  https://docs.bullmq.io/guide/job-schedulers

- LangGraph.js Overview  
  https://docs.langchain.com/oss/javascript/langgraph/overview

- LangGraph.js Persistence  
  https://docs.langchain.com/oss/javascript/langgraph/persistence

- LangGraph.js Interrupts  
  https://docs.langchain.com/oss/javascript/langgraph/interrupts
