# ros2_bag Client 详细设计

| 属性 | 内容 |
|---|---|
| 文档版本 | v0.1.1 |
| 文档状态 | **DRAFT — V1.0 开发阶段，随时修订；规范尚未批准用于完整实施** |
| 设计范围 | Python Client SDK：同步/异步 HTTP API、类型模型、控制结果、SSE 消费与重同步、异常/资源关闭、测试 |
| 架构依据 | docs/architecture.md（开发稿，可直接联动修改） |
| 需求依据 | docs/requirements.md（开发稿，可直接联动修改） |
| 领域依据 | docs/recorder.md（开发稿，可直接联动修改） |
| 协议依据 | docs/protocol.md（开发稿，可直接联动修改） |
| Server 依据 | docs/server.md（开发稿，可直接联动修改） |
| 选型依据 | `docs/system_technology_selection.md`；HTTPX 0.28.1 + httpx-sse 0.4.3，Python 3.10 / 3.12 |
| 实现现状 | 当前仓库为 Recorder-only 基线，**尚无正式 Python Client SDK**；本文定义待实施目标 |
| 下游 | CLI/TUI/Web 设计文件尚未编写；在对应 Application 实现前补齐。另有部署与专项验证 |

# 1. 定位、边界与公共结构

## 1.1 Client 的唯一职责与设计原则

Client 位于 **Server 与 Application 之间**：只通过当前设计的 HTTP/JSON/SSE 访问 Recorder Runtime；不依赖 ROS 2、rclpy、C++ 动态库、MCAP、Manifest 文件或宿主机进程。CLI、TUI、Web 均使用同一个 Python SDK，不能在应用中各自实现请求字段、错误映射、重连或“请求结果 UNKNOWN”逻辑。

V1.0 只承诺：

1. **控制/查询对齐。** 规范的 13 个网络端点由 Client 公共 API 表达，参数值域、身份守卫、权限语义、版本和异常严格遵循 `protocol.md`，不新增其他 Runtime 端点。
2. **同步与异步完整等价。** 提供面向脚本/CLI 的同步 API 与面向 Textual/Starlette 等事件循环的异步 API，复用**同一领域数据模型和响应校验规则**，而非两套含义不同的实现。
3. **业务模型不泄漏传输库。** 公共参数、状态、错误与 SSE 通知使用 SDK 自有的 Python 值对象；HTTPX Response/Request 与 httpx-sse ServerSentEvent 仅留在 transport/parser adapter。
4. **结果与观察分离。** `OperationResult` 是特定控制请求的可证明结果，`CurrentStatus` 是 Recorder **此刻被观察到**的状态，两者不能通过时间或状态值“猜测合并”。Topic 证据查询另以具有关联性的结果更新**该 Topic 操作的证据判定**。
5. **副作用请求永不自动重放。** 无论 POST 返回 429/503、客户端 Timeout/Cancel、Runtime 重启、Server 回退还是调用方重新进入 SDK，都没有隐含 retry；证据未知不能由 Client 自行转成 SUCCESS/FAILURE。
6. **有界流消费与安全关闭。** SSE 重连与 GET 降级仅作用于只读数据；Client 不能将持续 SSE/状态刷新视为某个 Recorder Task 的生命周期托管。Client/UI 断开/关闭**绝不自动 Stop** 录制。
7. **Primitives, not Features。** 不实现持久 Operation Registry、本地恢复引擎、ROS Graph Discovery 副本、重放 EventStore、跨 Runtime 协调器、Client 内置 UI、数据库和可配置通用 workflow。

本文件按规范性的**语义契约**设计。示例 class/dataclass/函数签名是 V1.0 推荐公共形态：实现若调整 Python 名称、异常封装或模块目录，不得擅自改变 Protocol 的 HTTP 身份、状态版本、UNKNOWN、安全边界，也不得让 Application 被迫直接依赖 HTTPX/httpx-sse。

## 1.2 包布局、模块依赖与最小组件

建议建立可独立安装的纯 Python 包 `ros2_bag_client`，不与 ROS 2 Python Package 名称绑定：

```text
ros2_bag_client/
├── pyproject.toml
├── src/ros2_bag_client/
│   ├── __init__.py            # Stable public API exports
│   ├── config.py              # Endpoints, TLS/Auth, timeouts, bounds
│   ├── models.py              # Immutable request/result/status/history DTOs
│   ├── errors.py              # Local/transport/protocol exception family
│   ├── client.py              # RecorderClient (sync)
│   ├── async_client.py        # AsyncRecorderClient
│   ├── _http.py               # HTTPX transport and shared request mapper
│   ├── _codec.py              # Strict JSON Schema/Protocol validation
│   ├── _sse.py                # httpx-sse adapter, stream and lifecycle
│   └── _state.py              # Snapshot/version/freshness rules
└── tests/
    ├── unit/                 # Fake HTTP Transport, fake SSE frames
    ├── contract/             # Current Protocol fixtures/schema
    └── integration/          # Drogon + ROS 2 Runtime real tests
```

| 范围 | 唯一职责 | 不能做的事 |
|---|---|---|
| `models.py` | 领域兼容的请求/状态/结果/异常值对象 | 导入 HTTPX、httpx_sse、rclpy 或依赖一个活跃 Client |
| `_codec.py` | JSON 编码/校验、整数安全、ErrorCode/Outcome 转换 | 宽松转换未知状态为成功，补造缺失字段 |
| `_http.py` | 有界请求、HTTPX 生命周期、Header 与错误传输 | 具备副作用 POST 重试；重定向到未授权地址 |
| `client.py / async_client.py` | 统一方法入口与资源管理 | 每个方法重写一套协议语义；构建隐藏自动控制策略 |
| `_sse.py` | 解析器封装、SSE Session、断开重连/取消 | 把第三方 Event/Response 暴露到公共 API |
| `_state.py` | 全量状态版本比较、STALE、重同步 | 将状态快照误作为某个 operation_id 的终态证明 |
| Application | 命令行/UI、用户确认、显示/退出码 | 绕过 SDK 直接自动重试非幂等 POST |

HTTPX `Client` 与 `AsyncClient` 可以分别复用 Transport 逻辑但不能跨同步/异步事件循环共享实例；线程安全仅按所选 HTTPX 版本文档允许的范围使用。相同接口的 sync/async 不是让同步 HTTP 方法在 TUI/Starlette 事件循环里阻塞调用，也不能通过在每个调用中 `asyncio.run()` 包装长期 SSE 生成器模拟稳定异步支持。

包边界与依赖：Python ≥3.10；目标 Ubuntu 22.04/Humble/Python 3.10 与 Ubuntu 24.04/Jazzy/Python 3.12、AMD64/ARM64；默认最小依赖 `httpx==0.28.1`、`httpx-sse==0.4.3`，不引入 rclpy、PyYAML、Textual、Starlette、ROS 2 或数据库作为基础 SDK 依赖。初始严格锁版本用于生产准入复现，升级需要兼容测试；Client 源包的环境/发布策略由 Python 打包规范管理。

## 1.3 公共 API 矩阵与同步/异步契约

建议的对外方法矩阵（请求/结果均为 SDK 自有 Python 值对象）：

| 功能 | `RecorderClient`（同步） | `AsyncRecorderClient`（异步） | 返回语义 |
|---|---|---|---|
| Runtime | `get_runtime()` | `await get_runtime()` | RuntimeInfo |
| Recorder 状态 | `get_status()` | `await get_status()` | CurrentStatus |
| Topic Graph | `list_available_topics(query)` | 同名 await | AvailableTopicPage |
| History 列表 | `list_recordings(query)` | 同名 await | RecordingPage |
| History 详情 | `get_recording(recording_id)` | 同名 await | RecordingDetail |
| Topic Event Evidence | `get_topic_operation(recording_id, operation_id)` | 同名 await | TopicOperationEvidence |
| Start | `start(request_with_operation_id)` | 同名 await，要求预先持有 ID | OperationResult；推荐先 prepare 再 submit |
| Stop/Pause/Resume | `stop/pause/resume(request_with_operation_id)` | 同名 await，要求预先持有 ID | OperationResult；不得只传裸 Target 后在 await 内生成隐藏 ID |
| Add/Remove | `add_topic(request_with_operation_id) / remove_topic(request_with_operation_id)` | 同名 await，要求预先持有 ID | OperationResult；请求必须含稳定的 operation_id/Topic Epoch |
| 预构建控制尝试 | `prepare_*(...)` 返回 OperationAttempt | 同名同步 prepare 方法 | 本地生成 operation_id、无 I/O、Attempt 单次提交状态由 SDK 管理 |
| 单次发送 | `submit(attempt)` | `await submit(attempt)` | exactly one POST；调用方事先拥有 ID，Cancel/Timeout 保留 UNKNOWN |
| 状态流 | `watch_states()` | `watch_states()` async iterator | StreamUpdate（包含 StateView/流状态） |
| 关闭 | `close()` / Context Manager | `await aclose()` / Async Context Manager | 释放 SDK 网络资源；**不调用 Stop** |

同步/异步对同一响应的业务字段、ErrorCode、SubmitState、操作证据判定及状态版本检查必须共享规范，不允许 TUI async Client 把 UNKNOWN 当失败而 CLI sync Client 将其当成功。同步接口不能在已运行的 asyncio/Starlette/Textual 事件循环中执行阻塞 HTTP；异步 Watch 只能在所属 asyncio 事件循环中消费与关闭，不允许跨 loop 移交活跃异步 Client。

常用请求守卫可以由 `RuntimeInfo + CurrentStatus` 显式构建，但变更方法仍**要求明确**的 `expected_runtime_instance_id`，除 Start 外要求 `recording_id + recording_generation`；不能自动选“当前录制任务”，更不能在收到 STALE_RECORDING 时自动重新获取目标并修改旧请求。

