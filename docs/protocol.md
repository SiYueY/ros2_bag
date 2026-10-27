# ros2_bag 通信协议详细设计

| 属性 | 内容 |
|---|---|
| 文档版本 | v0.1.7 |
| 文档状态 | **DRAFT — V1.0 开发阶段，随时修订；规范尚未批准用于完整实施** |
| 适用范围 | Recorder Runtime 的 HTTP/1.1、JSON、SSE 跨语言协议 |
| 架构依据 | docs/architecture.md（开发稿，可直接联动修改） |
| 产品需求 | docs/requirements.md（开发稿，可直接联动修改） |
| 领域依据 | docs/recorder.md（开发稿，可直接联动修改） |
| 技术选型 | `docs/system_technology_selection.md`；Drogon、HTTPX、httpx-sse |
| 下游文档 | `docs/server.md`、`docs/client.md`；CLI/TUI/Web 的 Application 契约在对应实现前补齐，可统一或按应用拆分 |

# 1. 协议范围与通用约定

## 1.1 定位、权责与非目标

Protocol 是 **Recorder–Server–Client–Application** 四层架构之间的 HTTP/JSON/SSE 规范，**不是第五个运行时组件**。Server 负责请求校验、访问控制、队列调度、C++ 领域调用和结果映射；Client 负责 HTTPX/httpx-sse 消费与重连；Recorder 管理录制任务、Topic、Writer、Manifest、Recovery 和资源安全的权威事实。

本文件定义外部可观察的路由、方法、请求与响应、标识域、结果、错误码、只读查询、状态与 SSE 同步、安全权限、协议演进和可执行验证契约。**不得**将 Recorder C++/ROS 2 类型、MCAP 原始 Payload、Drogon 对象、Python SSE Parser 类型或本地文件操作作为协议本身暴露。

V1.0 不提供：通用 Operation Registry、异步任务调度 API、幂等重放服务、持久化 SSE 事件日志、ROS Topic 数据转发、WebSocket/gRPC、录制文件下载/修改/删除 API、任意文件系统浏览和跨 Recorder 分布式协调。历史录制列表/详情为**只读元信息**，不是云端存储管理接口。Client/UI 断线或退出绝不自动 Stop Recorder。

跨层优先级：`architecture.md` 决定职责/生命周期，`requirements.md` 决定 MUST/AC，`recorder.md` 决定领域完成和持久化事实，本文决定**如何在网络上表达**；发现冲突时直接同步更新相关文档并检查测试；不能单靠改变 HTTP 状态码降低领域承诺。

## 1.2 协议基础、版本与字段编码

| 项目 | V1.0 契约 |
|---|---|
| 传输 | HTTP/1.1，TCP；远程访问必须受 TLS 保护；默认仅限本机监听 |
| 控制与查询 | JSON，`Content-Type: application/json; charset=utf-8`；UTF-8 |
| 通知 | HTTP SSE，`Content-Type: text/event-stream; charset=utf-8` |
| 认证 | 所有 Runtime HTTP/SSE 使用 `Authorization: Bearer <opaque-token>`；Token 对应 `read` 或 `control` scope，`control` 包含 `read` |
| API Prefix | `/api/v1`；该主版本决定路由与不兼容变更边界 |
| JSON 版本 | 顶层 `api_version: "1.0"`；不等同 Runtime/Recorder/Schema 版本 |
| 请求 ID | 每次 HTTP 请求独立的 `request_id`，用于传输诊断 |
| 控制操作 ID | `operation_id`，客户端提交前生成的 UUID；用于本次变更相关性，不保证持久结果查询或自动去重 |
| 时间 | RFC 3339 UTC 字符串，如 `2026-10-25T10:11:12.345Z`；缺失值为 `null` |
| 时间间隔 | 按字段明确使用 seconds/milliseconds，避免无单位浮点数 |
| 64-bit 整数 | 可能超过 JavaScript 精确整数范围的 generation、revision、Topic/Epoch 身份及累计计数统一用**十进制非负字符串**；不混用 JSON 浮点数 |
| 小型有界数量 | Topic 数、队列深度、分页 limit/offset 等用 JSON 整数，必须有配置或协议上限 |
| 枚举 | 以文档大写值为规范值；未知枚举不得默认解释为成功或安全状态 |
| 未知值 | 未采集/不可判定为 `null` 并配有可用性/状态字段；`0` 是真实零值 |
| 错误可见性 | 错误码稳定且机器可读，用户消息安全、可定位；HTTP 错误与领域操作结果分开 |

JSON 对象属性无顺序语义。Client 必须忽略当前主版本中**新增且不影响安全的可选字段**；不得把缺失的必填字段、未知控制结果、未知安全状态或额外成功枚举静默处理为 SUCCESS。JSON number 对 NaN、Inf 不合法；时间字符串须实际解析，不允许宽松本地时区猜测。 **请求和响应的扩展规则不对称**：Server 对所有 POST JSON 采用 closed schema，未知属性、重复 key、错误大小写、为其他 mode 提供的专属字段均返回 INVALID_ARGUMENT/MALFORMED_JSON，不允许“忽略后继续执行”；GET query 同样拒绝未知参数。Client 对响应才允许忽略同一 api major 中明确属于可选扩展的未知字段，但未知必填枚举/安全状态仍失败。

字符串命名：JSON 字段统一 `snake_case`；枚举按定义使用大写；路径/URI 和 Topic 名称分属不同语法。所有 JSON 字段、查询键和自定义 Header 都由 Server 设置长度、深度与请求体上限；Server 资源预算和实际限值由 `server.md` 量化，不在本规范虚构并发吞吐保证。

## 1.3 标识域：不可混用的身份

| 身份 | 创建方与有效域 | 是否持久化 | 使用规则 |
|---|---|---|---|
| `runtime_instance_id` | Runtime 启动时生成 UUID；直到该 Runtime 进程终止 | 否 | 检测 Server/Recorder 同进程重启；不能等于录制任务 ID |
| `recording_id` | Start 请求提供，Recorder 在输出根目录保证不覆盖 | 是 | 识别录制任务及历史查询；安全单层名称 |
| `recording_generation` | Recorder 在任务开始分配；关联实例内 Task 世代 | Recorder 领域按其契约 | 与 Runtime/recording_id 联合识别当前任务；不是状态版本 |
| `topic_id`、`topic_epoch` | Recorder | Topic Event 按领域规则持久化 | 标识同任务内 Topic 与订阅创建世代，同名重添加必须有新 Epoch |
| `operation_id` | Client 在提交变更前生成 UUID；Server 校验并传递 | **仅 Topic 事件按 Recorder 规则持久化** | 关联一次请求；同 ID 重发**不构成幂等保证** |
| `request_id` | Server 接收到每个 HTTP 请求时生成或校验透传 | 一般仅日志 | 每次 HTTP 尝试不同；与 operation_id 独立 |
| `state_revision` | Server 状态发布协调器；随观测到的完整状态变化递增 | 否 | 只在同一 `runtime_instance_id` 内可比较；不是 Recorder 领域世代 |
| `SSE event id` | Server 为连接内每个已发送的业务 SSE 帧分配 | 否 | 传输事件身份；不是 state_revision，也不承诺 Last-Event-ID 重放 |

**唯一性及格式：** `runtime_instance_id`、`operation_id`、`request_id` 使用规范小写 UUID。每一个**新的逻辑状态变更意图**必须生成新的 operation_id；UNKNOWN 后用户显式决定“再执行一次”属于新操作，必须使用新 UUID，同时保留旧 OperationAttempt 供诊断/Topic Evidence。SDK/用户不得复用旧 ID 期待服务端去重。网络层不提供 Idempotency-Key 或通用去重 Registry；检测到同一个 recording_id 下 operation_id 被用于不同 Topic/Epoch/操作种类时，Topic Evidence 必须判冲突/UNCERTAIN。`SSE event id` 另有连接内格式。`recording_id` 固定为 ASCII `^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`（1～64 字节、区分大小写），与 Recorder 单层任务目录完全一致；不允许点号、斜杠、反斜杠、百分号或 Unicode。路由参数只做一次标准 URL percent-decode，随后按该规范校验，禁止二次 decode/normalize。HTTP 不能提交物理 output_root。

**请求目标保护：** 所有变更请求必须携带 `expected_runtime_instance_id`；除了 Start，其余还必须携带 `recording_id` 与 `recording_generation`。Server 在**执行领域变更之前**再次验证目标仍匹配（不能只在 HTTP 入队时检查）；不匹配返回 `STALE_RUNTIME` 或 `STALE_RECORDING`，不得将请求“自然应用到当前另外一个任务”。Client 可以先 GET Status 再取得目标保护字段；Server 不因此新增全局注册表。

## 1.4 HTTP 请求、响应与通用行为

所有控制变更使用 **POST JSON**，不通过 GET 或浏览器表单隐式改变录制状态。GET 仅用于只读数据和 SSE；Server 不重写 Manifest，不读取任意本地文件路径，不暴露文件删除、下载或恢复触发接口。

HTTP 成功/失败语义采用明确的顶层信封。查询响应：

```json
{
  "api_version": "1.0",
  "request_id": "3fc3edb4-6b31-4c20-b2d0-309b53f47403",
  "data": {}
}
```

控制响应在同一信封基础上增加 `operation_id` 和 `outcome`：

```json
{
  "api_version": "1.0",
  "request_id": "3fc3edb4-6b31-4c20-b2d0-309b53f47403",
  "operation_id": "d17356c0-3888-4444-b00f-1938f2b7f206",
  "outcome": "SUCCESS",
  "data": {
    "recording_id": "run_001",
    "runtime_instance_id": "9815222c-f39d-4d45-8d1a-bc7f68e75af6",
    "recording_generation": "12"
  }
}
```

| `outcome` | 确切含义 | 允许推断什么 |
|---|---|---|
| `SUCCESS` | 本次 Recorder 领域操作达到对应方法的完成边界，Server 收到可信成功结果 | **本次操作**成功；不表示未来状态永不变化 |
| `FAILURE` | Server 明确拒绝执行，或 Recorder 返回已知的操作失败/领域错误 | 本次操作没有得到成功确认；**不保证完全无副作用** |
| `UNKNOWN` | Server 已无法证明这次操作是否完成，或调用方没有得到可信同步结果 | **不能**推断成功，也不能仅据当前 Recorder 状态推断失败 |

控制成功默认 HTTP 200；如果处于排队、等待 Writer 或写 Manifest 中，**不能先返回 200/SUCCESS**。V1.0 不提供默认 202 Accepted 异步查询流程；调用可能在 HTTP 等待期间完成、失败或仍在执行。Server 应设置可控的接收/排队/执行时间预算，但网络侧请求超时不自动停止 Recorder 的原生操作。

错误信封和 HTTP 映射见 §4。HTTP 连接在请求期间中断时，Client 没有服务器可信回执，无论它认为请求已经写入 socket 与否，均应将**可能已发送的变更**判为 UNKNOWN；“连接建立前确定未发送任何字节”的失败才可以作为本次尝试未提交处理。Client 不能默认重试有副作用的 POST。

**身份回显不等于已执行：** `operation_id` 在拒绝受理、执行失败、同步成功或 UNKNOWN 回应中均可回显，用于日志定位，但不代表 Recorder 已实际开始对应领域调用。`request_id` 标识这次 HTTP 尝试而非逻辑任务。对于请求体尚未合法解析或未认证的情况，可以省略 operation_id，不能为了回显信任任意未验证输入。

## 1.5 协议错误与运行环境条件

HTTP GET、POST 控制与 SSE 共用清晰的版本/授权/过载语义。Server 启动失败、尚未完成 Recovery 或输出根目录所有权不安全时，**不开放状态变更接口**；能够回应诊断时使用受限且不会泄露路径/凭证的错误响应。Recorder 内部定时 Stop、磁盘保护和 Writer 故障不是 HTTP 发起的操作，但会反映在 GET Status/SSE 的当前状态里。

对外状态模型是 Reporter 的**权威观察快照**，不是 Manifest 文件的替代、不是持久操作结果表。HTTP Status 可以由 Server 取得 Recorder 快照并映射；SSE 由有界采样与变化通知驱动。HTTP 当前状态的版本必须由 Server 发布协调器统一管理，不能让并发 GET 在背后独立制造一套不一致的 revision。

不得在 Cookie/浏览器会话中隐式推定 Recorder 控制面已获授权。Web 必须通过其 Server 后端访问同一 Protocol/Client，Web 会话与 CSRF 由 Web 单独承担。V1.0 Runtime API 的认证方案已由 OPEN-04 裁定为预共享不透明 Bearer Token + `read/control` scope；远程访问强制 TLS，部署与攻击验证见 §6.1、`server.md` 与 AC-18。

# 2. HTTP 资源与状态变更操作

## 2.1 V1.0 端点目录

| Method | Path | 语义 | 权限 | 对应责任 |
|---|---|---|---|---|
| GET | `/api/v1/runtime` | Runtime 身份、协议能力与只读服务可用性 | read | Server |
| GET | `/api/v1/status` | 当前权威 Recorder Snapshot | read | Server 映射 Recorder |
| GET | `/api/v1/topics/available` | 可用 ROS Graph Topic 列表/过滤 | read | Recorder 发现 |
| GET | `/api/v1/recordings` | 历史录制列表（含当前任务记录） | read | Recorder 历史索引 |
| GET | `/api/v1/recordings/{recording_id}` | 指定历史任务摘要和分段 | read | Recorder 历史索引 |
| GET | `/api/v1/recordings/{recording_id}/topic-operations/{operation_id}` | **仅 Topic Add/Remove** 的持久化事件证据查询 | read | Recorder 只读事件证据 |
| POST | `/api/v1/control/start` | Start Normal/Timed/Rolling | control | Recorder |
| POST | `/api/v1/control/stop` | Stop 当前受身份保护的 Task | control | Recorder |
| POST | `/api/v1/control/pause` | Pause 当前受身份保护的 Task | control | Recorder |
| POST | `/api/v1/control/resume` | Resume 当前受身份保护的 Task | control | Recorder |
| POST | `/api/v1/control/topics/add` | Add Topic 到指定任务 | control | Recorder/TopicManager |
| POST | `/api/v1/control/topics/remove` | Remove 指定 Topic Epoch | control | Recorder/TopicManager |
| GET | `/api/v1/events` | SSE 初始全量快照与后续变化 | read | Server 订阅与状态发布 |

**所有 GET 路由均为 Recorder 领域只读操作**：不得调用 Recorder 的任何变更方法，不得触发录制状态转移、Recovery、Cleanup、Manifest/Topic Event 写入或磁盘文件修复。`GET /api/v1/events` 可以在 Server 网络层创建/释放**有界订阅资源**，`GET /api/v1/status` 可以触发**只读状态采样**，这两者不属于 Recorder 领域变更；不得把“HTTP GET 安全”误解为完全没有网络内存与 CPU 消耗。`/api/v1/recordings/{recording_id}` 的参数在路由解码前后均必须经过路径安全校验。未知路由为 404；已知路径但 Method 不允许为 405，携带规范 `Allow`。控制必须检查权限和 Server 关闭状态，即使 Recorder 自身当前允许该操作。

端点为 V1.0 的**最小完整集合**；不另造 `/api/v1/tasks`、`/api/v1/operations`、`/api/v1/files` 等重叠抽象。未来兼容性扩展须经 §6 变更评审，不得让一个控制动词同时存在两个效果不同的路径。

## 2.2 Start：创建唯一活动录制任务

`POST /api/v1/control/start` 的最小请求：

```http
POST /api/v1/control/start HTTP/1.1
Content-Type: application/json

{"operation_id":"d17356c0-3888-4444-b00f-1938f2b7f206","expected_runtime_instance_id":"9815222c-f39d-4d45-8d1a-bc7f68e75af6","recording_id":"run_001","mode":"NORMAL"}
```

| 字段 | 约束 |
|---|---|
| `operation_id` | 必填，UUID；相关性 ID 而非去重键 |
| `expected_runtime_instance_id` | 必填，防 Runtime 进程重启后旧请求被误应用 |
| `recording_id` | 必填，ASCII `^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`；已存在的历史目录不可覆盖 |
| `mode` | 必填：`NORMAL / TIMED / ROLLING` |
| `duration_seconds` | 仅 TIMED：正数，单位秒；Recorder 以 steady time 计算 deadline，暂停仍计入期限。deadline 到达先关闭本 Task Generation 的新消息 ingress，截止前已通过准入的消息仍可 Drain，任务最终 IDLE/FAILED 可以晚于倒计时 0；UI 不得把倒计时 0 当 Stop 已完成 |
| `segment_duration_seconds` | 仅 ROLLING 可指定，正数；缺省取 Recorder 已验证配置。它是目标轮转周期，不是物理 Segment 的硬实时最大时长；高优先级控制/安全 Drain 可使轮转延迟 |
| `retention_duration_seconds` | 仅 ROLLING 可指定，正数；缺省取 Recorder 已验证配置 |

Start POST 只允许改变本任务的 mode/duration/rolling segment+retention；Writer/Pool/Queue/Disk/Executor、Compression、QoS 默认和 TimestampPolicy 都来自 Runtime 已验证配置，**不得由 HTTP 覆盖**。请求出现 `compression`、`timestamp_policy`、原始 MCAP YAML、插件路径、output_root 或 QoS 默认 Profile 等禁止字段时返回 INVALID_ARGUMENT，而不是静默忽略。`NORMAL` 禁止 TIMED/ROLLING 专属字段；`TIMED` 强制正 `duration_seconds`；`ROLLING` 的两个时长可缺省并由 Recorder Runtime 默认补齐。

**Start 成功响应的有效配置语义：** `effective_config` 是本任务不可变配置的唯一协议表达：mode 必填；仅 TIMED 的 duration_seconds 为正数；仅 ROLLING 的 segment/retention 为正数；其他不适用字段为 null，不能用 0 作为哨兵。将来如果某字段需要合法数值 0 的不同语义，必须另行定义，不能与 `null` 混用。`effective_config` 必须直接使用 Recorder StartResult 中的 EffectiveTaskConfig，而不是 Server 复制请求。`compression` 始终报告 Runtime StorageProfile 在本 Task 冻结后的实际值，禁用为 `NONE`；V1.0 不存在 Start 请求级压缩覆盖。`timestamp_policy` 固定报告 `RECEIVE`。

成功响应（示例）：

```json
{
  "api_version": "1.0",
  "request_id": "3fc3edb4-6b31-4c20-b2d0-309b53f47403",
  "operation_id": "d17356c0-3888-4444-b00f-1938f2b7f206",
  "outcome": "SUCCESS",
  "data": {
    "recording_id": "run_001",
    "recording_generation": "12",
    "runtime_instance_id": "9815222c-f39d-4d45-8d1a-bc7f68e75af6",
    "state": "RECORDING",
    "effective_config": {
      "mode": "NORMAL",
      "duration_seconds": null,
      "segment_duration_seconds": null,
      "retention_duration_seconds": null,
      "compression": "LZ4",
      "timestamp_policy": "RECEIVE"
    }
  }
}
```

成功条件由 Recorder 决定：Writer 与必需持久化记录满足 Start 完成边界、接入已安全生效。HTTP 成功回应丢失后，原 operation_id 保留为 UNKNOWN 诊断身份；若用户明确决定重新 Start，必须生成新的 operation_id，并且既有任务目录仍会使新的 Start 按 RECORDING_EXISTS/状态守卫失败，不能依据目录存在反推前一次请求成功。Start 失败后留下的目录、事件或 Segment 属于 Recorder 恢复证据；Server 无权清除它们“方便重试”。

## 2.3 Stop / Pause / Resume：操作目标与状态守卫

三种请求均需携带相同的身份守卫：

```json
{
  "operation_id": "93ac8a61-69f4-49c8-b3fa-400c4f396499",
  "expected_runtime_instance_id": "9815222c-f39d-4d45-8d1a-bc7f68e75af6",
  "recording_id": "run_001",
  "recording_generation": "12"
}
```

Server 在真正调用 Recorder 前验证 **Runtime 身份、任务 ID、Generation** 三者均匹配；在排队期间任务结束或新任务启动则拒绝过期操作。身份符合不保证状态符合，例如 Recorder 已被内部 Timed Stop 进入 STOPPING，外部 Resume 必须按 Recorder 返回 `INVALID_STATE` 处理。

| Command | 请求完成时的领域边界 | 成功响应关键字段 |
|---|---|---|
| `stop` | 关闭 ingress、Producer/Writer Drain、Writer Close 与必要 Manifest 终态完成 | `state: IDLE`、本次已完成 `recording_id` |
| `pause` | 不再允许新消息接入、暂停归因已发布；已接纳数据可以继续安全写入 | `state: PAUSED`、`paused_reasons` |
| `resume` | 清除本任务 USER 暂停意图，并在磁盘已安全且稳定期满足时确认 `DISK_RECOVERY_PENDING`；活动系统危险原因不由用户请求清除 | 返回真实 `state` 与 `paused_reasons`；只有原因集合为空时才为 RECORDING |

Stop 的 200/SUCCESS 不表示用户还能从 HTTP 重新开始原来的 recording_id，也不能在失败时强行将 INCOMPLETE 标为 COMPLETE。发生 `DRAIN_TIMEOUT` 时，若 Server 确实取得 Recorder 的领域错误结果，可返回 `FAILURE + DRAIN_TIMEOUT`，其 `effects: POSSIBLE`；若网络已经超时且没有可信领域结果，则本次请求是 `UNKNOWN`。Recorder 可能仍为 `STOPPING`，状态不能当成调用结果的替代。

Paused 原因值域固定为 `USER / DISK_CRITICAL / DISK_UNKNOWN / DISK_RECOVERY_PENDING`，使用集合语义。错误响应中的 `details.protection_reason` 表示仍阻止 Resume 的保护锁存原因，不等同于 Status 中最新的 `disk.state`。Pause 在尚无 USER 时增加 USER，即使任务已经因磁盘原因 PAUSED 也可 SUCCESS；已有 USER 再 Pause 返回 `ALREADY_PAUSED / effects:NONE`。`DISK_CRITICAL/DISK_UNKNOWN` 表示对应系统保护仍然生效：可能是当前采样仍危险，也可能是瞬时采样已改善但 `resume_stable_sec` 尚未满足，因此不能把 paused reason 当成最新 `disk.state` 的别名。`DISK_RECOVERY_PENDING` 表示默认 `auto_resume=false` 下危险已经解除并满足稳定期、但 ingress 继续关闭等待人工确认。Resume 可在 USER 或 DISK_RECOVERY_PENDING 存在时调用：移除 USER，并仅在 Recorder 用新鲜安全样本再次确认后移除 DISK_RECOVERY_PENDING；仍生效的 DISK_CRITICAL/DISK_UNKNOWN 永不由 Resume 清除。最终 reasons 为空且 Timed deadline 未触发、Task 未 Stopping/Failed 时才重新开放 ingress。仅有仍生效的系统保护且无 USER/PENDING 时返回 `DISK_UNSAFE / effects:NONE`。Server 不得自行增加、替换或删除任何暂停原因。

**重复操作规则：** 在 Recorder 明确已完成且当前状态非法时返回相应 INVALID_STATE/ALREADY_FINISHED 诊断；“安全上可以再次调用 Stop”不等于对外无条件幂等或自动返回 SUCCESS。Client 不应在请求 UNKNOWN 后自动发第二次 Stop/Pause/Resume。

## 2.4 Topic Add / Remove：类型、QoS 与 Epoch

`POST /api/v1/control/topics/add`：

```json
{
  "operation_id": "77f1d3d1-a292-4d0a-a7de-9098ec9d730a",
  "expected_runtime_instance_id": "9815222c-f39d-4d45-8d1a-bc7f68e75af6",
  "recording_id": "run_001",
  "recording_generation": "12",
  "name": "/robot/joint_states",
  "type": "sensor_msgs/msg/JointState",
  "qos": {"mode": "AUTO"},
  "repeat_policy": {"enabled": false}
}
```

字段要求：`name` 是 ROS 2 绝对 Topic 名，不是文件路径；`type` 可以省略，唯有 Graph 当前发现唯一类型时才能解析。**如果当前 Graph 对同名 Topic 观察到多个类型，则无论请求是否显式提供 `type` 都必须拒绝 `TOPIC_TYPE_CONFLICT`**；V1.0 不允许用请求中的 type 从多类型 Graph 中“选择一个”。显式 `type` 只用于约束唯一可见类型/类型支持必须与调用者预期一致。Recorder 还必须执行任务级 TopicStorageSignature 检查：同一 recording task 内该 name 曾成功绑定不同 type/serialization/schema 时，在任何新 Writer/Subscription/PREPARED 副作用前拒绝。

**TopicAdd 子对象是 closed schema，并具有明确值域：**

- `qos.mode=AUTO` 时 `qos` 只允许 `mode`，不得同时携带 EXPLICIT 字段；Recorder 根据当前 Publisher endpoint profiles 与冻结默认值求兼容配置。
- `qos.mode=EXPLICIT` 必须给出 `reliability=RELIABLE|BEST_EFFORT`、`durability=VOLATILE|TRANSIENT_LOCAL`、`history=KEEP_LAST|KEEP_ALL`。`KEEP_LAST` 必须给 `depth`，范围 **1..10000**；`KEEP_ALL` 禁止携带有意义的 depth（省略或 null）。不支持字段如 deadline/lifespan/liveliness 一律 `INVALID_ARGUMENT`，不能忽略。
- `repeat_policy.enabled=false` 时禁止 `max_messages/max_bytes`；`enabled=true` 时两者都必填，`max_messages` 为 **1..65536** 的 JSON integer，`max_bytes` 为 **1..16777216** 的 JSON integer，并且仍不得超过 Runtime 当前 `snapshot.max_total_bytes` 等更严格预算。超界为 `INVALID_ARGUMENT/effects:NONE`。
- `type`、Topic name、QoS/Repeat 字段还受通用 JSON 字符串/Body 大小限制；Server 与 Client 不自行扩大 Recorder 允许值域。

这些上限是 V1.0 协议安全上界，不等同于性能保证；Recorder 可以因当前 Runtime 更小的资源预算进一步拒绝，但不能静默截断请求。

Add 成功的数据：

```json
{
  "api_version": "1.0",
  "request_id": "c4621998-0243-425e-8cb8-e9034057892c",
  "operation_id": "77f1d3d1-a292-4d0a-a7de-9098ec9d730a",
  "outcome": "SUCCESS",
  "data": {
    "recording_id": "run_001",
    "recording_generation": "12",
    "topic": {
      "topic_id": "7",
      "topic_epoch": "2",
      "name": "/robot/joint_states",
      "type": "sensor_msgs/msg/JointState",
      "state": "ACTIVE",
      "qos": {"reliability":"RELIABLE","durability":"VOLATILE","history":"KEEP_LAST","depth":10}
    },
    "evidence": {"kind":"TOPIC_EVENT","terminal_event":"TOPIC_ADDED"}
  }
}
```

Recorder 完成的 durable `TOPIC_ADDED` 才允许 Server 报此次 Add SUCCESS。Paused 下添加可返回 `state: ARMED`（订阅存在但 ingress 尚未开放），同样要求对应 durable 终态。失败的 `TOPIC_ADD_PREPARED` 不能视为成功；Writer 注册 Schema 但订阅未建立也不构成成功。