## 1.4 公共值对象、身份与缺失值

建议 Python 3.10 兼容的最小核心类型；这是设计级 API 草案，不代表已经发布的 Python 模块：

```python
from __future__ import annotations
from dataclasses import dataclass
from enum import Enum
from typing import Optional

class Outcome(str, Enum):
    SUCCESS = "SUCCESS"
    FAILURE = "FAILURE"
    UNKNOWN = "UNKNOWN"

class SubmissionState(str, Enum):
    NOT_SUBMITTED = "NOT_SUBMITTED"     # Local certainty, NOT a Server Outcome.
    POSSIBLY_SENT = "POSSIBLY_SENT"     # Request might have entered Recorder.
    RECEIPT_RECEIVED = "RECEIPT_RECEIVED"

@dataclass(frozen=True)
class RecordingTarget:
    expected_runtime_instance_id: str
    recording_id: str
    recording_generation: str

@dataclass(frozen=True)
class ProtocolFailure:
    code: str
    message: str
    phase: Optional[str]
    effects: Optional[str]             # NONE/POSSIBLE/CONFIRMED_PARTIAL
    retryable: Optional[bool]

@dataclass(frozen=True)
class OperationResult:
    operation_id: str
    submission: SubmissionState
    outcome: Optional[Outcome]         # None ONLY for proven NOT_SUBMITTED.
    request_id: Optional[str]
    http_status: Optional[int]
    error: Optional[ProtocolFailure]
    data: object | None                # Real API: typed success payload.
```

**NOT_SUBMITTED 不属于 Server 的第四种 Outcome**。它是 Client 能够证明还未发起 HTTP 发送时的本地状态，`outcome=None`；一旦调用可能已提交但缺少可信终态就是 `POSSIBLY_SENT + UNKNOWN`。不能依据 `httpx.ConnectError` 或 `ConnectTimeout` 名称一律推断“请求绝未发送”：代理、自定义 Transport、连接复用和包装可能改变因果关系；只有 SDK 严格受控的发送前校验/本地准备路径才能提供这种证明。

`operation_id` 必须在尝试调用传输层之前确定并附到 JSON Body。每个新的逻辑操作生成新 UUID；UNKNOWN 恢复、SSE 重连和只读刷新绝不自动 POST。用户若在看见 UNKNOWN 后显式决定再次执行，SDK 创建**新的 OperationAttempt + 新 operation_id**，旧 Attempt 原样保留，Topic Add/Remove 仍可用旧 ID 查询旧 Evidence。SDK 拒绝调用者在同一个 Client 进程已知范围内把一个 OperationAttempt 的 ID 复用于不同操作；这只是本地防误用，不是 Server 幂等 Registry。`request_id` 是 Server 对每次 HTTP 尝试给出的独立 UUID；没收到时为 None。

其他公共 DTO 至少应定义：`ClientConfig`、`RuntimeInfo`、`RecorderSnapshot`、`CurrentStatus`、`TopicStatus`、`TopicDiscoveryItem/Page`、`HistoryEntry/Page/Detail`、`TopicOperationEvidence`、`StreamUpdate`、`StartRequest`、`TopicAddRequest`、`TopicRemoveRequest` 及领域错误类型。**不是要求为每一个 JSON 嵌套字段建一个独立服务类**：值对象可以有轻量嵌套模型，但必须有稳定的 public field semantics。

公共模型解析规则：

- `api_version` 受支持，字段有完整类型及值域；不支持的主版本、未知 `Outcome`、未知安全状态、缺失身份、无效 Time/UUID → ProtocolViolation，不能宽松推断 SUCCESS。
- `recording_id` 在请求侧使用与 Protocol 完全相同的 ASCII `^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$` 预校验；SDK 不做 Unicode/路径规范化来“修正”非法 ID，Server/Recorder 仍执行最终校验。
- `recording_generation`、`state_revision`、`topic_id`、`topic_epoch` 和长计数保留非负十进制字符串。Python `int(value)` 仅用于内部版本大小比较；不经浮点数往返，不接受 JSON 布尔值替代整数。**转换前必须验证十进制字符长度和可接受数值范围**，防止异常长整数串造成 CPU/内存消耗；上限以当前 Protocol 值域及 Server 最大响应/字段预算为依据，不能擅自截断高位重新解释身份。
- `written_live` 与 `replayed_to_new_segment` 独立，禁止自行生成包含重复 Replay 的假“接收总数”；`topics_removed_recent` 有界，`topics_removed_total` 是任务内累计成功移除数，不能由列表长度推导。
- 历史 `INCOMPLETE/FAILED/UNKNOWN`、Topic `NOT_FOUND/UNAVAILABLE`、资源 `valid:false` 都是具有实际含义的状态。未采集数据为 None，不能把 None 转为 0、false 或 `COMPLETE`。
- Timed `duration_seconds` 是不可变总期限；暂停仍计入。CLI/TUI/Web 倒计时应显示“剩余总期限”，并且不能从剩余 0 推断 Writer 已成功关闭。
- `effective_config.compression` 只读报告 Recorder 冻结的 Runtime StorageProfile（`NONE/LZ4`），不是请求回显；`timestamp_policy=RECEIVE`。NORMAL/TIMED 的 segment/retention 为 null，ROLLING 返回实际生效正值。
- 同主版本新增**安全无关的可选字段**可以忽略；对影响权限、状态/操作结果或身份的未知字段/枚举不得采用不安全默认值。对象大小/深度、字符串长度、Topic 数、History Page/segments 仍必须有界。

# 2. HTTP 连接配置、控制请求与异常边界

## 2.1 ClientConfig 与 Transport 的所有权

SDK 只使用 `httpx.Client` 和 `httpx.AsyncClient` 实现 HTTP；SSE 使用独立创建/关闭的 HTTPX Client/Transport 配置，通过 httpx-sse Adapter 消费。连接池、认证、TLS 证书、超时、代理与 Header 不应由 CLI/TUI/Web 各自拼装。配置建议：

| 选项（Public Config） | V1.0 初始值/限制 | 行为 |
|---|---|---|
| `base_url` | `http://127.0.0.1:8765` | 固定 Origin 与 /api/v1 路径；禁止 API 参数注入主机/任意路径 |
| `api_version` | `1.0` | 固定支持的协议主版本；连接时比对 Runtime 能力 |
| `connect_timeout_sec` | 3 s（候选） | 建立连接的预算；不能证明请求是否执行 |
| `read_timeout_sec` | 40 s（候选） | 普通 HTTP 读预算须覆盖 Server 排队 5 s + Native Response 30 s + 网络/调度余量；超时仍为 UNKNOWN |
| `write_timeout_sec` | 5 s（候选） | Body 发送上限；若任何字节可能发出则 UNKNOWN |
| `pool_timeout_sec` | 3 s（候选） | 等待本机连接池槽，不自动重试 POST |
| `sse_connect_timeout_sec` | 3 s（候选） | SSE 新 GET 的连接预算 |
| `sse_read_timeout_sec` | None（另设 Watchdog） | 避免 HTTPX 默认 5 s 超时误断长期 SSE |
| `verify_tls` | true | 远程 TLS 验证不可被普通选项默认绕过 |
| `follow_redirects` | false | 禁止 Control POST 经 HTTP Redirect 再发送或凭据外泄 |
| `trust_env` | false（安全建议） | 默认不继承未经审核的代理或 TLS 环境配置 |
| `http_limits` | 有界连接/Keepalive | 防止 Application 创建无界 Client/Socket/流 |
| `max_json_bytes` | 1 MiB + 受限信封余量（候选） | 接收真实字节数硬限制，不能信任 Content-Length |
| `max_sse_frame_bytes` | 1 MiB 完整帧（候选） | 不积累无界 SSE 输入；协议/Parser 前限制须实测 |
| `max_history_items` | 每页 ≤200，累计显式分页 | 不在 SDK 内自动无限遍历任务列表 |

数值都是**初始候选**，不是需求 OPEN-02 的已测 SLA。对 Control POST，Client 读取响应的预算必须结合 Server `max_queue_wait_ms`、`native_response_timeout_ms` 和网络/调度余量做联动校验；**不能只与单独的 Native Deadline 比较**。即使配置失配或网络超时，缺失可信响应的本次操作仍为 UNKNOWN，不能推断领域执行失败。

**隔离控制 POST 与 SSE GET 重连**：实现时控制 HTTP 与 SSE 使用不同的 HTTPX Client/Transport 实例，隔离超时、中间件/重试、连接池资源和长连接。同步与异步 Client 各自持有独立 Client 实例及生命周期；只读短 GET 可以与控制共用基础连接配置，但需确保 SSE 长连接不会饿死控制/查询流量。最小方案是“短 HTTP Client + SSE Client”，复杂部署可加独立只读池，不引入多套协议逻辑。

HTTPX 0.28.1 的 `AsyncClient` **不存在 `retries` 构造参数**。若显式配置 Transport，使用 `httpx.AsyncHTTPTransport(retries=0)` / `httpx.HTTPTransport(retries=0)` 并审查自定义 Transport、Proxy、Middleware 均无 POST Replay；默认 HTTPX Client 也不应额外装自动重试插件。`follow_redirects=False` 需要通过 Redirect + Authorization 注入测试，不能“自动跟踪重定向找到 Runtime”。

TLS/Auth 使用受控 `CredentialProvider` 或固定凭证引用，支持 read/control 权限差异，但不在协议层自行决定 Token 算法。默认日志、异常 repr 和 SDK DTO 不能包含 Authorization、私钥或完整敏感 Proxy URL；不因 `localhost` 默认认定浏览器来源可信。OPEN-04 的远程认证/TLS 方案待正式发布前验证。

## 2.2 控制请求序列化、目标守卫与时序

所有状态变更只调用 Protocol 的固定 POST 路由。同步/异步方法共用同一编码器，出网前确认：