`POST /api/v1/control/topics/remove`：

```json
{
  "operation_id": "0089b5af-7391-41c5-aab7-b29b8497fd54",
  "expected_runtime_instance_id": "9815222c-f39d-4d45-8d1a-bc7f68e75af6",
  "recording_id": "run_001",
  "recording_generation": "12",
  "topic_id": "7",
  "topic_epoch": "2"
}
```

Remove 按**具体 Topic Epoch** 执行，不允许只按名称删除“当前最新的同名 Topic”。返回的成功结果至少回显 `topic_id`、`topic_epoch`、`state: REMOVED` 和 durable `TOPIC_REMOVED` 证据类别。旧 Epoch 已接纳数据可仍存在于 Writer Queue，但不能混入新 Epoch。旧 Epoch 已不在当前实例或已被替换时，返回 `STALE_TOPIC_EPOCH` / `TOPIC_NOT_FOUND`，不得删除新的同名订阅。

对 Topic Add/Remove 而言，`operation_id` 须原样传递至 Recorder Topic Event；Server 不得每次进入 Recorder 时重新随机生成它。Client/Server 可以将该 ID 与本次 HTTP 尝试的 request_id 一同记录，但持久化 Topic Event 才是终态证据来源，HTTP 日志并不替代它。

## 2.5 Topic 操作证据的有限只读查询

`GET /api/v1/recordings/{recording_id}/topic-operations/{operation_id}` 是 **V1.0 唯一按 operation_id 提供的查询端点**。它只解释 Recorder `topic_events.jsonl` 中 `origin=EXTERNAL` 且与目标 recording_id、operation_id、Topic ID/Epoch 精确相关的 Add/Remove 事件；`origin=INTERNAL` 的 Stop/Shutdown/Recovery 生命周期事件即使 Topic 相同也不参与查询，不创建通用操作注册表，也不支持 Start/Stop/Pause/Resume 的终态查询。

**已选择路径 A：Recorder 提供专用只读领域证据查询能力，详见 `recorder.md` §1.3、§2.4；Server 不得直接写一个独立的 Topic Events 解析器**。返回一个 bounded 结果对象，不能为了查询一次操作把整个 JSONL 内容暴露给网络；日志扫描、文件读取和历史解析必须在有界的只读工作执行域执行，不可挤占 Writer/ROS 高频路径。

返回结果的 `evidence_status`：

| 值 | 最低持久化证据 | 客户端含义 |
|---|---|---|
| `CONFIRMED_SUCCESS` | 可信完整事件链证实同一 recording_id + operation_id + Topic ID/Epoch 的 ADDED/REMOVED 终态已持久化 | 可确认该 Topic 领域终态/副作用；**不等于原 HTTP 调用获得 SUCCESS 回执** |
| `CONFIRMED_FAILURE` | 可信完整事件链证实同一操作的 ADD_FAILED/REMOVE_FAILED 终态 | 可确认 Recorder 领域终态失败，但不能推断原 HTTP 回执已送达，也不能推断无副作用 |
| `UNCERTAIN` | 仅 PREPARED、终态截断/冲突、持久化错误或可疑不一致 | 不能确认成功或失败；请求仍 UNKNOWN |
| `NOT_FOUND` | 在**完整且可安全检查**的当前任务事件记录中未发现可信该 ID 的任何记录 | 不证明请求从未被执行；可能尚未写入或位于崩溃窗口。**事件文件不可读、损坏、扫描预算不足时不得返回 NOT_FOUND**，必须返回 UNCERTAIN 或 EVIDENCE_UNAVAILABLE |

**状态与证据严格分离：** 发现 `topic.state=ACTIVE`、Recorder STATE=RECORDING、时间窗匹配、相同 Topic 名或 Event Seq 连续，都不能单独把请求结果提升为 CONFIRMED_SUCCESS。多个事件关联同一个 operation_id 但出现不同 Topic 身份、不同操作类型或互相冲突的终态时，一律返回 UNCERTAIN 并记录领域异常。

查询成功 HTTP 200 返回：

```json
{
  "api_version": "1.0",
  "request_id": "c4621998-0243-425e-8cb8-e9034057892c",
  "data": {
    "recording_id": "run_001",
    "operation_id": "77f1d3d1-a292-4d0a-a7de-9098ec9d730a",
    "operation_kind": "ADD_TOPIC",
    "evidence_status": "CONFIRMED_SUCCESS",
    "topic_id": "7",
    "topic_epoch": "2",
    "terminal_event": "TOPIC_ADDED",
    "error": null
  }
}
```

`NOT_FOUND` 是正常可读查询结果（HTTP 200 + `evidence_status: NOT_FOUND`），表示**在本次完整且可安全检查的事件记录中没有找到可信的该 operation_id 证据**，而不是“该操作确定未执行”。Client **不得因此自动重发 POST**；如果 recording_id 本身不存在，才返回 HTTP 404。事件文件损坏、不可读取或受有界扫描上限中断时不得默认 NOT_FOUND，应返回可识别的 `EVIDENCE_UNAVAILABLE` 错误或 UNCERTAIN，避免掩盖缺失证据。**同一进程中已知 TOPIC_ADDED/REMOVED 终态 `fsync` 失败，即使该行可读，也不能据此返回 CONFIRMED_SUCCESS**。重启后必须区分原始 HTTP 请求结果与**现在已恢复验证的持久化领域终态**：文件中存在合法终态并经整个 Event 链和身份验证，可以确认其现存持久化副作用，但无法从文件反推原 fsync 调用曾成功返回。Client 保留原始 HTTP FAILURE/UNKNOWN，不将恢复后的证据视作“当时服务器已返回 SUCCESS”。仅 PREPARED、损坏/冲突/缺失则仍为 UNCERTAIN；若需证明原同步调用回执，需要额外持久操作回执设计，本协议不承诺。

此接口**不是幂等承诺**：在任何 UNKNOWN/UNCERTAIN/NOT_FOUND 情形下，Client 不应自动重发 POST，即使 operation_id 相同。即使找到 CONFIRMED_SUCCESS 也仅证明关联的领域持久终态在查询时可确认，无法保证原 HTTP 得到 SUCCESS 回执或该 Topic 现在仍 ACTIVE（可能已被后续 Remove）。

# 3. 查询 API 与状态数据模型

## 3.1 Runtime 信息与状态查询的一致性

`GET /api/v1/runtime` 用于发现协议版本、Runtime 身份、服务可用性及能力，不输出密钥、真实系统账号、完整挂载路径或任意底层环境变量。

```json
{
  "api_version": "1.0",
  "request_id": "b91f7e64-b58f-4c1f-9bd3-e99db577ec3a",
  "data": {
    "runtime_instance_id": "9815222c-f39d-4d45-8d1a-bc7f68e75af6",
    "readiness": "READY",
    "protocol_version": "1.0",
    "capabilities": {
      "recording_modes": ["NORMAL","TIMED","ROLLING"],
      "topic_events_lookup": true,
      "history_query": true,
      "state_stream": "SSE_FULL_SNAPSHOT",
      "operation_registry": false,
      "sse_replay": false
    }
  }
}
```

`readiness` 只表示 Runtime 对受保护 API 的服务就绪度，例如 `STARTING / READY / STOPPING / DEGRADED`。就绪度不等于录制任务状态；`READY` 也不保证磁盘安全允许 Start。Server 初始化阶段若尚未完成输出目录锁/Recovery，不能因已有 HTTP 监听就声明 READY。

`GET /api/v1/status` 返回第 §3.2 节的**完整 Recorder 状态**，不是只返回“发生变化的字段”。Server 应通过受控的状态发布协调器把 Recorder 状态样本与 `state_revision` 绑定，返回的领域内容、revision、Runtime 身份必须来自**同一已发布状态对象**；并发 HTTP 查询不能独立随意递增版本。**相同 revision 对应相同的规范性领域状态（含任务/Topic/资源状态、业务计数和安全有效性），仅 `observed_at` 等纯采样元信息可更新而不改变 revision**；不得以这一例外修改任何实际领域计数、暂停原因或有效性。

每个 Status 返回 `observed_at`（采样时刻 UTC）和 `sample_validity`（`CURRENT / STALE`）；Server 可以复用尚在约定有效期内的最近权威状态样本，但若超出 Server 配置的新鲜度预算必须主动采样或返回可识别 `STATE_UNAVAILABLE`，不能悄悄把过期缓存当成当前状态。**已经失效的采样不得以新时间戳或心跳“刷新”成 CURRENT**；SSE 的失效通知与重同步见 §5.4。**采样时刻不是 HTTP 响应生成时刻**；RecorderStatus 在各子系统间不是全局瞬时原子快照，须保留 source/采样有效性。

## 3.2 Status JSON：身份、任务、Topic、资源和诊断

外部稳定 Recorder 状态枚举：`IDLE / RECORDING / PAUSED / STOPPING / FAILED`；必须区分无任务时的 `null` 与数字 0/空字符串。Topic 实例使用 `ADDING / ARMED / ACTIVE / REMOVING / REMOVED / FAILED`，只能由 Recorder 确认真实状态，Server 不得从当前 ROS Graph 推断 Topic 已成功加入。

**完整状态对象的基准形态：**

```json
{
  "api_version": "1.0",
  "request_id": "3fc3edb4-6b31-4c20-b2d0-309b53f47403",
  "data": {
    "runtime_instance_id": "9815222c-f39d-4d45-8d1a-bc7f68e75af6",
    "state_revision": "84",
    "observed_at": "2026-10-25T10:11:12.345Z",
    "sample_validity": "CURRENT",
    "recorder": {
      "state": "RECORDING",
      "recording_id": "run_001",
      "recording_generation": "12",
      "effective_config": {
        "mode": "ROLLING",
        "duration_seconds": null,
        "segment_duration_seconds": 60,
        "retention_duration_seconds": 600,
        "compression": "LZ4",
        "timestamp_policy": "RECEIVE"
      },
      "paused_reasons": [],
      "elapsed_wall_seconds": 305.2,
      "elapsed_active_seconds": 300.1,
      "paused_total_seconds": 5.1,
      "last_error": null,
      "secondary_errors": []
    },
    "topics": [
      {
        "topic_id": "7",
        "topic_epoch": "2",
        "name": "/robot/joint_states",
        "type": "sensor_msgs/msg/JointState",
        "state": "ACTIVE",
        "qos": {
          "reliability": "RELIABLE",
          "durability": "VOLATILE",
          "history": "KEEP_LAST",
          "depth": 10
        },
        "matched_publishers": 1,
        "expected_hz": null,
        "actual_hz": 1000.0,
        "frequency_validity": "MEASURED",
        "gap": null,
        "counters": {
          "received": "304000",
          "enqueued": "303990",
          "written_live": "303988",
          "replayed_to_new_segment": "10",
          "dropped_capacity": "6",
          "dropped_allocation_failure": "0",
          "dropped_enqueue_failure": "4",
          "dropped_paused": "0",
          "dropped_stale_generation": "0",
          "write_errors": "0"
        }
      }
    ],
    "topics_removed_recent": [],
    "topics_removed_total": "0",
    "queue": {
      "used_messages": 2,
      "max_messages": 65536,
      "used_bytes": "2048",
      "max_bytes": "67108864"
    },
    "pool": {
      "fixed_capacity": 81920,
      "in_use": 2,
      "fallback_in_use": 0,
      "allocation_failures": "0"
    },
    "cleanup": {
      "cleanup_queue_depth": 0,
      "cleanup_pending_total": 0,
      "cleanup_queue_full_count": "0",
      "active_segment_id": null,
      "last_error": null
    },
    "disk": {
      "state": "NORMAL",
      "sampled_at": "2026-10-25T10:11:12.000Z",
      "valid": true,
      "available_bytes": "10485760000",
      "used_percent": 35.2,
      "failure_count": 0
    },
    "process": {
      "sampled_at": "2026-10-25T10:11:12.000Z",
      "valid": true,
      "cpu_percent_single_core": 18.4,
      "rss_bytes": "134217728",
      "thread_count": 8,
      "fd_count": 42
    }
  }
}
```

该示例是**协议数据形态而不是性能结果**，例如队列占用、CPU、Topic 频率等数字仅为演示。字段族的约束：