| 命令 | 必填请求字段 | 特别约束 |
|---|---|---|
| Start | operation_id、expected_runtime_instance_id、recording_id、mode | NORMAL/TIMED/ROLLING；TIMED 正数 duration；ROLLING 的分段/保留需正数 |
| Stop/Pause/Resume | operation_id、expected_runtime_instance_id、recording_id、recording_generation | 不能偷偷改用 Status 中更新后的另一个 Task |
| Topic Add | Target + name、可选 type、qos、repeat_policy | Graph 多类型须显式 type；EXPLICIT QoS 必须完整 |
| Topic Remove | Target + topic_id、topic_epoch | 只按 Epoch 删除，禁止 name-only |

Start Request 只包含 recording_id、mode 及对应 duration/rolling 时长；V1.0 SDK **不暴露 compression、timestamp_policy、QoS 默认或 storage plugin 覆盖字段**。物理 output_root、MCAP 配置文件和插件路径同样不能从 Client 注入。实际 Compression/TimestampPolicy 只从 Start 成功响应的 `effective_config` 读取。operation_id 在尝试传输前确定并进入 Body；每个新的逻辑操作必须是新 UUID。SDK 不提供“用同一 ID 重传”作为正常 API；UNKNOWN 后显式重试创建新的 OperationAttempt，并保留旧 ID 只用于诊断/Topic Evidence。

```text
PREPARE (validate model, acquire operation_id; no network effects)
    → invalid/closed before sending: NOT_SUBMITTED
    → SUBMITTING (HTTP request may have been sent)
       ├── Valid 200/SUCCESS receipt → OperationResult.SUCCESS
       ├── Valid explicit FAILURE receipt → OperationResult.FAILURE + effects
       ├── Valid 504/UNKNOWN receipt → OperationResult.UNKNOWN
       └── No trusted receipt (timeout, reset, cancel, corrupt reply)
           → OperationResult.UNKNOWN / retained possibly-sent attempt
    → Optional GET Status / Topic Evidence (read-only; never POST retry)
```

**async CancelledError 的传播不等于确认未执行**。async API 应保留取消的协作语义，不吞掉取消后自动补发；取消可能发生在 JSON 已送出、Recorder 仍执行时。推荐在发送前暴露/保存 OperationAttempt 的 operation_id 与状态；取消期间只关闭/释放 Client 侧请求句柄，保存 `POSSIBLY_SENT/UNKNOWN` 供调用者/应用显示，不能让用户误解“Cancel = Recorder 已中断”。该 Attempt 仅为内存中的单次调用记录，不是持久 Operation Registry。应在实际 async Task 生命周期与 Cancel 点做测试。

### 2.2.1 OperationAttempt 单次提交的公共 API（明确选择）

**在异步任务取消时，调用方必须能取得已提交请求的 operation_id**。所以 V1.0 的可取消控制 API 采用明确的两步形式，而非只在 `await client.stop(target)` 内生成一个调用方无法访问的 UUID：

```python
attempt = client.prepare_stop(target)  # synchronous, no I/O, creates/validates operation_id
result = client.submit(attempt)       # synchronous, exactly one POST
# async:
attempt = async_client.prepare_stop(target)  # still synchronous, no await
result = await async_client.submit(attempt)
```

`prepare_start/stop/pause/resume/add_topic/remove_topic` 统一生成**调用方持有的** `OperationAttempt`，其中包含不可变 Request DTO、operation_id、expected runtime/task/topic target 与本地 `submission_state`。可用一个无网络副作用的通用 `prepare(command, options)` 实现这些薄包装，不能让每个命令建立不同的 HTTP 语义。

`submit(attempt)` **只能成功开始一次**：使用 Attempt 自身的短临界区/CAS 把 PREPARED→POSSIBLY_SENT；若同一个 Attempt 被两个协程/线程重用，第二次立即返回/抛出 `ALREADY_SUBMITTED` 本地错误，不向网络发送第二个 POST。`submit` 在传输入口之前发生并记录这一转换；该转换不是“请求确实送达”，仅表示此后任何 Cancel/Timeout 若无可信回复都按 UNKNOWN 保守处理。明确发生在传输入口之前的验证/本地 Closed 错误可以保留 PREPARED/NOT_SUBMITTED，不允许把网络发送后的 ConnectionError 反向降级为未提交。

Sync/Async 便捷方法可以保留，但必须接收**调用方事先提供的 operation_id 或已创建的 Request DTO**；不允许 Async helper 在首个 await 内静默生成不对调用方可见的 ID。Sync 抛出的传输异常若可能已经提交，必须携带同一 OperationAttempt 与 UNKNOWN 判定，而不能只抛出裸 `httpx.ReadTimeout` 让调用方失去操作身份。Attempt 不持有长期网络连接、Recorder/Server 对象引用或持久化注册表；由 Application 按实际 UI 生命周期有界保存。

**资源与身份验证用例：** 在进入 Transport 前/发送首字节后/收到 Header 前/接收半个 JSON 时取消 async Task；在两个 await 并发 submit 同一 Attempt 时仅产生一次 POST；在 Client close/Watchdog 交错下 OperationAttempt 保留原始 ID/Target/UNKNOWN 信息且不导致自动 Stop。

## 2.3 HTTP 结果、错误信封与分层异常

| 情形 | Public Result / Error | 可作结论 |
|---|---|---|
| POST 前本地校验失败 / SDK Closed | LocalValidationError/ClientClosedError + 本地 NOT_SUBMITTED | 绝未发出 POST |
| 可信 HTTP 200 + SUCCESS、operation_id/身份正确 | OperationResult(SUCCESS, RECEIPT_RECEIVED) | 本次领域完成已证明 |
| 可信 4xx/5xx + FAILURE、ErrorCode/effects 合法 | OperationResult(FAILURE, RECEIPT_RECEIVED) | 失败回执已收到，副作用须按 effects 判定 |
| 可信 504 + UNKNOWN | OperationResult(UNKNOWN, RECEIPT_RECEIVED) | 该请求终态未知 |
| POST 可能送出后 ReadError/WriteError/Timeout/断连 | OperationResult(UNKNOWN, POSSIBLY_SENT) | 无可信回执，不能判执行失败 |
| 已可能送出后收到 JSON/身份/版本矛盾 | OperationResult(UNKNOWN, POSSIBLY_SENT) + 受限 ProtocolViolation 诊断 | **禁止**把协议错误当成领域失败 |
| 严格证明传输开始之前失败 | NOT_SUBMITTED + 本地错误 | 用户可以决定首次发起 |
| GET/SSE 明确 4xx/5xx | QueryError/AuthError/ServiceUnavailable | 只读查询/连接失败，不改变旧 OperationResult |

必须解析 Protocol §4.2 的 HTTP status + ErrorCode + Outcome + effects + retryable 组合。`503/QUEUE_TIMEOUT/FAILURE/effects:NONE/retryable:false` 是入队后确定未进入 Recorder；`429/CONTROL_QUEUE_FULL` 是 Admission 前拒绝；`504/OPERATION_RESULT_UNKNOWN` 是无法确认，不能互换。UNKNOWN 默认不得返回 effects:NONE 作为已证明未执行。

可信响应要求 HTTP status 与 Outcome 组合有效、JSON 字段完整、api_version 受支持、operation_id 与请求一致、Server request_id 有效、已返回目标身份与请求守卫一致。任何不一致不允许宽容地当成功；POST 已可能发送时保持 UNKNOWN，而不能把普通 ValueError 包装成“可安全重试的失败”。

异常族建议：`ClientError` 下区分 `LocalValidationError`、`ClientClosedError`、`AuthenticationError`、`QueryError`、`ProtocolViolation`、`ConnectionUnavailable`、`StreamError`、`ResourceLimitError`。**正常受信任 POST FAILURE 优先使用 OperationResult**，避免不同应用因异常处理分叉产生不同结论；GET 错误则抛明确的只读异常。默认字符串日志去掉第三方 HTTPX Error 中可能携带的 Secret；可以在安全诊断中保留精简 cause category。

## 2.4 POST Retry 禁止与 GET 只读重试

**控制 POST 的默认重试次数永远是 0**：既不 HTTP redirect，也不 Retry-After、中间件自动重新提交、断线恢复或者用旧 operation_id 重新发第二次。即使收到 `retryable:true`，也必须由 Application/用户显式决定是否建立一个新的操作尝试；SDK 自身不执行自动 POST。对 `503/QUEUE_TIMEOUT` 的 `retryable:false` 更不允许自动“稍后再试”。

GET（Runtime、Status、Graph、History、Topic Evidence）按调用者选择允许有界 read-only Retry，建议初始默认关闭；开启时仅对可安全判断的 429/503/连接瞬断做最多两次、退避和 jitter，不可对 401/403、INVALID_ARGUMENT、ProtocolViolation 或路径校验错误无条件重试。`Retry-After` 可作为 GET 退避提示但须有上限；GET 的多次读取不是单个原子历史快照，动态 History/Graph 分页仍可能漂移。

**不允许隐含 read-after-write 改写结果**：调用方可以明确请求 Status 更新；SDK 可以提供 opt-in 的只读 helper，但不能因为后续 Recorder IDLE、Topic ACTIVE、Disk Pause 等状态就替原来的 Stop/Add 生成 SUCCESS/FAILURE。

## 2.5 连接、身份、能力与生命周期边界

`get_runtime()` 暴露 api_version、runtime_instance_id、readiness 与 capabilities。它只表示那次已读状态，不代替 Recorder 执行前的目标身份守卫；遇到 Runtime 重启、recording_generation 变化或 STALE_RECORDING，SDK 不自动改目标重发之前的控制请求。