| 字段组 | 必须保持的语义 |
|---|---|
| `runtime_instance_id / state_revision` | 联合辨识 Server 发布的状态版本；不能仅比较 revision 的数字 |
| `recorder.state` | 当前 Recorder 稳定状态；IDLE 时 recording_id、recording_generation、effective_config 均为 null |
| `recorder.recording_generation` | Task 域身份，不能替代 SSE 版本；变更任务时 Topic 的相应身份必须一致或显式声明采样不稳定 |
| `elapsed_wall/active/paused` | 与 `recorder.md` 时间概念一致，seconds；未知时 null，不能输出负时长 |
| `recorder.effective_config` | 当前 Task 不可变配置；无 Task 时 null。TIMED 的 duration_seconds 使用 steady clock且暂停计入；ROLLING segment/retention 为实际值；compression/timestamp_policy 为 Recorder 冻结事实。UI 可用 duration_seconds-elapsed_wall_seconds 推算剩余期限，但 0 不表示 Stop 完成 |
| `topics[].topic_id / topic_epoch` | 每个 Topic 的逻辑/实例身份；不同 Epoch 的统计不能合并后伪装为同一实例 |
| `topics_removed_recent` | 当前 Recording Task 最近移除的 Topic 实例**有界列表**，长度由 Recorder 配置 `topics.recent_removed_instances` 约束；淘汰旧条目不影响累计值 |
| `topics_removed_total` | **当前 Recording Task 内**成功移除的 Topic 实例累计次数（含已从最近移除列表淘汰的条目），不包含尚未确认完成的 Remove；新任务有自己的独立累计口径，不把此前任务的数量混入当前任务 |
| `topics[].qos` | 实际选用的 QoS；未决字段用 null/显式 UNKNOWN，不造出兼容性 |
| `topics[].frequency_validity` | `MEASURED / WARMING_UP / UNAVAILABLE`；未测时 `actual_hz=null`，不以 0 Hz 假装有效测量 |
| `topics[].gap` | `true/false/null`；当 expected_hz 缺失、Topic 预热、Recorder PAUSED/REMOVING、Publisher 观测不足或无可评价完整窗口时为 `null`；连续两个**独立完整**评估周期低于 expected×0.5 才变 true，恢复完整周期达到阈值变 false。它不是 HTTP/SSE 丢包，也不表示 Writer 失败；由 Recorder 计算，Server 原样映射，Client 不自行计算。状态变化属于新 `state_revision` 的业务内容。 |
| `topics[].counters.written_live` | 仅 ROS 实时接收消息 Writer 成功调用数 |
| `topics[].counters.replayed_to_new_segment` | 仅 Rolling Snapshot 重放在**新 Segment** 成功写入的次数 |
| `cleanup.cleanup_queue_depth` | 等待项数，不包括 Worker 当前正在处理的单项 |
| `cleanup.cleanup_pending_total` | durable DELETING/cleanup_pending 总量，可含尚未入队和在途 |
| `cleanup.cleanup_queue_full_count` | 满队列导致的累计拒绝，绝不等于删除失败次数 |
| `disk.state` | `NORMAL / WARNING / CRITICAL / UNKNOWN`，安全判断不以旧 sample 数字代替 |
| `process.valid` | 未采样/初始差分不足/关闭时标记 false；CPU、RSS、FD 等不可得数据使用 null |
| `last_error / secondary_errors` | 结构化 ErrorRef；`secondary_errors` 最多 8 项，Recorder 只保留最新有界诊断；不得因列表增长使 Status 无界 |

**ErrorRef V1.0 结构边界：** 非 null ErrorRef 必须包含 `code`、`message`、`phase`、`at`；`code/phase` 为协议已定义 ASCII 枚举/标识且各不超过 64 bytes，`message` 为 UTF-8 且最多 1024 bytes，`at` 为 RFC3339 UTC。可选 `recording_id/topic_id/topic_epoch/operation_id` 必须遵守各自身份格式。`details` 最多 16 个键、编码后最多 4096 bytes；键为受限 ASCII 标识，值仅允许 string/boolean/integer/null 或协议对该 ErrorCode 明确定义的有界小数组，不允许任意递归 JSON。Server 负责在网络边界验证/映射，不能把 C++ exception text、栈、路径或第三方任意对象原样塞入 ErrorRef。

**只有两个 V1.0 规范性写入计数**，不公开含混的 `written` 别名。`written_live + replayed_to_new_segment` 可表示 API 已报告的两类成功写入次数之和，但**不表示独立 Publisher 消息数、没有丢失或 Segment 已 durable COMPLETE**。未采集 CPU 等指标不以 0 冒充采集；未知系统状态不静默归为 NORMAL。

对同一个 `state_revision` 的完整状态对象，应保持字段语义确定。状态发布协调器只在观察到**规范性状态内容变化**时递增 revision；单独更新 `observed_at` 或 SSE 发送时间不构成业务状态变化，`sample_validity` 由 CURRENT 转 STALE 等安全有效性变化则属于规范性状态改变并必须递增 revision。**规范性状态内容包括任务/Topic 状态、暂停与错误原因、磁盘和资源安全状态，也包括采样后的持续字段（例如 `elapsed_wall_seconds`、`elapsed_active_seconds`、计数器和速率）**；后者按 Server 配置的**有界采样与发布节拍**纳入新 revision，不能在每个 ROS 消息回调中更新 revision。可用 1 Hz 作为 `server.md` 的**候选初始状态发布周期**，但它不是本协议已冻结的时延或吞吐指标；具体默认值、上下限和 OPEN-02 验证由 Server 设计确定。单纯刷新 `observed_at` 不递增 revision，也不发送没有业务变化的 `state.changed`；采样只更新元信息而未发布业务变化时，协调器仍必须维护新的有效采样时刻供 GET /status 的新鲜度检查使用。**SSE 长连接如果一直没有领域变化，Server 应在状态新鲜度预算届满前，使用最近一次真正成功的采样发布**同一 revision 的完整 `state.snapshot` **作为新鲜度确认**，只允许 `observed_at` 等纯采样元信息更新；不得以注释心跳替代状态采样确认，也不能为了刷新时刻伪造一次 Recorder 采样。具体最小刷新频率及流量预算交由 `server.md` 量化。与 Recorder 不同子系统的暂时观测差异不可解释为操作历史证据。

**状态对象有界性：** 活跃 Topics 数和 `topics_removed_recent` 均受 Recorder 配置限制；HTTP JSON/SSE 序列化不得遍历全部已完成历史任务或全部 Topic 事件。若因 Recorder 真实状态超出 Server 的安全响应上限，不能默默截断一个声称完整的 Status，应返回 `STATE_TOO_LARGE`、降低可配置上限或进入有诊断的安全状态；**SSE 全量状态也不得截断必需的身份字段**。

**频率与 Gap 字段设计：** `recorder.md` §5.2.1 定义有界频率窗口与 gap；Protocol 统一映射 actual_hz/frequency_validity/nullable gap。Status Topic 必须携带可空的 gap 字段，Server/Client 必须与其 JSON/SSE Schema 同步实现和测试；null 不得推断成 false。provisional 估计值不进入 V1.0 已有 actual_hz 对外语义，预热仍为 null。

## 3.3 可用 Topic 发现 API

`GET /api/v1/topics/available?limit=100&offset=0&name_prefix=%2Frobot` 返回一次 ROS Graph 观察结果。可选 `name` 参数执行精确名称查询，此时应返回明确的“目前未观察到”结果，不应只返回空列表并让 Client 推断 Topic 永不存在。

查询参数：

| 参数 | 规则 |
|---|---|
| `limit` | 默认 100，最大 200；正整数 |
| `offset` | 默认 0；非负整数、受 Server 最大扫描预算约束 |
| `name_prefix` | 可选 ROS Topic 前缀；UTF-8 与长度校验 |
| `name` | 可选完整 Topic 名；与 name_prefix 不得同时给出 |

```json
{
  "api_version": "1.0",
  "request_id": "f1ca8047-4649-4a10-8e9f-b63e4232e5ec",
  "data": {
    "observed_at": "2026-10-25T10:12:00.000Z",
    "items": [
      {
        "name": "/robot/joint_states",
        "observation": "VISIBLE",
        "types": ["sensor_msgs/msg/JointState"],
        "publisher_count": 1,
        "actual_hz": null,
        "frequency_validity": "UNAVAILABLE",
        "recordability": "CANDIDATE",
        "diagnostics": []
      }
    ],
    "limit": 100,
    "offset": 0,
    "has_more": false,
    "snapshot_consistency": "BEST_EFFORT"
  }
}
```

`observation`：`VISIBLE / VISIBLE_NO_PUBLISHER / NOT_FOUND / UNAVAILABLE`。对于列表查询，只枚举可见结果；仅精确 `name` 查询返回 NOT_FOUND（这是观察结果，不是永久事实）。同名多类型时 `recordability: TYPE_CONFLICT`，类型支持缺失 `TYPE_SUPPORT_UNAVAILABLE`，QoS 不兼容可显式 `QOS_INCOMPATIBLE`；不得为了让 UI 显示“可录”而静默选择任意类型。无法采样 Publisher 频率时为 null，不能显示 0 Hz 作为事实。

分页基于**动态 ROS Graph 非原子观察**，offset/limit 不是长期一致的 Snapshot Cursor；两次查询之间新增/移除 Topic 可能导致重复/漏项，Client 可重新扫描。Server 不维护一个长期 Topic 数据库，发现是 Recorder 的只读能力，不由三套 Application 分别调用 ROS Graph 自行推断。

## 3.4 历史录制列表与详情

`GET /api/v1/recordings?limit=100&offset=0&name_prefix=run_&started_after=...&started_before=...`，以及 `GET /api/v1/recordings/{recording_id}?segment_limit=100&segment_offset=0`。查询仅返回允许展示的元信息，不包含 MCAP 原始数据或物理文件内容。路径仅可作为 Recorder 受控输出根目录内的展示信息；远程只读角色是否允许看绝对路径属于安全策略，不应向未授权用户暴露宿主目录布局。

列表参数：

- `limit` 默认 100、最大 200；`offset` 非负且受可配置最大扫描预算限制。
- `name_prefix` 可选，作用于合法 recording_id/name；不得被当作文件系统 glob。
- `started_after / started_before` 可选 RFC 3339 UTC，含义为起始时间范围；只要指定任一时间过滤，缺失 start_time 的任务就从 items 排除，并在 diagnostics 中报告 `unknown_start_time_excluded` 数量，不伪造 1970 年数据。
- 未指定时间过滤时，排序固定为：已知开始时间倒序，时间相同时按 recording_id 字典序；所有 start_time 未知的任务排在已知组之后，并按 recording_id 字典序。
- `has_more` 表示本次只读扫描观察到的后续结果；动态新任务/清理会影响跨页一致性，不能称为全局冻结快照。
- 单任务详情的 `segment_limit` 默认 100、最大 200，`segment_offset` 非负；它只分页**尚未被安全压缩的 Segment 明细**。Recorder 可能把最老的 durable REMOVED 连续前缀压缩为历史汇总，因此详情必须同时返回 `segment_history_summary`，不能把未返回的旧 REMOVED 解释为从未存在。

```json
{
  "api_version": "1.0",
  "request_id": "c4621998-0243-425e-8cb8-e9034057892c",
  "data": {
    "items": [
      {
        "recording_id": "run_001",
        "mode": "ROLLING",
        "recording_state": "INCOMPLETE",
        "start_time": "2026-10-25T09:00:00Z",
        "end_time": null,
        "message_counts": {
          "written_live": null,
          "replayed_to_new_segment": null
        },
        "segment_count": 3,
        "path": null,
        "recovery_status": "EVIDENCE_AVAILABLE",
        "last_error": {"code":"UNEXPECTED_TERMINATION","message":"Recording was interrupted"}
      }
    ],
    "limit": 100,
    "offset": 0,
    "has_more": false,
    "snapshot_consistency": "BEST_EFFORT",
    "diagnostics": []
  }
}
```

历史 `recording_state` 使用 `RECORDING / PAUSED / STOPPING / COMPLETE / INCOMPLETE / FAILED / UNKNOWN`。这是**历史任务持久化/读取视图**，与当前 `recorder.state` 不完全相同；例如某个历史任务为 INCOMPLETE，并不意味着新 Runtime 的 Recorder 现在为 Failed。已删除的 Rolling Segment 在详情中以状态 `REMOVED` 表示，不把它们从历史事实中悄悄抹去。

`GET /api/v1/recordings/{recording_id}` 返回同一 HistoryEntry 加上分页 `segments` 列表。Segment 含 ID、状态（`WRITING / FINALIZING / COMPLETE / DELETING / REMOVED / INCOMPLETE`）、可读相对标识、开始/完成时间及可得消息数；另返回 `segment_page:{limit,offset,has_more}` 与 `segment_history_summary:{segments_created_total,segments_removed_total,removed_segments_compacted_total,compacted_through_segment_id}`，其中累计值使用十进制字符串，high-water mark 无压缩时为 null。已压缩的历史只允许来自 durable REMOVED 连续前缀；不能从 summary 反推出已删除文件仍存在。缺少 Manifest、权限不足或数据损坏时应返回合法 UNKNOWN/诊断或错误；**不能从“文件目录还在”推断 COMPLETE**。

历史读取由 Recorder 域负责，V1.0 每次按查询范围读取 Manifest/Recovery，**默认不维护跨请求缓存**。Server 不重写解析、Recovery 或索引。对同一次历史查询，返回对象必须来自每项任务自洽的已读版本；多个任务间不保证全局同一时刻，动态分页可能重复或漏项。读取文件出错不得影响当前录制数据路径。

# 4. 控制结果、错误映射与请求相关性

## 4.1 受理、执行和领域完成的三个边界

V1.0 的外部变更调用分为**可拒绝的受理阶段、Server 串行化的执行阶段、Recorder 领域完成阶段**。这三个阶段不能用一个 HTTP 200、一个 `request_id` 或连接关闭事件代替。

```text
HTTP POST 到达
  → 版本/认证/权限/参数/大小校验
  → 有界执行队列 admission
       ├─ 未进入队列 → 明确 FAILURE（无 Recorder 变更调用）
       └─ 已进入队列
           → 排队等待；执行前再校验 runtime + task + generation
           → 唯一 Server 变更调用进入 Recorder
           → Recorder 完成
               ├─ 可确认成功 → HTTP 200 + SUCCESS
               ├─ 可确认领域错误 → 对应 4xx/5xx + FAILURE
               └─ 未能确认终态 → HTTP 504 + UNKNOWN（若还能安全应答）
  → 如果传输中断/响应丢失 → Client 将可能已发送的请求视为 UNKNOWN
```

**外部修改必须按受理/进入 Recorder 的顺序可判定，并且有界。** Server 持有一个与 Recorder 一一对应的控制执行域/队列（实现细节见 `server.md`），队列消费顺序与调用进入顺序可审查；并发查询可独立执行，但必须由 Recorder 提供线程安全状态。内部 Timed Stop、Disk Protection、Writer Error 不经 Server 控制队列，仍可能与外部操作交错；Server 不得将排队先后等同于 Recorder 内部事件先后。

**排队超时与执行超时：**

- **尚未执行、且成功从待执行队列移除**：Server 可以返回 `QUEUE_TIMEOUT / FAILURE / effects:NONE`，表示本次领域操作没有被该 Server 发起。
- **是否已进入 Recorder 不可确认**：只能返回 `UNKNOWN`，不能假装“排队超时代表完全未执行”。
- **Recorder 已执行但还没有终结证据**：若仍能应答，HTTP 504 携带 `UNKNOWN / OPERATION_RESULT_UNKNOWN`；Recorder 原生执行**不会因 HTTP 超时自动取消**。
- **Recorder 返回已知 DRAIN_TIMEOUT/EVENT_COMMIT_FAILED 等错误**：`FAILURE` 指 Recorder 未确认该操作成功，并可包含 `effects:POSSIBLE`；不得将失败回执理解为没有写过文件/创建过订阅。
- **内部变更改变任务状态**：只有 Recorder 能裁定外部操作是否实际完成。Server 不能在快照变 IDLE 时补写“Stop 成功”的假回执。

操作关联的日志可记录 `request_id、operation_id、admission、started_at、native completion、response write attempted` 等阶段；**日志不自动成为可查询的操作终态事实**，且响应写入成功不代表 Client 已接收到数据。

## 4.2 错误响应信封与 HTTP 状态映射

失败/未知操作统一返回如下受限信封：

```json
{
  "api_version": "1.0",
  "request_id": "3fc3edb4-6b31-4c20-b2d0-309b53f47403",
  "operation_id": "d17356c0-3888-4444-b00f-1938f2b7f206",
  "outcome": "FAILURE",
  "error": {
    "code": "EVENT_COMMIT_FAILED",
    "message": "Topic operation terminal event could not be committed",
    "phase": "EVENT_COMMIT",
    "effects": "POSSIBLE",
    "retryable": false,
    "details": {
      "recording_id": "run_001",
      "topic_id": "7",
      "topic_epoch": "2"
    }
  }
}
```

`effects` 的值为 `NONE / POSSIBLE / CONFIRMED_PARTIAL`，描述本次调用可能产生的领域副作用范围，不等于最终成功性：`FAILURE + POSSIBLE` 是有效且重要的组合；`UNKNOWN` 不得返回 `effects:NONE`，除非 Server 能证明根本未进入执行（此时应是明确拒绝/FAILURE）。`retryable` 只能表示客户端**可以安全发起新的尝试**的已证明场景；控制变更的 UNKNOWN、EVENT_COMMIT_FAILED、DRAIN_TIMEOUT 默认 `retryable:false`。对于 GET 的暂时性 503 可以建议重试；不等于自动重放 POST。

| HTTP | 规范 Error Code | 请求结果 | 最低语义 |
|---|---|---|---|
| 400 | `MALFORMED_JSON / INVALID_ARGUMENT / UNSUPPORTED_API_VERSION` | FAILURE | JSON 解析、字段缺失、类型/枚举/请求 API 主版本无效；未调用 Recorder |
| 401 | `UNAUTHENTICATED` | POST: FAILURE；GET: 无 outcome | 任意 Runtime HTTP/SSE 缺失、格式错误或无效 Bearer Token；未通过认证的请求不回显未验证 operation_id |
| 403 | `PERMISSION_DENIED` | POST: FAILURE；GET: 无 outcome | 具备身份但无 read/control 所需权限；拒绝执行 |
| 404 | `RESOURCE_NOT_FOUND / TOPIC_NOT_FOUND` | FAILURE（若请求合法） | 未知路径/录制任务/目标 Topic 资源；Evidence NOT_FOUND 不默认映射此码 |
| 405 | `METHOD_NOT_ALLOWED` | FAILURE | 只读端点误用控制方法等 |
| 409 | `INVALID_STATE / ALREADY_PAUSED / ALREADY_FINISHED / STALE_RUNTIME / STALE_RECORDING / STALE_TOPIC_EPOCH / TOPIC_TYPE_CONFLICT / RECORDING_EXISTS` | FAILURE | 领域状态、重复控制、冲突或目标身份不匹配 |
| 413 | `REQUEST_TOO_LARGE` | FAILURE | 超过有界请求体/字段大小 |
| 415 | `UNSUPPORTED_MEDIA_TYPE` | FAILURE | POST 请求的 Content-Type 不支持（GET/SSE 的不可接受 Accept 应为 HTTP 406） |
| 406 | `NOT_ACCEPTABLE` | FAILURE | 客户端声明的 Accept 不支持要求的响应媒体类型 |
| 422 | `UNSUPPORTED_QOS / TYPE_SUPPORT_UNAVAILABLE / SCHEMA_UNAVAILABLE / INVALID_CONFIG` | FAILURE | 已解析的语义不满足 Recorder 类型、Schema、QoS 或参数约束 |
| 429 | `CONTROL_QUEUE_FULL / RATE_LIMITED` | FAILURE、effects:NONE | Server 受理前有界拒绝，无 Recorder 调用；可用 Retry-After |
| 500 | `EVENT_COMMIT_FAILED / STORAGE_ERROR / DRAIN_TIMEOUT / INTERNAL_ERROR` | FAILURE | 已取得 Recorder 的确定领域失败；effects 必须按 Recorder 原样映射，不能用 504 代替 |
| 503 | `RUNTIME_NOT_READY / DISK_UNSAFE / RECORDER_UNAVAILABLE / RECORDER_RESOURCE_EXHAUSTED / EVIDENCE_UNAVAILABLE / STATE_UNAVAILABLE / STATE_TOO_LARGE / QUEUE_TIMEOUT` | FAILURE | 环境安全、Recorder 资源/执行域、恢复/状态证据或服务不可用；`QUEUE_TIMEOUT` **仅表示控制请求已排队但在进入 Recorder 前原子移除成功**，`effects:NONE` |
| 504 | `OPERATION_RESULT_UNKNOWN` | UNKNOWN | **Server 在 HTTP 回复预算内尚未取得 Recorder ControlHandle 的 DONE Result，或可信响应丢失**；不是 Recorder 的领域错误码 |

### 4.2.1 Recorder Error → Protocol 的唯一映射

一旦请求已经进入 Recorder，Server **不得自行重分类 RecorderError**。V1.0 使用下表作为规范映射；`effects` 逐字采用 Recorder Result，只有“确定未进入 Recorder”的 Server 受理错误才由 Server 产生 `effects:NONE`。

| Recorder / Server 原因 | HTTP | Protocol ErrorCode | outcome / effects | retryable |
|---|---:|---|---|---|
| Recorder `INVALID_ARGUMENT` | 400 | `INVALID_ARGUMENT` | FAILURE / Recorder effects（通常 NONE） | false |
| Recorder `TYPE_CONFLICT` | 409 | `TOPIC_TYPE_CONFLICT` | FAILURE / Recorder effects | false |
| Recorder `TYPE_SUPPORT_UNAVAILABLE` | 422 | `TYPE_SUPPORT_UNAVAILABLE` | FAILURE / Recorder effects | false |
| Recorder `SCHEMA_UNAVAILABLE` | 422 | `SCHEMA_UNAVAILABLE` | FAILURE / Recorder effects | false |
| Recorder `UNSUPPORTED_QOS` | 422 | `UNSUPPORTED_QOS` | FAILURE / Recorder effects | false |
| Recorder `INVALID_STATE` | 409 | `INVALID_STATE` | FAILURE / Recorder effects | false |
| Recorder `ALREADY_PAUSED` | 409 | `ALREADY_PAUSED` | FAILURE / NONE | false |
| Recorder `ALREADY_FINISHED` | 409 | `ALREADY_FINISHED` | FAILURE / NONE | false |
| Recorder `RECORDING_EXISTS` | 409 | `RECORDING_EXISTS` | FAILURE / NONE | false |
| Recorder `STALE_RECORDING` | 409 | `STALE_RECORDING` | FAILURE / NONE | false |
| Recorder `STALE_TOPIC_EPOCH` | 409 | `STALE_TOPIC_EPOCH` | FAILURE / NONE | false |
| Recorder `NOT_FOUND` / `TOPIC_NOT_FOUND` | 404 | `RESOURCE_NOT_FOUND` / `TOPIC_NOT_FOUND` | FAILURE / NONE | false |
| Recorder `DISK_FULL` | 503 | `DISK_UNSAFE` + `details.protection_reason=DISK_CRITICAL` | FAILURE / Recorder effects | false |
| Recorder `DISK_STATE_UNKNOWN` | 503 | `DISK_UNSAFE` + `details.protection_reason=DISK_UNKNOWN` | FAILURE / Recorder effects | false |
| Recorder `RESOURCE_EXHAUSTED` | 503 | `RECORDER_RESOURCE_EXHAUSTED` | FAILURE / Recorder effects | false |
| Recorder `EXECUTOR_UNAVAILABLE` | 503 | `RECORDER_UNAVAILABLE` | FAILURE / Recorder effects | false |
| Recorder `WRITER_ERROR` / `STORAGE_UNAVAILABLE` | 500 | `STORAGE_ERROR` | FAILURE / Recorder effects | false |
| Recorder `EVENT_COMMIT_FAILED` | 500 | `EVENT_COMMIT_FAILED` | FAILURE / Recorder effects | false |
| Recorder `DRAIN_TIMEOUT` **作为最终 DONE Result** | 500 | `DRAIN_TIMEOUT` | FAILURE / Recorder effects（通常 POSSIBLE） | false |
| Recorder `GENERATION_VIOLATION` / `POOL_FATAL` | 500 若仍能安全回应，否则连接可能终止 | `INTERNAL_ERROR` | fail-safe / 真实 effects | false |
| Server admission 容量满，尚未调用 Recorder | 429 | `CONTROL_QUEUE_FULL` | FAILURE / NONE | false |
| Server FIFO timeout 且原子证明未 STARTED | 503 | `QUEUE_TIMEOUT` | FAILURE / NONE | false |
| HTTP reply deadline 到达、Handle 尚未 DONE | 504 | `OPERATION_RESULT_UNKNOWN` | UNKNOWN / POSSIBLE | false |

**`DRAIN_TIMEOUT` 与 HTTP 504 严格分离。** 前者是 Recorder 已完成 ControlHandle 并明确给出的领域 FAILURE；后者表示 Server 尚未得到领域终态。Server 不得因为某个本地等待预算到期就制造 Recorder `DRAIN_TIMEOUT`，Recorder 也不得用网络 deadline 决定自身领域结果。**对所有控制 POST，§4.2.1 表中的 `retryable` 规范值统一为 `false`**：即使某个条件稍后可能恢复，也必须由 Application/用户在重新读取当前状态、重新验证 Target 后显式创建新的 OperationAttempt；Server/SDK 不允许依据 ErrorCode 自动重放。GET 查询的 429/503 可以独立返回可退避重试提示，但该语义不得传播到控制 POST。

### 4.2.2 受理前拒绝允许省略 operation_id

对于**确定发生在领域调用之前**的拒绝，Server 可以不回显尚未可信解析/授权的 `operation_id`，典型包括 400 malformed/closed-schema、401、403、405、406、413、415，以及 Server admission 前的 429。Client 将这类响应认定为可信 FAILURE 的条件不是“operation_id 必须匹配”，而是：响应属于当前 HTTP exchange；`api_version`、`request_id`、HTTP status、ErrorCode 组合合法；控制响应明确 `outcome:FAILURE`、`effects:NONE`；该 ErrorCode 在规范中保证未调用 Recorder。**一旦 Server 已成功解析并验证 operation_id 且请求进入 Control FIFO，所有后续控制响应（QUEUE_TIMEOUT、Recorder FAILURE/SUCCESS、504 UNKNOWN）都必须回显完全相同的 operation_id。** 缺失/不匹配时，若请求可能已发送，Client 保守保持 UNKNOWN。

`QUEUE_TIMEOUT` 的规范性组合为 **HTTP 503 / FAILURE / effects:NONE / retryable:false**：它不是 `CONTROL_QUEUE_FULL` 的受理前 429，也不是已可能执行的 `OPERATION_RESULT_UNKNOWN` 504。Server 必须凭队列状态线性化证据证明原生调用未发生；控制 POST 即使获得 QUEUE_TIMEOUT 也不默认允许 Client 自动重试。此状态码与 Outcome 的组合属于当前 Protocol 规范，Server/Client 不可各自解释。

表中 429/503 等应在 JSON 中提供稳定 ErrorCode；当 Recorder 在安全保护中返回明确错误时，HTTP 503 + FAILURE 并不代表“Recorder 线程已停止”或“任务已失败”。**所有 RecorderErrorCode 必须落入 §4.2.1 的已定义映射；新增 Recorder ErrorCode 若没有同步更新 Protocol/Server/Client/测试，编译或契约测试必须失败，不能落入默认 500 后继续发布。** 对于无法确定本次操作是否执行过的队列竞争，不能借 429/503 隐瞒 UNKNOWN。409 中的目标保护冲突表示**本次调用被 Server 在进入领域方法之前拒绝**；如果 Recorder 在调用中检测状态冲突，是否存在副作用则按 Recorder 返回信息如实表达。

`retryable` 属性仅帮助 Client 识别可尝试的**新请求前置状态**，不能自动为任何领域控制操作赋予幂等性。错误消息为安全的高层诊断，不包含 API Key、TLS 私钥、物理文件绝对路径、内部堆栈或 ROS CDR Payload。技术排障使用 request_id 关联受保护日志。

**查询失败返回形式：** GET 查询使用同样的 `error`，但不附带 `operation_id/outcome` 时表示查询资源不可读，不是一个录制控制操作的 FAILURE。SSE 建立前若发生认证或能力错误，使用普通 HTTP 错误响应，不发送半个有效的 SSE 状态事件。

## 4.3 operation_id：关联证据不是幂等去重

所有 POST 需要 Client 提交 UUID `operation_id`，其语义分层：