`capabilities.topic_events_lookup=false` 表示当前部署暂不支持可信只读证据查询（例如开发阶段）；SDK 不能回退到本地读 topic_events.jsonl 或“Topic ACTIVE 就表示成功”。正式 V1.0 发布该能力是 MUST 阻断项。只读查询失败只改变观察有效性或证据可用性，不改变操作结果。

Close、Context Manager、Stream Cancel、Client 应用退出均关闭自己的 HTTPX Response/连接池/迭代器；**不调用 Server Stop/Resume**。CLI/TUI/Web 不可通过 Python 对象的垃圾回收或 `__del__` 持有关键网络关闭语义。SSE 有专用网络资源，不使用控制的连接池重试参数。

# 3. 状态、发现、历史查询与 Topic 操作证据

## 3.1 Runtime、CurrentStatus 与来源/有效性

`get_runtime()` 只获取 Runtime 身份、readiness 与 V1.0 capabilities，不自动发起录制任务。`readiness` 表示服务可用性，不能取代 Recorder 当前任务状态，也不证明磁盘允许 Start。Client 必须保留 `runtime_instance_id`；新 Runtime ID 意味旧状态比较域失效，但不能让 SDK 自动改写旧 POST 的目标身份再重发。

`get_status()` 返回 `runtime_instance_id`、`state_revision`、`observed_at`、`sample_validity`，以及 Recorder/Topic/Queue/Pool/Cleanup/Disk/Process 的完整值对象。需要按下表保持权威含义：

| 字段族 | 必须保留 | 禁止行为 |
|---|---|---|
| Runtime 版本 | UUID + 十进制 Revision | 用 recording_generation/SSE Event ID 代替版本 |
| Recorder | ID/Generation、State/Mode、暂停原因、Timed 期限、错误 | 用 IDLE 推断上一次 Stop SUCCESS |
| Topic | ID/Epoch、QoS、写入/接纳/丢弃、Publisher 观察及 nullable `actual_hz/frequency_validity/gap` | 把同名不同 Epoch 统计合并，或把 gap=null 当成 false |
| Writer | `written_live` 与 `replayed_to_new_segment` | 把重放视为新接收消息 |
| Removed Topics | `topics_removed_recent`、`topics_removed_total` | 用最近列表长度充当全任务总数 |
| Cleanup | `cleanup_queue_depth`、`cleanup_pending_total`、`cleanup_queue_full_count` | 将 Queue 满视为文件删除失败 |
| Disk/Process | valid、sampled_at、nullable values | 将不可用 CPU/RSS 值填零 |
| Errors | ErrorCode/phase/secondary errors | 混淆当前 Failed 与历史 INCOMPLETE |

Server 发布对象是单一版本，但 Recorder 的子系统样本未必来自同一纳秒；Client 必须保留各子系统的有效性与来源。只有 Publisher 观测有效时才能将 0 Publishers 作为事实；否则应显示 Unknown。

HTTP Status 与 SSE 的完整 StateEnvelope 属于同一种 Protocol State，不应被不同 Application 分别映射为互相覆盖的“独立权威来源”。Client 的 StateView 必须统一校验 Runtime + Revision 与新鲜度，且任何 Status Snapshot 均不证明一个 UNKNOWN 控制请求的终态。

## 3.2 Graph 发现与分页一致性

`list_available_topics(query)` 映射 `GET /api/v1/topics/available`，支持 limit 1–200（默认 100）、非负 offset、`name_prefix` 或 `name`（互斥）。返回 items、observed_at、has_more、`snapshot_consistency:BEST_EFFORT`。

每个 Topic 观察包括 name、types、publisher_count、observation（VISIBLE/VISIBLE_NO_PUBLISHER/NOT_FOUND/UNAVAILABLE）、recordability（CANDIDATE/TYPE_CONFLICT/TYPE_SUPPORT_UNAVAILABLE/QOS_INCOMPATIBLE）、actual_hz 与 frequency_validity。SDK 不基于 PublisherCount 或空 items 自行证明“永远不存在”或“必然可录”；Graph 在两次分页间可以变化，出现重复/漏项，应允许显式重新扫描。

精确 name 查询中的 NOT_FOUND 是**本次观测**，不是永久不存在的事实。多类型需要由用户/Recorder 明确类型；QoS AUTO/EXPLICIT 的决策在 Recorder，Client 不调用 rclpy 获取另一套 Graph 信息。

## 3.3 History 列表、详情与只读边界

`list_recordings(query)` 允许 limit/offset/name_prefix/started_after/started_before，`get_recording(recording_id)` 返回含有界 Segment 的详细只读元信息。必须保持：

- 历史 `RECORDING/PAUSED/STOPPING/COMPLETE/INCOMPLETE/FAILED/UNKNOWN` 与当前 Recorder.state **分开**，不可因为 Runtime READY 就将旧任务当 COMPLETE。
- start_time/end_time、message_counts、segment_count、last_error 缺失时为 None/Unknown，绝不补造纪元时间或零。
- Segment 状态 `WRITING/FINALIZING/COMPLETE/DELETING/REMOVED/INCOMPLETE`；删除掉的 Rolling Segment 仍可能有受控历史条目。
- `path:null` 不允许 Client 在本地拼 output_root 或探测宿主机绝对目录；读取受 Server 权限控制。
- `snapshot_consistency:BEST_EFFORT`，分页无跨请求全局 Snapshot 保证；跨页可以重复/漏项，需要一致列表时由 Application 从 offset=0 重新读取。指定时间过滤时 start_time 未知项被排除并由 diagnostics 计数；未指定时间过滤时未知时间项排在已知项之后。无限分页自动读取不属于 V1.0 默认行为。

Recorder 独自实现 Manifest/Recovery 解析与历史数据完整性判断；Client 仅消费只读 API，不扫描输出目录，也不运行 Recovery、Delete 或文件修复。

## 3.4 Topic Evidence：请求关联与有效结论

`get_topic_operation(recording_id, operation_id)` 仅为 Add/Remove 提供持久化事件证据，不支持 Start/Stop/Pause/Resume 的结果查询。返回 `operation_kind`、可选 Topic ID/Epoch、terminal_event、error 及四态 evidence_status：

| Evidence | 是否能确认**这一次 Topic 操作** | Client 行为 |
|---|---|---|
| `CONFIRMED_SUCCESS` | 可信领域 ADDED/REMOVED 终态在查询时确认为持久化事实 | 标记领域副作用已确认；保留原 HTTP FAILURE/UNKNOWN 回执，不声称 Topic 当前仍 ACTIVE |
| `CONFIRMED_FAILURE` | 可信领域失败终态 | 保留原 HTTP 回执；记录失败诊断，不能推断 effects:NONE |
| `UNCERTAIN` | 否，PREPARED/冲突/截断/不可信持久化 | 保持 UNKNOWN，绝不自动重发 |
| `NOT_FOUND` | 否，本次可信完整只读查询未见该 ID | 不证明未执行；保持 UNKNOWN，绝不自动重发 |

`HTTP 200 + evidence_status:NOT_FOUND` 是**成功完成了证据查询**，不是 HTTP 404、更不是本地 `NOT_SUBMITTED`。recording_id 不存在对应只读 404，无法读 Event 或扫描不完整应为 UNCERTAIN/`EVIDENCE_UNAVAILABLE`，不得默认为 NOT_FOUND。

推荐将原始 `OperationResult` 与额外 `TopicEvidenceAssessment` 分开持有：原始 HTTP UNKNOWN 不会被事后覆盖成“当时收到成功响应”；只有完全匹配同一 recording_id + operation_id 的 CONFIRMED_SUCCESS/FAILURE 才允许确认**领域持久化事实**，原始 HTTP SUCCESS/FAILURE/UNKNOWN 记录不得被覆盖。进程内终态 fsync 失败但暂时可读行仍为 UNCERTAIN；重启后恢复验证持久的领域终态可被证实，但这不会倒填原 HTTP 的同步结果。相同 Topic 名、ACTIVE 状态、连续 Event Seq 或时间窗口均不能代替身份证据。

```text
Original Add request:
  operation_id = 77f1...730a, HTTP reply UNKNOWN

Later GET Topic Evidence:
  recording_id = run_001, operation_id = 77f1...730a
  evidence_status = CONFIRMED_SUCCESS, topic_id/epoch = 7/2

Display:
  "The original HTTP reply was UNKNOWN; durable Recorder evidence
   subsequently confirmed this exact Topic Add."
  Do not infer the Topic remains ACTIVE at present.
```

Topic Evidence 的 Parser 与事件安全性判断属于 Recorder。C++ 实现未完成时 Server 只能返回 capability=false/明确不可用；Client 不回退为本机 JSONL 解析器。正式 V1.0 必须通过该 MUST 的集成验收。

## 3.5 OperationResult 与 CurrentStatus 的独立性

建议 Client StateView/OperationResult/TopicEvidenceAssessment 使用不同的不可变值对象。Application 只能按以下证据更新结论：

| 信息 | 可更新依据 | 不可更新依据 |
|---|---|---|
| 控制 OperationResult | 本次可信 HTTP 回执 | 随后任意 Status、SSE 状态值恰好一致 |
| Topic 操作证据判定 | 可信、精确关联的 durable Event 查询 | 相同 Topic 名、时间窗、State ACTIVE |
| 当前 Recorder 状态 | 合法完整 Snapshot + 版本/新鲜度比较 | 某次 Stop/Resume 的历史结果 |
| 当前状态新鲜度 | 实际采样与接收时刻、超时预算、连接状态 | 注释心跳、TCP Socket 在线 |
| 历史任务完成 | Recorder History/Recovery 权威只读证据 | Recorder 当前 READY 或文件目录仍存在 |

例：“Stop 请求结果 UNKNOWN；当前 Recorder 状态 IDLE（state_revision 87、CURRENT）”。它不能推导“该 Stop 成功”。TUI/Web 应保留未知结果提示，CLI 应能区分相应退出码；具体文案/退出码归 Application 文档。