| 场景 | Server 能确认什么 | 不能确认什么 |
|---|---|---|
| HTTP 已通过校验并排队 | Server 曾受理特定 operation_id | Recorder 是否已执行、是否完成 |
| Recorder Add/Remove 正在执行 | Server 关联该调用与 Topic operation_id | Event PREPARED 是否已经 durable/终态是否 durable |
| Topic Event durable ADDED/REMOVED | 经 recording_id/Topic ID/Epoch/operation_id 完整关联可证明对应操作完成 | Topic 现在是否仍 ACTIVE、是否适合自动重发 |
| Start/Stop/Pause/Resume 原生调用返回成功 | Server 可在本次连接上确认同步结果 | 断线后是否存在持久的可查询请求终态 |
| 发生 HTTP Timeout / 连接关闭 | Client 没收到可信结果 | 不能根据目标状态、时间和 request_id 推断本次调用成功 |
| 重新发送相同 operation_id | 属于调用方协议误用；Server 不承诺去重 | 不得把它当作“继续同一操作”；Topic Evidence 若形成身份冲突必须 UNCERTAIN |

每个 operation_id 只属于**一个逻辑控制意图**；对 Topic Add/Remove，recording_id + operation_id 只能关联一个操作种类和一个目标 Topic 身份。发现该 ID 被复用于其他 Topic、Epoch、操作种类或任务时，Recorder Evidence 返回冲突/UNCERTAIN，不得选取“最近一次”终态。Server 必须维护本次请求从 HTTP body 到 Recorder 原生入参的映射；对 Add/Remove 的持久证据查询依赖 Recorder 事件文件，不强制长期保留 Server 控制队列或请求对象。

**不提供通用 `GET /api/v1/operations/{operation_id}`。** Start/Stop/Pause/Resume 发生 HTTP UNKNOWN 后，允许 Client 重新读取 `GET /api/v1/status` 以说明“现在状态是什么”，但不能更新“这次请求是否成功”的事实；该请求终态持续 UNKNOWN 直到客户端持有原有同步回执或有明确额外请求级终态证据。用户可以显式决定下一操作，Client 不能私自补发“看起来无害”的 Stop/Pause。

## 4.4 断线、取消、重试与状态刷新的共同语义

Client 应独立持有两类数据：

- **OperationResult**：本次变更的 operation_id、请求结果 `SUCCESS/FAILURE/UNKNOWN`、HTTP 状态/领域错误、是否确认副作用。
- **CurrentStatus**：Runtime 当前权威 Recorder 快照、state_revision、observed_at、是否过期。

两者可以同时显示，不相互改写。示例：“Stop 请求结果 UNKNOWN；最新 Recorder Status 是 IDLE”。这句话不推出“本次 Stop 成功”。若收到重连后的 Runtime Instance ID 与旧值不同，所有旧的状态比较应标记为跨世代；未完成操作结果不能被新 Runtime 当前状态自动填补。

| Client 观察到的情况 | 默认处理 |
|---|---|
| 明确 200 + SUCCESS、必填身份正确 | 标记本次操作成功；可随后刷新状态用于显示 |
| 明确 4xx/5xx + FAILURE | 记录失败/副作用范围；必要时只读刷新，不自动反向操作 |
| 明确 504 + UNKNOWN | 记录 UNKNOWN；刷新状态，不重试副作用 POST |
| 已经开始发送请求，读超时/断线 | UNKNOWN；刷新状态或等待连接恢复后刷新 |
| 确认在连接/写入前失败，绝未发送请求 | 记录 NOT_SUBMITTED（客户端本地结果，不是 Server operation outcome），可由用户决定是否首次发送 |
| Topic Add/Remove UNKNOWN | 可只读查询相同 recording_id/operation_id 的 Topic Event 证据；CONFIRMED_SUCCESS/FAILURE 只更新独立 `TopicEvidenceAssessment` 的领域事实判断，**原始 OperationResult 仍保持 UNKNOWN**；UNCERTAIN/NOT_FOUND 不增加确定性 |
| 查询 API 失败 | 只标记状态不可用/过期，不修改已有操作结果 |
| SSE 断开、发现版本缺口或 Server 重启 | 立即标记旧展示状态 STALE，并按 §5 建立新完整基线；不触发 Stop |

CLI 的退出码、TUI/Web 的操作提示与“再次操作”确认由 Application 设计定义；Python Client 的异常类型与同步/异步 API 由 `client.md` 定义。Application 设计可统一或拆分，但不同应用不得对同一次操作得出相矛盾的成功结论。

**HTTPX 适配须保证控制 POST 不被自动重放。** HTTPX 的 `AsyncClient` 构造参数没有 `retries`；其内置连接重试选项属于 `AsyncHTTPTransport(retries=...)`，默认 Transport 不启用额外连接重试。`client.md` 不得使用无效的 `AsyncClient(retries=0)` 示例：需要显式控制时，可配置 `AsyncHTTPTransport(retries=0)`，并检查任何自定义 Transport、第三方 Retry 中间件或应用包装都不会自动重放控制 POST。**SSE 重连是 Client 主动建立新 GET 连接的独立策略，不等于对控制 POST 的重试**；`client.md` 应明确将控制 POST 与 SSE 消费隔离为不同的 HTTPX Client/Transport 实例及各自超时/重连配置，Application 包装器、中间件不得绕过这一边界对控制 POST 自动重放。Protocol 规定**重试行为隔离**而不要求某个具体类名或固定的连接池数量；若实现采用等价共享池方案，必须通过审查证明控制 POST 无自动重放。只有确定未发送并符合 §4.4 的情形，才可能执行**第一次实际发送**，而不是把结果不确定的请求偷偷重试。

# 5. SSE 状态通知、版本和重新同步

## 5.1 状态版本与事件身份：两个不同的序列

`state_revision` 是 Server **完整状态发布协调器**在同一 Runtime 生命周期内的递增序号，从字符串 `"1"` 起始；只有发布的规范性状态内容发生变化才增加。观察时钟 `observed_at` 更新本身不必增加版本。Runtime 重启后分配新的 `runtime_instance_id`，state_revision 可重新从 "1" 开始，必须将二者组合作为状态版本身份。`recording_generation` 不参与 state_revision 的计算，二者**语义不可互换**。

**SSE event ID 独立于状态版本。** 每次 GET /events 建立一个新的 `stream_id`（规范小写 UUID），该连接的业务事件具有从 **1** 开始、严格递增的 `stream_sequence`，采用无符号十进制 ASCII 字符串（不带前导零，首次为 `"1"`）。SSE 帧格式为 `id: {stream_id}/{stream_sequence}`，例如 `id: 93ac8a61-69f4-49c8-b3fa-400c4f396499/1`。此 ID **只标识当前连接内的传输事件**，不是持久 Topic Event Sequence、recording_generation、operation_id，且不保证一个状态变化被投递给所有 Client。Client 应将 SSE ID 作为不透明字符串保存用于日志与诊断，**不依赖解析 sequence 数值来判断状态是否新鲜**；新鲜度只依据 `runtime_instance_id + state_revision`。初始 Snapshot 与变化通知占用 SSE ID；需要发送 `stream.resync_required` 时它也占用事件 ID，注释心跳不占 ID。

Client 对每个完整状态事件比较 `state.runtime_instance_id + state.state_revision`（也即 GET Status `data` 中的同名字段）：
- 同 Runtime、较新的 revision：以整个状态对象替换本地状态，不能以 Patch 方式猜测变化字段。
- 同 Runtime、重复 revision：允许作为新鲜度确认或重同步；**业务字段、身份、资源值和安全有效性必须相同**，仅 observed_at 等观测元信息可以更新。只有成功取得新采样才允许刷新 observed_at；延迟到达的同 revision 快照不得使 Client 的 observed_at 回退，业务字段不同则为 PROTOCOL_VIOLATION。
- 同 Runtime、较旧 revision：视为延迟/乱序状态，不得覆盖较新状态；触发协议诊断，必要时重新订阅。
- Runtime ID 不同：立即废弃旧版比较与旧状态“当前有效”承诺，等待新的完整基线。
- revision 无法解析、状态缺关键字段、payload 声称完整却截断：必须丢弃为无效消息，重建同步，不能把部分状态合并进旧缓存。

因为每个状态通知都携带**完整快照**，某个 Client 错过中间一个或多个 revision 后，如果收到同 Runtime 更新的合法完整版本，可以直接恢复到该新版本的当前状态；这种恢复**不代表中间变化历史被无损重放**。Client 可记录 revision 跳跃用于诊断，但不可把缺口自动解释为 Recorder 错误或 Topic 事件丢失。

## 5.2 SSE 端点、帧格式与事件类型

`GET /api/v1/events` 必须以 HTTP 200 返回 `text/event-stream; charset=utf-8`，禁用中间代理的不受控响应缓存；若认证/容量/Runtime 就绪度检查失败，**在建立 SSE Stream 之前**以标准 HTTP JSON 错误回应。推荐响应头：

```http
HTTP/1.1 200 OK
Content-Type: text/event-stream; charset=utf-8
Cache-Control: no-cache, no-transform
Connection: keep-alive
```

SSE 帧使用标准 UTF-8、LF 行终止，字段采用 `event:`、`id:`、`data:`，帧之间空行；JSON 编码在单个 `data:` 行，换行字符在 JSON 字符串内部正常转义。多行 data 按 SSE 规范合并，不要求 Server 主动切分；Client 必须支持标准 SSE 解析，不得按行直接调用 `json.loads(data_line)` 模拟 SSE Parser。Server 的心跳采用**只有注释、不含 data/event/id 的帧**：

```text
: heartbeat

```

业务事件类型：

| `event:` | `data` | 接收行为 |
|---|---|---|
| `state.snapshot` | 完整 StateEnvelope | 首次订阅/重新同步的**全量基线**，以及无业务变化时**相同 revision + 更新有效 `observed_at`** 的定期新鲜度确认；Client 按完整对象校验，不能把同 revision 当成新领域状态 |
| `state.changed` | 完整 StateEnvelope | 观测到状态变化后的**全量替换**，不是增量 Patch |
| `stream.resync_required` | Error/Reason JSON（不带 Recorder 完整状态） | 本连接不能再保证连续有效状态交付；Client 必须标记 STALE 并重新订阅/GET 状态 |
| 注释心跳 | 无业务 data | Client 不产生业务状态更新 |

**StateEnvelope 结构：** SSE 事件由 `api_version` 与 `state` 两个必填属性组成，`state` **完整复用 GET /api/v1/status 的 `data` 对象**，包含同一 `runtime_instance_id`、`state_revision`、`observed_at`、`sample_validity` 和 Recorder/Topic/资源字段；不在外层重复定义另一个可冲突的版本值。GET 的 `request_id` 是 HTTP 请求专属字段，SSE Envelope 不复制它。

下方是字段完整的有效状态事件示例（零 Topic、进程采样不可用是明确的合法取值）。JSON 仅为演示业务数据，不代表性能实测。

```json
{
  "api_version": "1.0",
  "state": {
    "runtime_instance_id": "9815222c-f39d-4d45-8d1a-bc7f68e75af6",
    "state_revision": "86",
    "observed_at": "2026-10-25T10:11:16.345Z",
    "sample_validity": "CURRENT",
    "recorder": {
      "state": "RECORDING",
      "recording_id": "run_001",
      "recording_generation": "12",
      "effective_config": {
        "mode": "ROLLING",
        "duration_seconds": null,
        "segment_duration_seconds": 60,
        "retention_duration_seconds": 600,
        "compression": "LZ4",
        "timestamp_policy": "RECEIVE"
      },
      "paused_reasons": [],
      "elapsed_wall_seconds": 305.2,
      "elapsed_active_seconds": 300.1,
      "paused_total_seconds": 5.1,
      "last_error": null,
      "secondary_errors": []
    },
    "topics": [],
    "topics_removed_recent": [],
    "topics_removed_total": "0",
    "queue": {
      "used_messages": 0,
      "max_messages": 65536,
      "used_bytes": "0",
      "max_bytes": "67108864"
    },
    "pool": {
      "fixed_capacity": 81920,
      "in_use": 0,
      "fallback_in_use": 0,
      "allocation_failures": "0"
    },
    "cleanup": {
      "cleanup_queue_depth": 0,
      "cleanup_pending_total": 0,
      "cleanup_queue_full_count": "0",
      "active_segment_id": null,
      "last_error": null
    },
    "disk": {
      "state": "NORMAL",
      "sampled_at": "2026-10-25T10:11:12.345Z",
      "valid": true,
      "available_bytes": "10485760000",
      "used_percent": 35.2,
      "failure_count": 0
    },
    "process": {
      "sampled_at": "2026-10-25T10:11:12.345Z",
      "valid": false,
      "cpu_percent_single_core": null,
      "rss_bytes": null,
      "thread_count": null,
      "fd_count": null
    }
  }
}
```

相应 SSE 帧的 `data:` 可以把该 JSON 压缩成单行 UTF-8；下面两帧的 `state` 均包含必要完整字段，示例中 `state.changed` 从 RECORDING 变为 PAUSED：