Client 不替 UI 决定用户再次操作的确认交互，但须明确暴露 UNKNOWN、INCOMPLETE、Disk Pause、Cleanup Queue Full、History/Topic Evidence 不可用等状态，保证三类 Application 的协议含义一致。

# 4. SSE 解析、版本、新鲜度与重新同步

## 4.1 SSEAdapter、Parser 与业务事件边界

SSE 消费采用 `httpx-sse 0.4.3` 的 `connect_sse(client, "GET", url)` / `aconnect_sse(client, "GET", url)`，通过 `EventSource.iter_sse()` / `aiter_sse()` 获取解析后事件。SDK 的 SSEAdapter 再解释 Protocol §5 的业务事件，不能向 Application 泄漏第三方 ServerSentEvent、EventSource、SSEError 或 HTTPX Response 类型。

```text
Dedicated HTTPX SSE Client
  → GET /api/v1/events (read auth, status + Content-Type validated)
  → httpx-sse streaming parser
  → Adapter: business event name + bounded complete JSON envelope
  → StateTracker: runtime/revision/sample_validity/freshness
  → immutable StreamUpdate / CurrentStatus for CLI/TUI/Web
          ↓ on disconnect, stale, invalid frame or resync_required
       STALE → bounded GET reconnect → fresh full state.snapshot
```

建立 SSE 连接前先检查 HTTP status，401/403/429/503 等属于握手前 JSON 错误，不能将错误 Body 当 SSE。HTTP 200 也必须核验 Content-Type 是 `text/event-stream`；握手失败映射成 SDK 受限 StreamError/AuthError。公共 API 不依赖 Parser 的 last-event-id 做服务器事件重放。

**协议业务事件仅限** `state.snapshot`、`state.changed`、`stream.resync_required`；只有注释的心跳不是业务事件。SSE `id: stream_uuid/sequence` 作为不透明传输身份记录日志，不解析 sequence 数字推断 Recorder 或 Topic 的操作顺序。

## 4.2 全量 StateEnvelope 和版本比较

一条新 SSE 流首次可接受的**业务状态**必须是完整 `state.snapshot`；不能先接一个 state.changed 增量后用旧缓存补齐。所有状态事件都包含完整的 `api_version:"1.0"` 和 `state`，后者必须具备 Runtime UUID、十进制 Revision、observed_at、sample_validity、Recorder/Topic/资源的规范性字段。JSON 缺失/类型错/Identity 冲突时必须拒绝整个状态，不合并残缺内容。

| 到达状态 | Client 的更新规则 |
|---|---|
| 新 SSE 连接首条合法 snapshot | 完整校验后建立 baseline，才可以进入 SYNCED |
| 同 Runtime，revision 增大 | 完整替换旧领域状态；允许中间版本缺口但记录诊断 |
| 同 Runtime，revision 相同 | **所有规范性业务字段相同**；仅 observed_at 等真实新采样元信息可更新 |
| 同 Runtime，revision 更小 | 不覆盖新值，记录乱序/协议诊断；必要时重新同步 |
| Runtime ID 不同 | 清除旧版本可比较性，旧 View 立即 STALE，重新建立全量基线 |
| 缺必填字段/未知安全枚举/截断数据 | ProtocolViolation，不按 Patch 方式恢复 |
| 只有 SSE ID 跳跃 | 记连接诊断，不推断丢了 Recorder Topic Event 或操作终态 |

**相同 Revision 的新鲜度刷新**属于完整 `state.snapshot`：状态业务数据严格相同而 observed_at 更新，代表 Server 真正成功重新采样；不会产生新的录制状态变化提示。若同 Revision 发生 written_live、elapsed_wall_seconds、paused_reasons、Topic 集、Disk、sample_validity 等字段变化，则属于 ProtocolViolation，不能仍显示 CURRENT。

若新状态具有新 Revision 且进入 STALE（或从 STALE 恢复 CURRENT），按完整替换处理。Server 只在有界采样周期观察变化；Client 不要求每个 ROS 消息对应新 Revision，亦不把跳过中间 Revision 当作 Recorder 录制丢包的证明。

## 4.3 注释心跳、合法空 data 与 httpx-sse Issue #35

技术选型确认 httpx-sse 0.4.3 已包含 CR/LF/CRLF 解析修订，但存在 Issue #35 的专项风险：先接收带非空 event id 的业务帧，再接收到仅注释的心跳时，Parser 可能错误产生一个空数据事件。Server 选择只用 `: heartbeat` 作为保活，不发送 `data:` 空消息代替心跳；Client 仍必须验证实际 Parser 输出。

**最小业务适配策略**：第三方 Parser 负责 CR/LF/CRLF、chunk 边界及多行 data 合并；Client 对明确的事件名称和 JSON Envelope 做严格校验。Parser 虚假产生的默认 `message` 事件/空 data 不能变成 StateUpdate 或刷新 observed_at。不能仅因为事件 ID 有值就把一条空 data 解释为业务事件。

**不得在通用 SSE Parser 层删除全部空 data 事件**。SSE 标准允许合法的 `event: custom` + 空 `data:`；它可以正确被 Parser 表达，但因为本项目的业务事件均需要非空 JSON，不被接纳为 ros2_bag 的领域状态。测试必须分别证明 Parser 的标准解析行为和 Client 的业务过滤正确性。

专项测试至少覆盖：注释心跳在首次 ID 前/后、合法空 data、默认 event、重复/空 id、多行 data、CR/LF/CRLF、UTF-8、U+2028/U+2029、无 Content-Type、错误 JSON、流在半帧断开、取消/关闭生成器。若缺陷会污染真实业务事件 ID 或数据边界，不可仅通过忽略空消息宣称修复，应在 Adapter 中实施可验证兼容措施或按技术选型更换 Parser；公共 Client API 不能因此变化。

### 4.3.1 SSE 原始流大小和 CancelledError 所有权前置门禁

`httpx-sse` 对解析成功后的 Event.data 执行大小校验**不能证明内存有界**：如果上游发送很长且始终不换行的 HTTP Chunk，第三方 `iter_lines()/aiter_lines()` 可能先累计超长字节，再交给业务层。实现必须提供可受控的 Streaming Byte/Line/Frame limiter **位于 Parser 之前**，并在 Content-Length 伪造、无限连续 data、100MB 单行、跨 chunk UTF-8 下测量 peak RSS；若锁定版本无法安全接入，必须评审使用经验证的 Parser Adapter 替代，不能用“解析后检查 1 MiB”冒充端到端有界。

Async 控制请求必须在第一个可能发出字节的 await **之前**让 Application 取得 `operation_id` 与独立 `OperationAttempt` 引用（用户显式传入 Request DTO ID 或先创建 Attempt）；一旦可能已提交，Cancelling Task 传播 `CancelledError` 也不得擦除其 `POSSIBLY_SENT/UNKNOWN` 标志。裸 `await client.stop()` 被取消时，如果 caller 之前拿不到 ID，就无法关联 Topic Evidence；因此推荐的公共异步 API 约束调用方通过 Request DTO 显式提供 ID 或先调用同步 `prepare_*()` 获取 Attempt，不允许仅在协程内部临时生成用户不可取得的 ID。无需建立持久通用操作注册表。

## 4.4 StateTracker：新鲜度、时间来源与 STALE

Client 明确区分两种时钟：

- `observed_at` 是 Server 上次**实际成功**采样 Recorder 的 RFC 3339 UTC 时间，必须来自合法的完整 Status/SSE State；不是事件到达时间或注释心跳时间。
- `time.monotonic()` 是 Client 本地跟踪有效 Snapshot 到达、断连、Watchdog/Backoff 的时间；不会受本机系统 UTC 校时影响，也不依赖 ROS 仿真时间。

跨机器 UTC 可能有偏差，不能只用“本机 datetime.now - Server observed_at”作为唯一年龄证明。Client 应保存上次有效 full snapshot 的本地 monotonic 接收点、Server observed_at、Server `sample_validity` 与本地 View 的同步/新鲜度状态。**新鲜度 Watchdog 必须由独立于 SSE 数据迭代器的定时执行路径驱动**：异步实现可用一个受管的 Watchdog Task/Deadline 等待，并在长时间 `aiter_sse()` 没有任何合法 full snapshot 时主动关闭当前 EventSource/取消 SSE Reader；同步实现用专门的受控 Reader/Watcher 生命周期或具备 Deadline 的传输等待实现同等语义。不能使用 `sse_read_timeout=None` 后只在 `for event in stream` 的循环体里检查过期，否则完全静默连接永远无法触发 STALE/reconnect。Watcher 关闭必须同时终止 Watchdog、Reader 和重连等待，且不会自动发 POST。默认 Server 候选 2 s 全量健康刷新、5 s 最大采样年龄，Client 需给网络与调度抖动留下可配置余量；具体值是 OPEN-02 交接事项，不是协议发布指标。

| STALE 来源 | 应做什么 | 不能做什么 |
|---|---|---|
| Server 发布新 revision、`sample_validity: STALE` | 保留完整旧领域状态作为诊断，立刻将 View 标为 STALE | 将旧 Recorder 数据继续标记 CURRENT |
| Server 心跳正常但长期未收到合法 full snapshot | 根据 Client monotonic Watchdog 将**本地** View 标 STALE，安排只读重新同步 | 把心跳当成新 Recorder 采样 |
| SSE 断线、解析失败、Runtime ID 不一致 | 标记 STALE，废弃旧 revision 比较/按新流重建 | 继续根据旧值自动执行 POST |
| 同 revision 新 observed_at 且业务完全一致 | 刷新本地有效 Snapshot 时刻，View 可保持 SYNCED/CURRENT | 虚增 revision 或产生“状态改变”提醒 |
| Server 恢复 CURRENT 的合法新 revision | 完整替换、重新校验 View | 将过去的 UNKNOWN 控制标为成功 |