```text
id: 93ac8a61-69f4-49c8-b3fa-400c4f396499/1
event: state.snapshot
data: {"api_version":"1.0","state":{"runtime_instance_id":"9815222c-f39d-4d45-8d1a-bc7f68e75af6","state_revision":"87","observed_at":"2026-10-25T10:11:18.345Z","sample_validity":"CURRENT","recorder":{"state":"RECORDING","recording_id":"run_001","recording_generation":"12","effective_config":{"mode":"ROLLING","duration_seconds":null,"segment_duration_seconds":60,"retention_duration_seconds":600,"compression":"LZ4","timestamp_policy":"RECEIVE"},"paused_reasons":[],"elapsed_wall_seconds":305.2,"elapsed_active_seconds":300.1,"paused_total_seconds":5.1,"last_error":null,"secondary_errors":[]},"topics":[],"topics_removed_recent":[],"topics_removed_total":"0","queue":{"used_messages":0,"max_messages":65536,"used_bytes":"0","max_bytes":"67108864"},"pool":{"fixed_capacity":81920,"in_use":0,"fallback_in_use":0,"allocation_failures":"0"},"cleanup":{"cleanup_queue_depth":0,"cleanup_pending_total":0,"cleanup_queue_full_count":"0","active_segment_id":null,"last_error":null},"disk":{"state":"NORMAL","sampled_at":"2026-10-25T10:11:12.345Z","valid":true,"available_bytes":"10485760000","used_percent":35.2,"failure_count":0},"process":{"sampled_at":"2026-10-25T10:11:12.345Z","valid":false,"cpu_percent_single_core":null,"rss_bytes":null,"thread_count":null,"fd_count":null}}}

: heartbeat

id: 93ac8a61-69f4-49c8-b3fa-400c4f396499/2
event: state.changed
data: {"api_version":"1.0","state":{"runtime_instance_id":"9815222c-f39d-4d45-8d1a-bc7f68e75af6","state_revision":"88","observed_at":"2026-10-25T10:11:20.345Z","sample_validity":"CURRENT","recorder":{"state":"PAUSED","recording_id":"run_001","recording_generation":"12","effective_config":{"mode":"ROLLING","duration_seconds":null,"segment_duration_seconds":60,"retention_duration_seconds":600,"compression":"LZ4","timestamp_policy":"RECEIVE"},"paused_reasons":["USER"],"elapsed_wall_seconds":307.2,"elapsed_active_seconds":300.1,"paused_total_seconds":7.1,"last_error":null,"secondary_errors":[]},"topics":[],"topics_removed_recent":[],"topics_removed_total":"0","queue":{"used_messages":0,"max_messages":65536,"used_bytes":"0","max_bytes":"67108864"},"pool":{"fixed_capacity":81920,"in_use":0,"fallback_in_use":0,"allocation_failures":"0"},"cleanup":{"cleanup_queue_depth":0,"cleanup_pending_total":0,"cleanup_queue_full_count":"0","active_segment_id":null,"last_error":null},"disk":{"state":"NORMAL","sampled_at":"2026-10-25T10:11:14.345Z","valid":true,"available_bytes":"10485760000","used_percent":35.2,"failure_count":0},"process":{"sampled_at":"2026-10-25T10:11:14.345Z","valid":false,"cpu_percent_single_core":null,"rss_bytes":null,"thread_count":null,"fd_count":null}}}

```

心跳**不是合法业务事件的替代**；Server 不主动发空的 `data:` 作为心跳，因为 httpx-sse 0.4.3 存在需要专项验证的 comment + event ID 解析边界。Client 对不存在规范 `event:` 类型、缺少有效 JSON 的可疑“空事件”，不得视为 state.changed，但也**不能在通用 SSE Parser 内粗暴丢弃所有空数据事件**：标准空 data 事件的解析正确性仍应单独测试。Protocol 的业务事件本来就要求合法的非空 JSON envelope。

## 5.3 首次订阅：注册与完整快照的无空窗衔接

**禁止**“客户端先 GET /status，之后 GET /events，并认为两者之间状态没有变化”。服务端必须在 SSE 注册和初始快照之间形成**原子等价的状态发布连续性**，同时保证状态复制与缓冲都不进入 Recorder 高频回调。

推荐 Server 状态协调器持有最近一次完成的 Recorder 状态发布对象 `PublishedState{runtime_id, state_revision, observed_at, state}`，一个串行发布域负责更新 revision 并广播，订阅创建与发布顺序由同一协调器控制：

1. HTTP 通过 read 权限、资源上限和 Runtime ready 检查；配置新 subscriber 的有界缓冲、stream_id、最后已发送序号。
2. 在状态发布协调器的同步点**先注册订阅者**，并取得该协调器最近一个完整 PublishedState 的不可变引用；如果未有任何有效快照，应触发首次采样完成后再宣称订阅成功。
3. 把上述完整基线入订阅者的**初始待发送位置**，确保其为该连接第一个业务状态事件；订阅注册后发生的新 PublishedState 进入受控后续位置，不得从初始事件之前越过。
4. 注册完成后触发一次或确保即将执行的**订阅后 Recorder Snapshot 重新采样**，消除“上次协调器采样到现在”期间的陈旧窗口；如果 Recorder 已在订阅前改变，新的发布状态将通过后续 `state.changed` 或重建 `state.snapshot` 体现。
5. 在最新采样进入同一发布协调器后，按 state_revision 顺序发送初始全量与更新。采样期间事件到达只占用有界订阅缓冲，不存在跨队列乱序写入。
6. 如初始发布或后续缓冲超限，不能跳过基线后继续发送不完整增量；必须安全**合并成一个更新的完整快照**或向 Client 发出 resync_required/断开连接，由 Client 重新建立基线。

这属于 Server 的状态协作合同，不要求 Recorder 提供通用 EventBus，也不要求 Recorder 为每个 SSE Client 重新计算状态快照。Recorder 内部 Timed Stop、Disk Pause 和 Topic 变化会在状态协调器下一次有效采样时反映；**不保证所有瞬时中间状态都被发布**，但已采样、已发布版本之间不得出现造成永久陈旧展示的订阅空窗。

状态首次到达以前，Client/UI 应展示 CONNECTING/SYNCING，而不是将旧 HTTP Status 当成当前有效。每次新 SSE 连接（包括重连）首先得到新的 `state.snapshot`；即使 HTTP 带有 `Last-Event-ID`，Server 也不以它作为“可靠补发”依据。

## 5.4 慢客户端、缓冲上限、丢帧与重连

**采样成功但状态稳定：** 若业务内容和 `sample_validity` 均未变化，Server 可保留相同 `state_revision`；但必须在约定新鲜度预算到期前，将**真正成功采样后**更新的 `observed_at` 通过完整 `state.snapshot` 送达 Client，确保合法的健康空闲连接不因上次业务变化很久以前发生而被误标 STALE。该帧不是 `state.changed`，不构成新的领域状态版本；仍需遵守全量帧大小、每连接/全局带宽和慢客户端的有界保护。注释心跳仅确认连接可能仍可写，不证明 Recorder 状态新鲜。

**STALE 证据来源：** 发布 `sample_validity: STALE` 不是生成一份新的 Recorder 状态。只有协调器**仍持有此前经过校验的、完整且身份匹配的不可变 `PublishedState`**时，才可以在其规范性领域字段保持不变的前提下，把有效性改为 STALE 并递增 revision；不得伪造 Task/Topic/Queue/Writer 字段或新 `observed_at`。如果从未取得可信的完整基线、原有基线已损坏/身份不一致或无法安全组装并发布，则不能凭空构造 STALE 快照，必须拒绝初始 SSE 或尝试发送 `stream.resync_required` 并关闭现有连接。**Recorder 当前不可用并不自动抹除此前持有的可信基线**，但没有可信基线就不能发布 STALE 的完整状态。

**采样失效与订阅新鲜度：** 当 Server 无法取得有效的新 Recorder 样本，且上次 `PublishedState` 的 `observed_at` 已超过配置的新鲜度预算时，状态协调器必须将 `sample_validity` 从 `CURRENT` 改为 `STALE`、发布**新的 `state_revision`** 并向已建立连接发送包含完整旧领域状态及 STALE 标志的 `state.changed`；`observed_at` 保持最后一次真实采样时刻，不能伪造新的采样时间。若无法可靠生成/发布完整 STALE 快照，则应尝试发送 `stream.resync_required` 并关闭 SSE；若 Socket 阻塞无法投递最后一帧，也必须关闭连接，不能让“心跳仍在”成为数据新鲜的证明。恢复有效采样后发布 `CURRENT` 的新 revision 与全量状态。Client 亦须依据 `observed_at` 与本地新鲜度预算设置失效检测：超过预算即使还收到注释心跳，也应把本地视图标为 STALE，并主动只读重新同步；连接重建不得自动重试控制 POST。首次建立 SSE 若根本没有可证明有效的完整基线，不得先发送标记 CURRENT 的历史快照。

Server 必须限制 SSE **并发连接数、每连接待发送事件数量、待发送字节数、单事件大小、建立期间缓冲及整体资源预算**。Drogon 的 `ResponseStream::send()` 返回成功不意味着客户端已经消费数据，也不证明框架/Socket 缓冲有界；完整生产准入必须通过慢客户端和断网测试验证，超出可控预算时关闭或拒绝连接，不允许向 Recorder 数据面传递背压。

因为 `state.changed` 是完整快照，Server 可以在未写出队列中用最新完整状态**覆盖尚未发送的旧完整状态**，并显式保留新 revision、记录合并计数；不得把已经部分写入 Socket 的帧与另一个 JSON 拼接成损坏 SSE 帧。若下游不可观察的框架内部缓冲继续增长，Server 不得仅靠应用队列合并声称安全，应断开慢连接并释放所有订阅资源。

推荐可观察的失败行为：

| 情况 | Server 协议动作 | Client 必须做的事 |
|---|---|---|
| SSE 连接上限 | 握手前 HTTP 429/503，含 ErrorCode | 按有界重试间隔重连，不影响 Recorder |
| 已建立连接遇到待发送字节/事件上限 | 优先以最新完整 State 合并待发送项；不能保证时发 resync_required 并关闭，或直接关闭 | 标记 STALE，重连取新 snapshot |
| 首次订阅期间缓冲上限 | 不发送“缺字段初始快照”；重新采样/合并全量或终止 | 重新建立基线 |
| 网络中断/半开连接 | 清理订阅；不能要求 Client 退出触发 Stop | 重连，旧展示先标记 STALE |
| Runtime ID 改变 | 新连接/查询返回新 Runtime 身份；旧连接可能断开 | 丢弃旧 revision 可比性，重建 |
| 健康采样但业务状态长期未变化 | 到新鲜度预算之前发送同 revision 的全量 `state.snapshot`，其 observed_at 必须确实来自新采样；不得虚增业务 revision | 验证同 revision 业务字段一致并刷新观察时刻，不因单纯空闲触发错误 STALE |
| PublishedState 采样过期或采样失败 | 发布新 revision 的全量 `state.changed`（`sample_validity: STALE`）；无法安全发布时 resync_required/断连，心跳不表示 CURRENT | 即使仅收到心跳，也依据 observed_at 检测过期并标记 STALE；恢复后重新同步，不重试控制 POST |
| 同 Runtime 丢中间 revision | 下一条**有效完整状态**可恢复当前值；中间事件历史不可恢复 | 替换全状态，记录 gap 诊断 |
| JSON/事件格式损坏 | 标记协议错误，必要时断开 | 不把部分数据合并进旧状态；重新同步 |

`stream.resync_required` 如能发出，示例：

```text
id: 93ac8a61-69f4-49c8-b3fa-400c4f396499/19
event: stream.resync_required
data: {"api_version":"1.0","reason":"SUBSCRIBER_OVERFLOW","runtime_instance_id":"9815222c-f39d-4d45-8d1a-bc7f68e75af6"}

```

**不能保证在 Socket 完全阻塞时一定能发出最后一帧**；突然断连本身就是 Client 必须重新同步的触发条件。禁止默默丢弃完整快照后继续把过时的 Client 展示标记为 CURRENT。

重连机制：Client 使用有界、退避及随机扰动策略恢复 GET /events；每条新流**无条件首先取得完整 state.snapshot**。Server 可以忽略 `Last-Event-ID` 的旧值做补发，但必须明确返回新流的基线，而不是把它当成“成功恢复无损中间事件”。HTTP Status 可在 SSE 不可用时作为**有界降级轮询**，但不要同时使用两个无版本比较的来源相互覆盖。

**技术准入约束：** Drogon 1.9.14 异步 Streaming 的慢客户端资源、断线回收、取消/关闭顺序及与 Recorders 隔离需要专项实验；httpx-sse 0.4.3 的注释心跳、空 data、CR/LF/CRLF、Unicode U+2028/U+2029、异常关闭、解析器事件 ID 行为需要单独验证。若无法满足，应按技术选型文档重新评估局部适配/第三方实现，而不是放松本规范的重同步语义。

# 6. 安全、兼容性与验证基线

## 6.1 安全权限和输入边界

**协议定义权限语义，Server 负责鉴权实现。** V1.0 至少需要两个逻辑动作权限：

| 权限 | 覆盖端点 | 限制 |
|---|---|---|
| `read` | Runtime、Status、可用 Topic、历史列表/详情、Topic 证据查询、SSE | 只读并且有响应数据授权与脱敏边界 |
| `control` | 所有 `/api/v1/control/*` POST | 可改变当前 Recorder 任务与 Topic，必须先通过明确认证授权 |

Server 默认监听受限 loopback；**所有 Runtime API、查询和 SSE 都必须携带并验证且仅接受一个 `Authorization: Bearer <opaque-token>`，不因 loopback 自动匿名放行。** Token 对应 `read` 或 `control` scope，`control` 隐含 `read`；缺失、重复、格式错误或无效 Token 返回 401，并可使用标准 `WWW-Authenticate: Bearer` 挑战；已认证但 scope 不足返回 403。Token 不得出现在 URL、Query、Cookie、JSON Body、SSE data 或普通日志。远程可达场景还必须使用 TLS；TLS 可由 Drogon 直接终止或由显式配置的受信反向代理终止，但 Runtime 仍自行验证 Bearer Token。协议不规定 Token 内部格式或签名算法，因为 Token 是高熵不透明预共享 Secret。