Server 显式 `sample_validity` 与 Client 本地 `view_freshness` 是不同字段。Client 本地超时可将 View 标 STALE，但不得伪造 Server 的新 Revision 或修改 Server 原样值；Status 与 OperationResult 也不能混为单一“当前进程状态”。

## 4.5 重连状态机、退避与只读降级

推荐 SSE 生命周期：

```text
DISCONNECTED
  → CONNECTING → WAITING_BASELINE
                 ├─ valid full state.snapshot → SYNCED
                 │     ├─ newer revision → SYNCED
                 │     ├─ same revision, newer true sampling → SYNCED
                 │     ├─ STALE sample → STALE
                 │     └─ resync_required / lost stream → STALE
                 └─ protocol/identity error → STALE or PROTOCOL_ERROR
  → RECONNECT_BACKOFF → CONNECTING  (bounded)
  → CLOSED (user close/cancel, never POST)
```

每条新连接要新 `stream_id`、首先完整 `state.snapshot`；不能以先前 `Last-Event-ID` 作为可靠事件补发游标。可以把 event id 留在诊断日志，但 Client 不能自造服务器 event id 数列。旧状态不应在重连中被当作当前安全操作依据，新 Runtime ID 必须废弃旧 revision 的可比较性。

初始候选：`initial_backoff_sec=0.5`、`max_backoff_sec=30`、倍数 2、随机抖动 ±20%；每个 Watcher 同时至多一个有效 SSE Request 与一个 Backoff 协程/同步等待；Close/Cancel 立即停止新连接尝试。401/403 进入 AUTH_REQUIRED/停止高速重试，主版本不支持/业务 JSON 失效进入 PROTOCOL_ERROR，429/503 参考上限约束的 Retry-After 做只读 Backoff；不自动发 POST。

SSE 不可用时 Application 可以显式开启有界 GET /status 降级轮询；GET 与 SSE 必须统一 StateTracker 按 Runtime ID + revision 比较，不得因慢 GET 到达顺序覆盖较新 SSE。恢复 SSE 后可以停止轮询并保留状态完整性。若仍无可信数据，展示 DISCONNECTED/STALE，而非将录制状态默认设成 IDLE。

## 4.6 Watcher 交付队列、取消与资源所有权

`watch_states()` 对外只交付 SDK 自有的 `StreamUpdate`：至少区分 CONNECTING、STATE、STALE、RECONNECTING、ERROR、CLOSED 等生命周期通知；STATE 是完整 CurrentStatus。Lifecycle 消息不能含伪造 Recorder state，SSE Stream ID 不等于 Topic operation_id。同步 Watcher 使用 Context Manager/显式 close；异步 Watcher 使用 `async with` / `async for` 与受控 aclose/取消路径，必须释放 HTTPX streaming response 和重连任务。将 Generator 丢弃交给 GC 不是符合 V1.0 的关闭策略。

Watcher 对 Application 的通知 Queue 必须有界。因状态事件包含全量快照，可用**最新完整快照**替换尚未被应用消费的旧 Snapshot，并记录 revision gap/合并计数；绝不能将 STALE/Runtime 更换/错误通知静默合并掉而留下“看似 CURRENT”展示。队列容量、最大待交付字节、Application 消费超时与断开条件由 SDK Config/验收确定；网络输入无限持续也不能导致 Python heap 无界增长。

一个 Watcher 默认对应一条 SSE 连接，CLI/TUI/Web 应自行共享同一个 Watcher/StateTracker 给多个 UI 组件；每个 Widget 都另开 SSE 不是 SDK 默认。SDK 关闭 Session 不影响 Recorder 原生录制，断开 SSE 不发 Stop，也不触发 Topic Remove。

# 5. 生命周期、安全、配置与 Application 交接

## 5.1 同步/异步所有权、Context Manager 与关闭屏障

每个 `RecorderClient` 与 `AsyncRecorderClient` 都拥有明确的 HTTPX Transport 生命周期与待关闭状态。推荐内部生命周期：

```text
NEW → OPEN → CLOSING → CLOSED
          ├─ finite request leases
          ├─ finite active SSE watchers
          └─ bounded reconnect / GET poll workers
```

Context Manager 进入时创建/接管 Transport；同步 `with RecorderClient(...) as client` 退出时关闭所有受本实例拥有的 HTTPX 连接；异步 `async with AsyncRecorderClient(...) as client` 对应 `await aclose()`。实现需要区分“SDK 自行创建 HTTPX Client、由 SDK 关闭”和“测试/应用注入外部 Client、由调用方关闭”的显式所有权模式，不可偷偷关闭未拥有的外部 Session。

**关闭顺序**：先置 CLOSED gate/阻止新 API、取消只读自动重连/Watchdog、关闭 SSE EventSource/HTTP Streaming Response、处理在途 HTTP 请求与 Callback/Response 的释放、关闭由 Client 拥有的 HTTPX 实例。对于已开始发送的控制 POST，Close 只断开网络等待，不能保证 Recorder 不执行；OperationAttempt 保留 operation_id 与 POSSIBLY_SENT 信息，并允许上层在新 Client 实例中显式只读查询状态/Topic Evidence。

Async Client 与 Watcher 不应通过 `__del__`、weakref finalize、asyncio.run() on close 实现可靠资源收尾。异步 `CancelledError` 保留取消信号，清理必须用受控 `try/finally`、Context Manager、并对双重 aclose 幂等；若清理也发生异常，应优先不掩盖原始可能已发送的控制操作身份。Client 已进入 CLOSING/ CLOSED 后调用控制方法，在任何出网前明确拒绝并可报告 NOT_SUBMITTED，而不是突然恢复旧 Transport 实例。

多 Watcher/Request 并发的 Owner 图必须可审查：一个 Stream 只由一个消费任务读取；多个 UI Subscriber 若共享一个 Watcher，使用有界分发，不让任意用户 Callback 在网络 Reader 临界区内同步阻塞。连接关闭、SSE 错误、应用取消和重复 Close 时，对实际 Socket、Timer、Watcher 的释放做到“一次且仅一次”。

## 5.2 API 方法使用示例与 Application 职责

以下示例表达预期**公共使用方式**，不是已经可 import 的实现；模型构造函数精确签名可在实现时按 §1.4 收敛：

```python
# Illustrative API contract; ros2_bag_client is not implemented yet.
from ros2_bag_client import RecorderClient, ClientConfig, RecordingTarget

with RecorderClient(ClientConfig(base_url="http://127.0.0.1:8765")) as client:
    runtime = client.get_runtime()
    status = client.get_status()
    # A stopped/idle Recorder has no active target: do not infer one.
    if status.recorder.recording_id is None:
        raise RuntimeError("No active recording to stop")
    target = RecordingTarget(
        expected_runtime_instance_id=runtime.runtime_instance_id,
        recording_id=status.recorder.recording_id,
        recording_generation=status.recorder.recording_generation,
    )
    result = client.stop(target)  # Exactly one POST.
    print(result.operation_id, result.outcome)

    if result.outcome == "UNKNOWN":
        latest = client.get_status()   # Read-only. Does not resolve Stop.
        print(latest.recorder.state)   # Even IDLE does NOT prove Stop SUCCESS.
```

```python
# Illustrative async contract; does not imply an installed module.
from ros2_bag_client import AsyncRecorderClient, ClientConfig

async def watch_recorder() -> None:
    async with AsyncRecorderClient(ClientConfig()) as client:
        async with client.watch_states() as updates:
            async for update in updates:
                if update.kind == "STATE":
                    print(update.status.state_revision)
                elif update.kind == "STALE":
                    print("Status is no longer current")
                # Leaving this scope closes SSE; it does not Stop Recorder.
```

Timed 模式的 Application 倒计时只表达总期限：到 0 表示 Recorder 已触发该 Task 的新消息准入截止，不表示 Writer Drain/Manifest Stop 已完成。CLI/TUI/Web 必须继续显示真实权威状态，不能在本地倒计时归零时伪造“录制完成”。Rolling 的 segment duration 同样不能被 UI 表达成物理文件绝不会超过该秒数的硬实时保证。

**调用方必须显式区分**：向 Recorder 发送一次 Stop、查询当前 Status、查看历史 Recording Detail、进入/退出 SSE Stream。这四种行为没有暗中的级联副作用。Client 可以提供让 CLI/TUI/Web 较易消费的普通 helper（如从 Status 提取 Target、将 Topic Evidence 关联 OperationAttempt），但不能引入为了某个 UI “自动修复” Recorder 状态的隐式 POST。

| Application | Client 使用建议 | 由 Application 定义而非 Client |
|---|---|---|
| CLI argparse | 同步 Client，单次命令/Context，若 UNKNOWN 输出 operation_id 并视需要只读刷新 | 格式、退出码、终端确认与 stdout/stderr |
| TUI Textual | Async Client，一个共享 Watcher/StateTracker，UI 不阻塞事件循环；操作结果与当前状态分栏 | 键盘交互、加载动画、按钮禁用/再次操作确认 |
| Web Starlette | Web Backend 的 Async Client；每个后端实例受控共用连接与只读 SSE 流，浏览器不持 Runtime 秘钥 | Session、CSRF、防跨站、浏览器 SSE 代理与断线展示 |

CLI/TUI/Web 不应直接用 HTTPX 拼请求或各自用 EventSource 修改 Control Outcome。Web 浏览器关闭其 SSE、CLI Ctrl-C、TUI Quit 只终止对应 Client/Watcher，不 Stop Recorder；必须显式拥有 control 权限才能发起录制操作。

## 5.3 SDK 配置校验、秘密与 URL 安全

建议的 ClientConfig 只表达 SDK 网络与本地有界性，不复制 Recorder/Server 内部 YAML 配置。