安全状态必须默认拒绝：远程请求的权限/身份无法确认时不能放行控制；对代理提供的 `X-Forwarded-*` 等 Header，只有配置受信任的反向代理才能采用；不能依据客户端伪造的 IP/Host/Origin 头绕过限制。SSE 与普通 GET 同受 read 授权，不能以“只有状态通知”理由绕过敏感信息过滤。

**Web 单独处理浏览器会话与 CSRF。** Drogon Recorder API 不自动继承 Web 的 Cookie Session；Web 后端应通过 Client SDK 使用受控凭证调用 Recorder，浏览器不能直接持有 Runtime 控制面密钥。CORS 默认不为任意 Origin 开放，尤其不得使用宽松跨域凭据策略代替鉴权。Web 对浏览器的 SSE 代理、Session、CSRF 和会话存续必须在 Web Application 实现前形成明确设计，不改变本 API 的权限模型。

**输入和输出约束：**
- JSON 必须检查最大字节数、嵌套深度、字符串字节长度、数组元素数、枚举与整数范围；重复 JSON key 应拒绝或采用经过审查的单一确定策略，不能让授权字段被覆盖。
- URI 路径中的 recording_id 必须在 URL 解码一次后验证为安全单层 ID；拒绝反斜杠、NUL、路径穿越、含斜杠的百分号解码结果、未知双重编码等绕过。Topic 名称不是文件路径，统一经 Recorder ROS 名称校验。
- 不允许请求字段指定 Runtime output_root、任意 MCAP 文件读取路径、恢复文件、Drogon 配置路径或通过自由字符串配置 Writer 插件。
- `operation_id`、`request_id` 不含敏感信息，不采用 Client 任意字符串注入日志；记录时限制长度并进行安全编码。
- 状态/SSE/日志不输出凭证、完整敏感 ROS Payload、进程环境变量；绝对目录路径默认脱敏或由受权限控制的历史查询返回。
- Query/History/Topic 证据扫描与网络写出必须有界；访问控制不能依赖 Recorder 高频数据路径锁。

## 6.2 预发布协议修改与能力报告

项目尚未发布，HTTP Path、ErrorCode、JSON/SSE Schema 与公共 SDK 可以直接进行**不兼容修改**，但每次修改必须同步更新 Protocol 示例、Server Mapper、Client DTO/Parser、应用行为及端到端测试，不能产生混搭的新旧实现。固定的 `/api/v1` 和 `api_version=1.0` 是当前目标草案的标识，不表示已经向外承诺长期稳定的版本兼容。

当前 Protocol 的身份域、UNKNOWN/effects、Topic Evidence、Status、SSE 完整快照与安全默认拒绝都是可审查设计选择，**不是旧设计兼容包袱**；若发现更简洁且同样可验证的正确方案，应直接替换相应章节。任何改动不能在未经同步修订要求/验收的情况下暗中削弱数据完整性和安全性。

`GET /runtime` 的 capability 必须描述**当前运行程序真实可用的能力**，而不是开发计划。任何构建若不能提供可信 Topic Evidence，则必须报告 `topic_events_lookup=false` 并使相应查询返回明确不可用；正式 V1.0 若需求仍包含该能力，就必须提供并通过测试，不得把 false 当作跳过 MUST 的手段。

## 6.3 协议级验收与故障注入矩阵

本协议应产生**请求/响应/SSE 帧与端到端状态证据**，与 Recorder 独立 AC 相互对应，但不把设计文档本身写成“测试已完成”。必须覆盖以下确定性场景：

| 用例 | 注入或输入 | 通过判据 | 需求 / AC |
|---|---|---|---|
| HTTP 基本协议 | JSON 格式错、未知枚举、API Version/Method 不合法 | 4xx + 机器可读 ErrorCode，未调用 Recorder | FR-CTL-01、AC-11 |
| 任务守卫 | Runtime ID 改变、Stop 指向旧 generation、旧 Topic Epoch | STALE_RUNTIME/STALE_RECORDING/STALE_TOPIC_EPOCH，绝不误作用于新任务 | FR-CTL-02、AC-11/12 |
| 同时变更 | 并发 Start/Stop/Topic Add 与内部 Timed Stop | Server 外部调用顺序可判断，Recorder 内部变化可交错，无伪造成功 | FR-CTL-02/03、AC-11/12 |
| 控制队列过载 | 执行队列满、拒绝受理、排队未执行超时、超时与进入执行竞态 | 受理前 429/CONTROL_QUEUE_FULL；已排队且可证明未执行为 503/QUEUE_TIMEOUT/FAILURE/effects:NONE；已可能执行时为 UNKNOWN 而非伪报没执行 | NFR-PERF-04、AC-12 |
| 请求取消 | POST Stop 发出后主动断网、HTTP 客户端超时 | 客户端 Operation UNKNOWN，Recorder 不被网络取消自动 Stop/Resume | FR-CTL-03/07/08、AC-11 |
| 内部状态并发 | Stop 超时、Timed Stop 使状态 IDLE；另一次磁盘 Pause 使状态 PAUSED | 仅凭 IDLE 不判本次成功，仅凭 PAUSED 不判本次失败 | FR-CTL-03、AC-11 |
| Topic Evidence | ADD_PREPARED、终态 fsync 失败、持久化成功、Event 冲突 | 仅可关联合法终态 CONFIRMED；其他 UNCERTAIN/NOT_FOUND，不触发自动 POST 重试 | FR-CTL-03、AC-11 |
| Status 数据 | 空状态、重添加 Topic、跨任务世代、资源指标尚未采样、持续时长/计数增长、只有 observed_at 更新、最近移除列表溢出 | null/0 区分、Epoch 正确；按有界周期观测的业务计数/时长变化可触发新 revision，只有采样时刻更新不触发；最近移除列表有界，`topics_removed_total` 含已被列表淘汰的任务内成功移除项；GET/SSE 同 revision 业务内容一致 | FR-DAT-06/07、AC-10 |
| 计数与 Rolling | 多 Segment 重放、Writer/接入分别错误、Cleanup Queue 满 | written_live/replayed 独立，Cleanup 三项字段准确，AC-16 按原序号去重 | FR-DAT-06、AC-10/16 |
| Graph 发现 | 无发布者、未发现、多类型、类型支持缺失、动态改变 | observation/recordability 正确，CLI/TUI/Web 语义统一 | FR-REC-12、AC-02/22 |
| History 查询 | COMPLETE/INCOMPLETE/FAILED/UNKNOWN、缺失起止时间、并发 Cleanup | 状态不误报、null 不补零，只读分页受限、不暴露不允许路径 | FR-DAT-09、AC-23 |
| SSE 初始竞态 | 注册前后发生 Recorder 变化、取初始快照期间多次 Publish | 首帧有效完整基线，后续可达最新状态，无永久陈旧窗口 | FR-CTL-04/05、AC-13 |
| SSE 中断 | 慢客户端、队列满、首次订阅缓冲溢出、Runtime 重启 | 有界合并或重同步/断连，Client 状态 STALE 后恢复完整基线 | FR-CTL-04/05、AC-13 |
| SSE 正常空闲与采样失效 | Recorder 业务状态长期不变但采样正常；随后注入采样失败且注释心跳继续；再恢复采样 | 业务稳定时按预算发送同 revision 的全量 snapshot 刷新真实 observed_at，不误标 STALE；失效后须发新 revision + 全量 STALE 或 resync_required/断连；Client 不把心跳当新鲜样本；恢复以 CURRENT 新 revision 重建 | NFR-PERF-04、AC-13 |
| SSE 合规 | 仅注释心跳、合法空 data、CR/LF/CRLF、Unicode U+2028/U+2029 | 不制造伪业务事件，通用 Parser 仍正确识别标准空数据事件 | FR-CTL-04/06、AC-13/14 |
| 无敏感泄露 | 远程未授权、TLS 缺失、恶意 ID/路径、Web CSRF | 拒绝不安全操作；JSON/SSE/日志没有 Secret | NFR-SEC-01/02/03、AC-18 |
| Shutdown | Recorder 长 Stop，Server I/O 退出与 SSE 断开交错 | 不伪造已退出、SSE 资源清理、Recorder 对象引用受保护 | NFR-REL-04/05、AC-17 |

特别要求：SSE 验收应同时测试 **Drogon 原生 Stream 行为 + Client httpx-sse 解析行为 + 两者真实互操作**，不能以本地 mock SSE Frame 单元测试代替慢客户端生产准入。HTTP JSON 测试应在可注入 Recorder 假对象和真实 Recorder 集成环境各进行一次，不得只看请求路由是否返回 200。

## 6.4 Server / Client 设计交接与待量化事项

Server/Client 不应再各自解释相同字段。以下事项属于**已定语义、待量化或交付验证的参数**，不得通过参数选择改变规范性结果：

| 事项 | 归属 | 交付时必须说明 |
|---|---|---|
| 外部变更执行队列 | `server.md` | 串行化范围、容量、Admission/执行竞态、等待/关闭预算与线程不自等待 |
| Topic operation 证据 | `recorder.md` + `server.md` | **当前设计：**Recorder 提供 `lookup_topic_operation(recording_id, operation_id)` 或等价的只读领域能力（§1.3/§2.4），统一 Event/Recovery 语义及 fsync 失败窗口；Server 仅负责鉴权、有界查询调度与 Protocol 映射，不解析 JSONL。**该 API 的 C++ 实现及其故障注入验证是发布验收前必须关闭的实施项**，设计规范不代表它已实现 |
| Runtime Shutdown | `server.md` | 有限等待与 DRAIN_TIMEOUT、Writer/Executor/Object 生命周期、受控异常退出 |
| 状态协调器 | `server.md` | `PublishedState`、采样周期、Revision 递增规则、合并、GET/SSE 同一对象来源；**量化**样本新鲜度预算、状态采样/发布周期、同 revision 全量刷新时机、单连接/全局带宽、全量帧大小与排队/传输延迟；无可信完整旧样本时不允许凭空构造 STALE |
| SSE 实施预算 | `server.md` | 连接数/每连接字节与事件上限、慢发送检测、Streaming 隐性缓冲、清理 |
| Client SDK | `client.md` | HTTPX 同步/异步调用、状态新鲜度、UNKNOWN、Evidence Lookup、异常与取消语义 |
| SSE Parser | `client.md` | httpx-sse Issue #35、心跳/空 data、Last-Event-ID、重连与公共类型隔离 |
| Web 会话 | Web Application 设计 | Browser Session、CSRF、只经受权 Python Client 访问 Runtime；实现前必须闭合，可独立成文或并入统一 Application 设计 |
| OPEN-01 | 性能验证规范 | 参考硬件、CPU 归一化边界、最终正式发布阈值 |
| OPEN-02 | Server/Protocol 验证规范 | HTTP 延迟、SSE 状态可见延迟、容量、采样默认周期、过载测试 |
| V1.0 Security | `server.md` + Web Application 设计 | Bearer Token Secret/Scope、TLS/受信代理、Web Session/CSRF、凭据泄漏与远程攻击用例按 AC-18 验证 |

**不应推断为已经通过：** 使用 Drogon v1.9.14 和 httpx-sse 0.4.3 的技术选型，并不意味着其慢连接和 SSE 协议风险已经通过生产准入。Client Python 3.10/3.12 支持范围按技术选型执行；实际发布需记录 Python 的来源、系统安全更新和依赖兼容性，不以“CPython 上游生命周期”直接替代 Ubuntu LTS 安全支持判断。

**协作原则：** 以当前产品功能与可实现的安全边界为依据，Recorder、Protocol、Server、Client 的错误语义和字段设计可以双向调整；同步更新所有调用方与测试，避免部分升级。

## 6.5 Protocol 实施就绪检查表

协议可交给实现前，需要明确：

1. 13 条 HTTP 路由的 Method、必填字段、值域和资源权限完整，任何控制变更都具有唯一合法入口。
2. operation_id、request_id、recording_generation、runtime_instance_id、state_revision、SSE ID 的身份与生命周期不存在混用。
3. SUCCESS/FAILURE/UNKNOWN、Event 证据查询与 HTTP 错误具有**可执行的组合映射**：§4.2 的 HTTP 状态码 / 机器可读 ErrorCode / `effects:NONE|POSSIBLE|CONFIRMED_PARTIAL`，加上 §2.5 的 `evidence_status: CONFIRMED_SUCCESS|CONFIRMED_FAILURE|UNCERTAIN|NOT_FOUND`，必须能区分受理拒绝、部分副作用、无法证实请求终态和只读证据缺失；任何超时/状态碰巧一致的场景不会伪造某次请求成功。
4. Status 的写入、重放、Cleanup、Disk 与缺失值字段符合 Recorder；历史只读列表不伪造完成、不泄漏路径、不自行执行 Recovery。
5. SSE 首次完整基线、发布顺序、慢客户端有界策略、断线/代际变化的 Client 重同步在实际协议帧层无矛盾。
6. 未决 OPEN-01/02 的具体量化参数已明确由验证规范承接；OPEN-04 的 V1.0 安全设计已裁定为 Bearer Scope + 远程 TLS，但仍必须通过 AC-18 才能取得生产准入。本协议不把设计闭合当作验证通过。
7. Client 能在不修改领域结果语义的前提下实现同步、异步、SSE 重连与 Application 显示，控制 POST 与 SSE 重连策略独立且不自动重放有副作用 POST；过期状态/心跳不能伪装 CURRENT，且不因部署配置变化改变 MUST 要求。
8. Topic Evidence **已裁定路径 A**：Recorder 必须提供正式只读接口及 fsync 失败可信度边界，Server 不自行解释 Event 文件；该接口、故障注入及 HTTP 查询验收均属于正式 V1.0 的实施/发布阻断项。

**开发阶段维护规则：** 端点、错误和 SSE 合同如需改变，直接修改本文并同步更新 Server/Client 和验证用例；本文件不是已发布协议兼容基线，也不代表 HTTP/SSE 已实现。