```yaml
client:
  base_url: "http://127.0.0.1:8765"
  api_version: "1.0"
  connect_timeout_sec: 3
  read_timeout_sec: 40
  write_timeout_sec: 5
  pool_timeout_sec: 3
  sse:
    connect_timeout_sec: 3
    read_timeout_sec: null
    reconnect_initial_sec: 0.5
    reconnect_max_sec: 30
    reconnect_multiplier: 2.0
    jitter_ratio: 0.2
    # Local freshness watchdog must be calibrated against Server OPEN-02.
    local_no_snapshot_timeout_sec: 7
    delivery_queue_capacity: 4
  limits:
    max_json_bytes: 1310720
    max_sse_frame_bytes: 1048576
    max_page_limit: 200
  security:
    follow_redirects: false
    trust_env: false
    verify_tls: true
```

此 YAML 是**表示配置含义的示例**，不要求 SDK 自己依赖 PyYAML；实现可由 Python Dataclass/Env/CLI 配置向 SDK 传参。`local_no_snapshot_timeout_sec=7` 只是相对 Server 候选 2s/5s 的开发初值，非 OPEN-02 已通过 SLA。Status/SSE 完整 JSON/frame 需要在响应接收阶段限制**实际累计字节**；`Content-Length` 不足以防止 Chunked/恶意服务器不受控输入。HTTPX/Parser 若在 SDK 无法介入前就可能无界缓存 SSE 原始帧，必须通过可控包装/替换机制证明内存上限，不能仅在解析完巨大事件后再验证 max_sse_frame_bytes 就声称有界。

校验矩阵：

- BaseURL scheme/Host/Port 受控，仅 http/https，不接受 userinfo@host、embedded Credential、跨 Origin redirect、file/unix 任意传输；远程使用 HTTPS 且证书验证通过。显式 unix socket 模式若未来需要，应单独审查，不属于 V1.0 默认 API。
- 正数 Timeout/容量、允许为 null 的 SSE read timeout、Retry/Backoff/Max Delay 之间有明确有效范围，非法 Config 在发送前拒绝；不能以数值 0 表示“不限制”然后制造无界内存/等待。
- Token/CA/proxy/用户名与请求 Body 日志脱敏；若使用 SecretProvider，读取/刷新失效时只产生本地或查询权限错误，不能给未知认证状态自动尝试控制 POST。
- 每个 Control 请求的请求体是受控的 JSON Schema，禁止 extra 字段注入 arbitrary output_root/插件路径；Pagination 可选参数经 URL query encoding 一次且 URI 由 SDK 固定构造。
- 所有 HTTP Response/Data/SSE/History 的字符串、嵌套、数组长度受有界校验；累计读取可按业务 API 显式分页，不许隐式无限增长。
- 同步 Config 与异步 Config 的默认值必须一致；Python 构造与外部配置注入路径有同一规范性校验，不因不同初始化方式绕开安全默认。

Client 不实现远程 Token 签发或 Web 浏览器 CSRF；OPEN-04 的具体认证/TLS 方案由 Server/Web/部署规范闭合。SDK 必须能安全传递凭证、不意外跨主机重定向、保留 read/control 权限失败诊断，但不能以本地回环为由跳过协议字段验证。

## 5.4 运维诊断、资源指标与日志字段

Client 可提供轻量结构化诊断回调或标准 Python logging Adapter，不引入完整 Telemetry Service。需要记录（在适当权限下）：

| 事件 | 最小诊断 |
|---|---|
| HTTP GET | method/route、request_id（已收到才有）、HTTP status、ErrorCode、elapsed time、响应大小 |
| 控制 POST | command、原始 operation_id、request_id（若收到）、submission、outcome、effects、是否有可信回执、发送/读耗时 |
| Topic Evidence | recording_id、operation_id、evidence_status、可信 topic_id/topic_epoch（可得时）、查询错误与阶段 |
| SSE 连接 | stream_id（若有效）、Runtime ID、revision、重连次数、Backoff、Parser 异常类别、同步/STALE 转换原因 |
| 资源 | active HTTP/SSE connections、未关闭 Watcher、Queued Update Bytes/Items、合并/丢弃数量、最大/高分位 Event 字节数 |
| 关闭 | Client OPEN/CLOSING/CLOSED、活跃任务数、重复 aclose、取消期间是否保留 OperationAttempt |

Secret 不允许进入日志/异常、repr、状态 DTO。`request_id` 是一次 HTTP 尝试；`operation_id` 是 Client 提交的操作身份，不是 SDK 跨 Runtime 持久队列的键。因为 Client 不保证 HTTP 发送后得到对端 ACK，日志不能写“Server definitely never executed this control”除非在发送前确有无请求副作用的证据。

Server ErrorCode/phase 可在客户端日志定位到受保护的 Server 诊断，但不能默认上传原始 ROS Message、MCAP、Manifest 或应用 Cookie。Python Traceback 最多在受控调试通道输出，公开用户提示只保留安全的简明错误与 operation_id，供用户手动后续核对。

## 5.5 依赖、平台与升级边界

SDK 基础依赖锁定选型：`httpx==0.28.1`、`httpx-sse==0.4.3`；Python 3.10/3.12、Ubuntu 22.04/Humble、24.04/Jazzy、AMD64/ARM64 都必须验证。HTTPX 的 CPython 平台兼容不等于每个 ROS 2 二进制包都与 Client 绑定：**本 SDK 不应依赖 ROS Python ABI**，其部署可以通过标准 venv/uv/pip 管理。Python 3.10 的实际安全更新风险应结合 Ubuntu LTS 供应链评估，不把上游 EOL 时间直接当作“Ubuntu 系统不再有安全更新”。

httpx-sse 0.4.3 是已选 Parser，但其生产准入保持 PENDING，特别是 Issue #35 注释/ID 边界、CRLF/Unicode、异步取消/关闭、与 Drogon SSE 真实互操作。SDK 设计应保证：如果最终换成经过验证的上游修复版本或独立成熟 Parser，只修改 `_sse.py` 等内部 Adapter，不改 `watch_states()`、StreamUpdate、CurrentStatus、OperationResult 的公共语义。

其他依赖不作为 V1.0 SDK 必需品：rclpy、Pydantic、FastAPI/Starlette、Textual、CLI 参数解析器、Web Session、中间件自动 Retry、数据库、Prometheus Client 等均留给相关 Application 或独立验收规范。Python 包的正式公开导出、SemVer、安装包体积和依赖闭包需在交付前形成可重复 wheel/lockfile 验证，不依赖当前开发机偶然安装成功。

# 6. 验证矩阵、交付与实施就绪

## 6.1 验证层次与测试输入

验证顺序为**JSON/身份与领域结果 → HTTP Transport/Cancel → SSE Parser/状态同步 → Application 集成 → Drogon 互操作**。不能仅通过正常 HTTP 200 和几条简单 SSE 事件的 Smoke Test 就宣称 Client 合格。

| 测试层 | 输入与环境 | 关键输出 |
|---|---|---|
| Codec/Model | Protocol v1 JSON fixture、畸形/极限响应 | Schema、十进制身份、时间、null、枚举、Version |
| HTTPX MockTransport | Sync/Async 可控响应/异常 | Timeout、发送前失败、可能发送、Cancel、Redirect |
| SSE Parser/StateTracker | SSE 原始字节分块、版本/时钟故障 | 注释、Issue #35、全量替换、STALE、重连 |
| 真实 Runtime | Drogon v1.9.14 + Recorder 或受控 FakeRecorder | Stream 互操作、资源回收、UNKNOWN、慢客户端 |

相同 Protocol fixture 应在同步和异步 API 中得到相同业务结果；真实 SSE 生产准入还要在 Python 3.10/3.12、Ubuntu Humble/Jazzy 和 AMD64/ARM64 环境上复现，不能将 MockTransport 的通过误写为已通过部署矩阵。

## 6.2 HTTP/JSON、控制与证据查询测试矩阵

| 用例 | 注入场景 | 必须通过的判据 | 需求/AC |
|---|---|---|---|
| 13 路由 | 全部 GET/POST、Version、Content-Type | 公共入口和路径/字段一致，无额外控制路由 | FR-CTL-01/06、AC-11/14 |
| Sync/Async | 相同合法/非法响应 | DTO、Outcome、ErrorCode 完全等价 | FR-CTL-06、AC-14 |
| 目标守卫 | 缺 Runtime ID、旧 Recording Generation、旧 Topic Epoch | 不偷换目标，不自动补发 POST | FR-CTL-02/03、AC-11 |
| 429 Admission | CONTROL_QUEUE_FULL | FAILURE/effects:NONE，禁止自动 retry | FR-CTL-03/08、AC-11 |
| 503 Queue Timeout | QUEUE_TIMEOUT、effects:NONE、retryable false | FAILURE，与 429/504 正确区分 | FR-CTL-03/08、AC-11 |
| 504 UNKNOWN | OPERATION_RESULT_UNKNOWN 后 Status 变 IDLE | 原请求仍 UNKNOWN，不因当前 IDLE 判 Stop 成功 | FR-CTL-03、AC-11 |
| HTTP 断线 | POST 已进入 Transport 后 ReadError/Timeout | POSSIBLY_SENT + UNKNOWN，不发第二次 POST | FR-CTL-03、AC-11 |
| ConnectError | 直连、Proxy、自定义 Transport、连接复用 | 只有可证明从未提交才 NOT_SUBMITTED | FR-CTL-03、AC-11 |
| Redirect | 307/308、跨 Origin 与 Authorization | 控制不 Follow Redirect，不泄露认证凭据 | NFR-SEC-02、AC-18 |
| 错误回执 | 不匹配 operation_id、缺 request_id、坏 JSON | 不接受伪 SUCCESS；可能送出时保留 UNKNOWN | FR-CTL-03、AC-11 |
| Cancel/Close | Async POST 发送/读入途中取消 | 保留取消语义和 Attempt ID，不触发 Recorder Stop | FR-CTL-06/07、AC-14 |
| Topic Add/Remove | Graph 多类型、QoS、repeat、Epoch | 不按 name 删除新 Epoch，不隐式选择冲突类型 | FR-CTL-01、AC-11/22 |
| Topic Evidence | 四种 evidence_status，终态与身份冲突 | 只有身份可信的 CONFIRMED 补充 Assessment，原 HTTP 结果不变 | FR-CTL-03/08、AC-11/14 |
| Evidence 失败 | 503/EVIDENCE_UNAVAILABLE、404 Recording、能力 false | 不伪造 NOT_FOUND，不自动重试 Add/Remove | FR-CTL-03、AC-11 |
| History | INCOMPLETE、FAILED、UNKNOWN、null 时间/计数、已删 Segment | 不伪造 COMPLETE，分页有界 | FR-DAT-09、AC-23 |
| Graph | 无 Publisher、多类型、未观察到、频率不可用 | 频率不伪造 0 Hz，观察状态独立于可录性 | FR-APP-06、AC-22 |
| 长整数 | 超过 2^53 的 Revision、ID 与计数；恶意超长十进制字符串 | 合法值保持字符串且不经浮点数，过长字段在 int 转换前有界拒绝 | FR-DAT-06、AC-10 |
| 安全与权限 | 未认证/拒绝、Secret in Errors、Host 变化 | 无越权操作、Secret 泄漏或不安全 Redirect | NFR-SEC-01～03、AC-18 |

## 6.3 SSE Parser、重连与新鲜度测试矩阵

| 场景 | 注入/输入 | 必须断言 | 需求/AC |
|---|---|---|---|
| 首次 SSE 基线 | 首先收到 state.changed 而非完整 snapshot | 不进入 SYNCED，重连/重新建立 full baseline | FR-CTL-04/05、AC-13 |
| 不完整状态 | StateEnvelope 缺 Topic/Recorder/有效性字段 | 整帧拒绝，不将数据 Patch 进旧 Status | FR-CTL-04、AC-13 |
| 相同 Revision | 业务字段相同，observed_at 由真实采样更新 | 本地新鲜度刷新，不虚增领域 Revision | FR-CTL-04、AC-13 |
| 相同 Revision 冲突 | Counter、Elapsed、Disk 或 Validity 改变但 Revision 不变 | ProtocolViolation、不误展示 CURRENT | FR-CTL-04、AC-13 |
| Revision 缺口 | Rev 12 直接收到合法完整 Rev 15 | 接受最新完整状态、记录 Gap，不要求历史 Replay | FR-CTL-05、AC-13 |
| Runtime 重启 | Runtime UUID 更换、Revision 重置 | 旧 View STALE，新 Baseline 后进入 SYNCED | FR-CTL-05、AC-13 |
| 仅注释心跳 / 完全静默 | 连续 heartbeat 或 EventSource 长时间完全无数据、无完整状态刷新 | 独立 Watchdog 在 Reader 阻塞时仍触发 Local STALE 并受控重连，不因 Heartbeat 延长有效性 | FR-CTL-04/05、AC-13 |
| Server STALE→CURRENT | 基于可信旧 Snapshot 发布 STALE，再恢复 | 新 Revision 与全量字段正确，observed_at 不伪造 | FR-CTL-04、AC-13 |
| httpx-sse Issue #35 | 非空 Event ID 后发送纯注释心跳 | 不向 Application 产出伪业务事件，Parser 合规性单独保留测试 | FR-CTL-06、AC-14 |
| 合法空数据事件 | Custom event + 空 data | Parser 可解析，SDK 不把它当 StateUpdate | FR-CTL-06、AC-14 |
| 换行与 Unicode | CR/LF/CRLF、U+2028/U+2029、跨 Chunk、多行 data | 不错拆 Event、不破坏 Unicode 内容 | FR-CTL-06、AC-14 |
| 超大 SSE | 长连续 data、无边界、Frame 超限 | Parser/Transport 内存上界得到证明，不仅是解析后校验 | FR-CTL-04/06、AC-13/14 |
| 握手异常 | 401/403/429/503 或非 SSE Content-Type | 正确 Auth/Error/Backoff，不将错误页面当事件 | FR-CTL-06、AC-14 |
| 重新连接 | 不同 stream_id、带旧 Last-Event-ID | 每次都重建首个完整 Snapshot，不要求无损历史补发 | FR-CTL-05、AC-13 |
| GET 与 SSE 竞争 | 旧 GET 迟到、已接受更新的 SSE Revision | 旧 GET 不覆盖新状态，应用 View 单一版本规则 | FR-CTL-05、AC-13 |
| Async 关闭 | async for Cancel、stream close、半开连接 | Response/Async Generator/Backoff 均归还，不 Stop Recorder | FR-CTL-06/07、AC-14 |
| 慢 Application | 消费队列满、持续 Full Snapshot | 缓冲有界，可替换未消费旧状态，不丢安全 STALE 信号 | NFR-PERF-04、AC-13 |
| Drogon 互操作 | 真实 SSE Chunk/代理/慢读/断网 | 正确解析并释放资源；Server 三层缓冲仍需独立 OPEN-02 准入 | FR-CTL-04/06、AC-13/14 |

不可仅使用手写 SSE 字符串 Mock 代替 `aconnect_sse()` 与 Drogon 的真实交互；客户端与 Server 的各自测试均完成，才能声称端到端已通过。

## 6.4 实施交付、阻断项与执行顺序

必须交付的结果：

1. Python 3.10/3.12 可安装的 `ros2_bag_client` 包、依赖锁、同步/异步共享的模型与 Decoder；无 ROS Python ABI 依赖。
2. 13 路由对应的 HTTP 调用接口、Status/History/Graph/Topic Evidence DTO，以及严格字段/值域/大整数校验。
3. 控制 POST 单次提交、OperationAttempt、NOT_SUBMITTED/UNKNOWN 证据边界、超时与 asyncio Cancel/Close 的资源释放。
4. Topic Evidence 与原始 HTTP Attempt 分离的证据评估；不能直接读取 JSONL。
5. httpx-sse Adapter、StateTracker、全量快照、STALE/同 Revision 刷新、重连与只读 GET 降级；公共模型不含第三方 SSE 类型。
6. ClientConfig 安全校验、TLS/Auth、响应/帧限额、结构化脱敏诊断，CLI/TUI/Web 接入示例。
7. httpx-sse Issue #35、空 Data、Unicode/换行与 Drogon 的实测互操作/取消/关闭验证。
8. 平台/依赖闭包、打包/安装、长期 Watcher 内存与连接泄漏检查、验收证据。

| 未决/依赖事项 | 归属 | 设计与生产状态 |
|---|---|---|
| Topic Evidence C++ 查询 | Recorder / Server | 领域 API 待实现与验证，属于实施阻断项 |
| httpx-sse 0.4.3 | 本文与技术选型 | SELECTED，但 Parser 生产准入 PENDING |
| Drogon SSE 三层缓冲 | Server / OPEN-02 | 必须实测资源上界；Client 成功不代表 Server 准入通过 |
| OPEN-01 | 性能规范 | SDK/应用 CPU 负载是否纳入同机测试按正式统计边界 |
| OPEN-02 | Server/Client 验证 | 连接/时延/刷新/帧预算初始值有待量化 |
| OPEN-04 | Server/Web/部署 | TLS/鉴权与代理方案正式确认前不能宣称远程安全准入 |
| Python 平台 | 技术选型/CI | 3.10/3.12、Ubuntu 22.04/24.04、AMD64/ARM64 需验证 |

建议实施顺序：先 DTO/Codec/Runtime/GET，再六种控制 POST + UNKNOWN，随后 Topic Evidence，再实现 SSE Parser/StateTracker，最后 Watch/重连、应用接入与故障注入。不允许为赶工从 HTTPX Error 直接推断已执行/未执行或删除 SSE 完整快照验证。

## 6.5 Client 实施就绪检查清单

Client 实现和集成之前必须确认：

1. Protocol 的 13 条路由在公共 API 完整体现，sync/async 同语义，Client 公共类型与 HTTPX/httpx-sse 解耦。
2. NOT_SUBMITTED 与可能发送后 UNKNOWN 的证明条件明确；async Cancel 不会自动重发、丢失 Operation ID 或无证据断言 Recorder 未执行。
3. 所有控制 POST 零自动 Retry；429/503/504 与 `effects` / ErrorCode / `retryable` 组合正确。
4. CurrentStatus 的版本、Topic Epoch、计数、null/Unknown、History 与 Graph 的 BEST_EFFORT 含义不变。
5. Topic Evidence 四态与原始 OperationResult 分离；UNCERTAIN/NOT_FOUND 不成为重新 POST 的依据。
6. SSE 由 httpx-sse 完成标准帧解析，Issue #35 的空 Data/注释边界能验证；状态更新全量而不是 Patch。
7. Runtime 重启、Gap、同 Revision 采样刷新、Server/Local STALE、GET 降级都复用一个 StateTracker。
8. 连接、Watcher、Response、缓存、重连与 Application 消费队列都有容量/超时/关闭约束。
9. 凭据、TLS、Redirect/代理、日志脱敏遵从 OPEN-04 安全边界；不能让 UI 会话变成 Runtime 控制密钥。
10. OPEN-01/02/04、Drogon SSE 与 httpx-sse 的发布准入仍明确 PENDING，不以设计完成冒充功能已通过。

本文件定义 Python Client 的开发目标，不表示代码或测试已经完成。若 HTTP/SSE/Recorder 接口不合理，可以直接同步修改设计，但必须保持 SDK 与 Protocol 的操作结果、身份、新鲜度和取消语义一致。
