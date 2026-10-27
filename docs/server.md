# ros2_bag Server 详细设计

| 属性 | 内容 |
|---|---|
| 文档版本 | v0.1.5 |
| 文档状态 | **DRAFT — V1.0 开发阶段，随时修订；规范尚未批准用于完整实施** |
| 设计范围 | 同进程 Recorder Runtime、Drogon HTTP/JSON、控制调用调度、PublishedState、SSE、安全及生命周期 |
| 架构依据 | docs/architecture.md（开发稿，可直接联动修改） |
| 需求依据 | docs/requirements.md（开发稿，可直接联动修改） |
| Recorder 依据 | docs/recorder.md（开发稿，可直接联动修改） |
| 协议依据 | docs/protocol.md（开发稿，可直接联动修改） |
| 选型依据 | `docs/system_technology_selection.md`（Drogon v1.9.14 / C++17） |
| 下游 | `docs/client.md`；CLI/TUI/Web 的 Application 契约在对应实现前补齐，可统一或按应用拆分；另有部署及专项验证规范 |

# 1. 定位、组件与运行边界

## 1.1 目标、权责和设计原则

Server 在**同一 C++17 Recorder Runtime 进程**内实现 `protocol.md` 的 HTTP/1.1 JSON 控制/查询与 SSE 状态通知。Server 不拥有 Recorder 的录制状态、Queue、Pool、Writer、Topic Event、Manifest、Recovery 或文件清理授权；它拥有网络监听、路由鉴权、请求相关性、外部控制串行化、只读查询调度、状态样本发布及订阅者资源。

核心原则：

1. **单一领域事实来源。** 只有 Recorder 的原生操作结果、状态快照和领域只读证据查询有权说明“录制如何变化”。Server 不推导另一套录制状态机，也不因网络应答成功改写 Manifest。
2. **执行域隔离。** Drogon I/O 回调不等待阻塞 Recorder 方法、文件历史扫描或 ROS Executor 协作；网络慢客户端不向 ROS 2 Callback / Writer / Cleanup 施加反压。
3. **操作完成独立于传输。** `SUCCESS` 必须对应确切 Recorder 完成屏障；排队、网络已发送、收到某状态 Snapshot 均不能证明具体请求成功。取消与超时后，Recorder 调用未必结束。
4. **端到端有界。** 排队、在途查询、序列化内存、状态样本、SSE 连接/帧与关闭资源必须具备上限、拒绝或释放规则；仅限制用户态队列不足以证明 Trantor/Socket 有界。
5. **受控 Shutdown。** 禁止在 Writer/Executor 仍依赖对象时凭等待超时强行析构；异常退出与正常终结必须分开标记。
6. **Primitives, not Features。** V1.0 不引入通用任务平台、EventBus、通用 Operation Registry、Broker、数据库、第二套持久化事件解析器或新的跨进程 Server–Recorder IPC。

本规范规定 Server 实现**必须满足的行为和结构边界**；示例类型、局部类名和候选默认预算允许在不改变协议、安全性和所有权契约的前提下调整，并须经真实 Drogon v1.9.14 与目标硬件验收。本文只定义目标设计，不记录源码实施进度或测试通过率；实现状态由仓库代码、验证证据和发布资料管理。

## 1.2 最小组件与依赖方向

```text
                         HTTP/1.1 + JSON / SSE
                                  │
                        Drogon Handler / Filter
                                  │
                   ┌──────────────┴──────────────┐
                   │                             │
             ControlDispatcher              QueryDispatcher
            (单一有界执行域)              (有界只读工作域)
                   │                             │
                   ├─────── RecorderFacade ──────┤
                   │                │            │
                   │         RecordingCoordinator │
                   │          / Recorder domain   │
                   │                             │
                   │                   SnapshotSampler
                   │                             │
                   │                    StatePublisher
                   │                       │     │
                   │                  GET Status SSEHub
                   │                             │
                   └────────── Runtime ──────────┘
                        所有权 / 生命周期屏障
```

| 组件/概念 | 唯一职责 | 不允许做的事 |
|---|---|---|
| `RuntimeHost` | 配置、输出根目录锁、Recovery、ROS Context/Executor、Recorder 和 Server 的启动关闭顺序 | 在 Recorder 未终结时提前停掉依赖 Executor 或释放锁 |
| `HttpApi` | Drogon 路由、参数及 Content-Type、Version、JSON 编/解码 | 等待同步 Writer/Executor 操作；读取 MCAP/Manifest |
| `AccessGuard` | 请求身份、read/control 权限、远程 TLS/受信代理边界、基础限流 | 通过 Origin/Cookie/未验证 Header 隐式提权 |
| `ControlDispatcher` | 唯一外部变更执行域、有界 admission、结果/timeout/关闭状态管理 | 按不同 HTTP 路由同时调用 Recorder 变更方法 |
| `ReadDispatcher` | Recorder status 以外的 Graph/History/Topic Evidence 等有界读取 | 在 Drogon 网络循环执行可阻塞扫描；重写 JSONL 事件语义 |
| `SnapshotSampler` | 有界周期读取 RecorderStatus，向发布域提交经过验证的值对象 | 为每位 SSE 订阅者分别轮询 Recorder |
| `StatePublisher` | 构造 immutable PublishedState、分配 state_revision、GET/SSE 共享 | 以 recording_generation 或 SSE ID 代替状态版本 |
| `SseHub` | 订阅注册、全量基线、发送调度、心跳与慢客户端处置 | 每连接分配独立线程；向 Recorder 高频线程回传网络背压 |
| `ProtocolMapper` | C++ 领域值到固定 HTTP JSON 与错误语义转换 | 直接导出 C++ 指针/原生 ROS Message 或伪造领域状态 |

上述是**职责划分**，不要求每个名词变成独立子系统/动态插件。可将 HttpApi/ProtocolMapper、StatePublisher/SseHub 的轻量类型就近组合。允许在单个 RuntimeHost 内集中配置与生命周期协调，但 Controller 不应成为录制业务逻辑的拥有者。

Server 只依赖 Recorder 的**稳定原生 API / 值对象快照**。Protocol 是独立文档而非运行时进程或模板库；Client/Python、CLI、TUI、Web 与 Server 的 C++ 链接无关。

## 1.3 执行域、所有权和锁顺序

| 执行域 | V1.0 推荐配置 | 可阻塞行为 | 所有权与退出屏障 |
|---|---|---|---|
| Drogon / Trantor I/O | 框架事件循环线程，初始值遵循 Drogon 受控配置 | **不允许**长时间等待 Recorder、fsync、History 扫描或 condition_variable | 框架管理；关闭前禁止回调访问已销毁 Dispatcher |
| Server Control Worker | **恰好 1 个**独立 Worker | 同时最多提交一个外部 Recorder mutation，并同步等待其 `ControlHandle` 到领域 DONE；Writer/Executor 协作在线性化后的 Recorder 域完成 | RuntimeHost 管理；Handle 未 DONE 时不得 detach 或提交下一项外部 mutation |
| Server Read Workers | 默认 2 个、容量有界；与 Control 分离 | 可以执行有界 Graph/History/Evidence/Status 复杂映射；不得进入 Recorder 数据热路径 | 必须在 Recorder 析构前停止新任务并释放所有引用 |
| Server Snapshot Worker | 1 个由 Server 管理的定时采样执行域，可与 Read Worker 复用但**保留采样资源配额** | 允许 `status()` 短时读取，不能长时间阻塞网络回调 | Shutdown 时停止调度并清算在途 snapshot |
| Server State/SSE 协调 | 每一 Runtime 的**单一串行发布序列**；可用现有 Server 调度循环实现，无需独立线程 | 仅对不可变快照进行短临界区复制、发布、排队 | 所有 Subscription 关闭/引用归还后才能销毁 |
| ROS 2 Executor | Recorder 规范默认 1 个 Worker（配置可验证调整） | ROS Callback 与 Executor 协作 | **不能**被 Server Control 自等待关系提前停止 |
| Writer / Cleanup / Recorder Monitor | Recorder 自有，参照 `recorder.md` | 各自限定的存储与资源 I/O | Server 不负责它们的内部停止次序 |

**严格排除**：Drogon I/O Handler 直接执行 Recorder mutation，ROS Callback 同步请求 HttpApi，或者 SSE 发送时持 Recorder 领域锁。Server Control Worker 只串行**外部**请求并同步等待 Recorder 公共调用；真正的领域变更由 RecorderControlSequencer 与内部 Timed Stop/Disk/Writer 事件统一排序。两者不是重复的状态机：Server 不处理内部安全事件，Recorder 不处理 HTTP Admission/Deadline。

所有对象销毁依赖按下面顺序：HTTP Handler / SSE Callback 只能通过 `weak_ptr/lease` 获取 Server 生命周期；Control/Read/Snapshot 任务通过受管理的 `RecorderLease`（可用 shared owner 或 RuntimeHost 在途引用计数）确保引用期间 Recorder 生存；Recorder 最终销毁必须在所有 lease 清零、Worker 真实退出并满足自身关闭屏障后。C++ 示例中的 `shared_ptr` **不允许制造 Runtime–Server–Recorder 的拥有环**。

**锁顺序建议**：尽量没有跨组件嵌套锁。HttpApi 不持有任何锁进入 Dispatcher；Dispatcher 只持短暂自身队列锁执行 admission，锁外调用 Recorder；StatePublisher 的串行发布锁不包裹 `status()`、JSON 序列化、网络 `send()` 或认证逻辑；SseHub 的订阅映射锁不包裹发送。若实现中必须嵌套，需列出严格先后顺序并做 TSan/竞态注入证明。锁保护的是状态与指针交接，不代替 native 完成屏障。

## 1.4 关键数据对象与交接接口

下列类型提供**语义草案**，不是强制的 C++ ABI；实现可以依照项目已有错误/Value Object 风格选取 C++17 明确签名，不能因选择 `Result<T,E>` 或不同模板改变协议完成语义。

```cpp
struct RequestContext {
    std::string request_id;
    std::string operation_id;       // Empty for read-only calls.
    std::string authenticated_subject;
    Permission permission;          // Read / Control
    std::chrono::steady_clock::time_point deadline;
};

struct ControlAttempt {
    RequestContext context;
    ControlCommand command;         // Validated owned value; no Drogon pointers.
    std::string expected_runtime_id;
    RecordingTarget target;         // Optional for Start; task + generation otherwise.
    CompletionSink completion;      // Lifetime-safe, at most once HTTP reply.
};

struct PublishedState {
    std::string runtime_instance_id;
    std::string state_revision;     // Decimal string.
    std::chrono::steady_clock::time_point sampled_monotonic;
    std::string observed_at_utc;
    SampleValidity validity;
    ImmutableProtocolState state;   // Recorder-derived fully owned value.
};
```

`ControlAttempt` 进入工作队列前即完成 JSON Copy、大小/字段校验与目标身份解析；**异步任务不能捕获 Handler 栈上的 request/error 指针或裸 Drogon 回调引用**。`CompletionSink` 是受控弱引用/一次性完成槽，Client 断线后 native 操作允许继续，但 Sink 标记 Abandoned，不能访问已释放的 HTTP Response。

Recorder 标识（recording_id、recording_generation、Topic ID/Epoch、operation_id）、Server request_id/runtime_instance_id/state_revision、SSE stream_id/stream_sequence 均按 Protocol 独立生成与存活，禁止合并为“一个 request UUID”。系统时间与 steady 时间分离：Recorder Timed Stop 使用 monotonic；Server 超时/样本 age/工作队列等待同样使用 steady，不用 ROS 仿真时钟；UTC 用于展示/审计而不是比较截止。

# 2. Runtime 生命周期与关闭屏障

## 2.1 启动顺序、Readiness 与部分失败

RuntimeHost 统一拥有进程启动流程，并显式维护 output_root advisory lock、Recovery、ROS Context/Executor、Recorder 与 Drogon Server 的依赖顺序；任何简化启动入口都不能替代 `architecture.md` §4.1 的完整生命周期契约。

```text
Load & Validate RecorderConfig + ServerConfig
  → 验证远程监听/TLS/Auth 与输出根路径的安全组合
  → 获取 output_root 独占文件锁（不可 unlink 锁文件）
  → 在 Recorder 领域执行必要保守 Recovery，并分类 Findings
  → 初始化受控 ROS Context / Node / Executor
  → 初始化 Recorder，准备其 Worker 与依赖
  → 创建 Server Dispatcher / PublishedState / SSE Hub
  → 启动独立 Server 工作执行域 + 首个可信 Recorder Snapshot
  → 注册 HTTP 路由 / AccessGuard / SSE
  → 启动 Drogon Listener（权限与容量配置已确定）
  → Readiness = READY，允许 control/read/SSE
```

若框架必须先创建 Listener 才能完成部分初始化，则**控制面仍保持 NOT_READY**，初始化完成前不得响应任何控制成功或启动空数据 SSE。`/api/v1/runtime` 可在受限权限下反映 STARTING/DEGRADED；无可信初始全量状态时不能握手成功后发送伪造 CURRENT 快照。Ready 判定同时需要 output_root 已锁、Recorder 已安全初始化、控制 Worker 可用、状态发布域可用、认证安全已成立。

Recovery Finding 分类由 Recorder 负责，Runtime 的决定规则：

| 状态/故障 | 启动动作 | 网络呈现 |
|---|---|---|
| 无法安全取得 output_root 锁、路径/权限异常 | **停止启动**，不开放控制 | 无监听或受限启动错误 |
| Recorder/ROS/Writer 必需依赖无法初始化 | 清理已取得资源，停止启动 | 不允许返回 READY |
| 孤立 INCOMPLETE Segment、Topic UNCERTAIN，可可靠隔离 | 保留证据，允许继续启动 | Readiness 可 READY，但历史查询需如实呈现异常 |
| 未知 Schema/不安全相对路径/可能误删 | 停止可能危害旧任务的恢复/清理；如无法隔离则停止启动 | 不暴露伪造 COMPLETE；必要时 DEGRADED/NOT_READY |
| 部分历史任务不可读，但新任务存储路径仍明确安全 | 保留失败记录并继续（需 Recorder 明确分类） | 历史单项 UNKNOWN/EVIDENCE_UNAVAILABLE，不能默认忽略 |
| Server Port 被占用、TLS/认证依赖错误、Worker 创建失败 | 无“Recorder 正常但 HTTP 仍 READY”的假状态；按逆向依赖安全收尾 | 未建立控制面 |

启动失败不可以为了重试绑定端口而重复开始 Recorder 或再次运行具有副作用的 Recovery。若 Reader/Snapshot Worker 创建中途失败，停止已启动工作域、清理订阅槽、关闭 Recorder；不得把半初始化 Dispatcher 发布给 HTTP Handler。

## 2.2 控制面服务状态与停机准入

Server 自有服务状态建议为：

| 状态 | 意义 | 受理行为 |
|---|---|---|
| `STARTING` | 尚未完成 Recorder/Server 依赖和基线状态创建 | 拒绝 POST；按部署权限允许受限 runtime/诊断 GET |
| `READY` | Listener、控制工作域、状态发布域、Recorder 入口均有效 | 依据 Auth/Config/Recorder 状态决定受理 |
| `DEGRADED` | 只读查询或采样/历史局部异常，但未破坏核心运行安全 | 仅允许**能够证明安全**的操作；不可借 DEGRADED 放宽 Recorder 磁盘保护 |
| `QUIESCING` | Runtime Shutdown 已发起，控制 admission 关闭 | 拒绝新控制、新 SSE；已有连接仅用于可控收尾 |
| `DRAINING` | 正等待已有 ControlHandle、Recorder 工作与订阅退出 | 不能让新请求复活旧工作域 |
| `STOPPED` | 所有依赖都达到真实终结屏障并析构完成 | 不再有 Listener/Recorder 活动引用 |
| `UNSAFE_TO_DESTROY` | 等待耗尽、某 Writer/Executor/Native 调用仍不能安全释放 | **不是 STOPPED**，只能保留依赖或走明确的进程级失败终止策略 |

这是 **Runtime/Server 的生命周期阶段**，不等于 Recorder 的 `IDLE/RECORDING/PAUSED/STOPPING/FAILED`。GET Runtime 可将其映射到 Protocol 的 STARTING/READY/STOPPING/DEGRADED 等枚举，不泄漏内部阶段名；STOPPED 后根本不会再收到正常 HTTP 响应。

**状态切换线性化点：** 关闭 control admission 的原子状态/队列锁同步点。所有在此点之后进入的 POST 一律 `RUNTIME_NOT_READY`；之前成功 admission 的请求仍按同一 Worker 串行执行或被**证明尚未启动**后拒绝，不能被 Server 简单丢弃并当成自然失败。

**Readiness 的唯一 Protocol 映射：** 内部阶段只用于 Runtime 自诊断，对外的 `GET /api/v1/runtime` 不增加新枚举。已获准应答的阶段按下表映射，既不从 Recorder 领域状态推断网络就绪，也不把无法安全析构的 Runtime 伪装成成功退出：

| 内部阶段 | Protocol `readiness` | 接口行为 |
|---|---|---|
| `STARTING` | `STARTING` | 只在受控诊断入口可应答；控制拒绝 |
| `READY` | `READY` | 前提是所有 V1.0 必需能力在当前部署真实可用 |
| `DEGRADED` | `DEGRADED` | 可用只读/诊断，但操作必须遵循 Recorder 安全门 |
| `QUIESCING / DRAINING` | `STOPPING` | 已有授权诊断响应可按有限等待完成；拒绝新控制/SSE |
| `UNSAFE_TO_DESTROY / STOPPED` | **没有正常 HTTP 映射** | 不应继续提供普通 Runtime API；通过进程日志/监管者输出诊断 |

任何构建若 Topic Evidence 等 V1.0 MUST capability 不可用，都不得仅凭 Listener 已就绪就宣称满足**正式发布 READY 的全部能力**；诊断模式必须如实报告 capability=false/DEGRADED，正式 V1.0 发布前必须关闭相应 MUST 缺口。

## 2.3 Signal 处理与 ROS Context 保护

SIGINT/SIGTERM 不允许在 POSIX signal handler 内直接调 Drogon、Recorder、JSON、锁、Writer Close 或 rclcpp 复杂 API。优先使用受控 `sigwait/signalfd` 或最小信号通知（自管 FD/event notification），交给 Runtime 管理执行域发起一次性 Shutdown；多次信号请求只加速有界关闭策略/记录诊断，不能运行并发 Stop 过程。

**关键平台验证**：ROS 2 Humble/Jazzy 的默认 signal handler 可能提前调用 `rclcpp::shutdown()`。Runtime 在 `rclcpp::init` **前**必须决定信号处理所有权，显式禁用与本设计冲突的自动关闭行为或提供等价受控机制；精确 API 参数和版本差异在目标平台原生构建时确定，不能未经版本验证假定某个签名存在。

相同要求适用于独立 ROS Context：异常退出路径不能“先发 shutdown(Context) 再等待需要 Executor 的 Topic remove”。写入线程永久 I/O 阻塞也不能通过将所有线程 detach 然后正常返回 main 来解决——那会破坏 C++ 对象和动态库生命周期。

## 2.4 Shutdown：状态机、预算与不可中断 I/O

```text
shutdown requested (control plane / signal / fatal Runtime error)
  → CAS Runtime READY/DEGRADED → QUIESCING
  → 关闭新控制 POST / 新 Recorder 领域只读查询 / 新 SSE admission
  → 对队列中还未 started 的 Request 完成带 effects:NONE 的拒绝
  → 在途 Recorder ControlHandle 不被网络取消；等待真实领域完成屏障
  → Runtime 直接调用 Recorder.request_shutdown()，取得共享 ShutdownHandle（不经过 Server Control FIFO）
  → Recorder Sequencer 合并 Stop/Shutdown，并保持 Executor/Context 有效
  → 独立 Runtime deadline 等待 ShutdownHandle；确认 Writer/Topic/Pool/Cleanup/Monitor/Task 全部退出
  → 停止采样与只读工作域，结束 SSE/HTTP 连接
  → 关闭 Drogon Listener / loop 并 join Server Workers
  → 验证所有 RecorderLease/Handler 资源归还
  → 真正析构 Recorder，再停 Executor、Node、Context
  → 释放 output_root lock，标记 STOPPED
```

**Recorder Shutdown 的调用域。** RuntimeHost 不在自己的析构线程、Drogon I/O 或 Server ControlWorker 中同步执行可能永久阻塞的 Recorder Shutdown。它调用 Recorder 的 O(1) `request_shutdown()`，由 RecorderControlSequencer 使用保留系统事件推进；Runtime 自己只等待共享 `ShutdownHandle`。这样即使外部 ControlWorker 正等待一个长期未 DONE 的 Stop ControlHandle，或者 CleanupWorker/ResourceMonitor/Writer 被内核 I/O 卡住，Runtime 的 `recorder_grace` Timer 仍能独立运行并把进程标记 `UNSAFE_TO_DESTROY`。只有 Handle 表明 Recorder 全部受管线程实际退出后，Runtime 才允许进入 Recorder destructor/Node/Context/output_root lock 释放。

**这份依赖顺序不是阻塞调用顺序的字面实现。** 进入 QUIESCING 后可以立即停止新 SSE/清理不持 Recorder 引用的网络资源；但是必须维持足够的 Server 响应能力，向尚有接收端的在途控制传达**真实结论**或 UNKNOWN。不能从 Drogon I/O EventLoop 自等待 join；不能停止 Listener 的同时假设所有 Handler 已自动析构。

推荐区分以下独立等待预算，默认值仅是初始实现配置（不是需求 OPEN-02 的已验收时延）：

| 预算 | 候选默认 | 过期后的安全行为 |
|---|---:|---|
| `shutdown.request_grace_ms` | 2000 ms | 拒绝队列中的尚未进入 Recorder 的控制；回复 FAILURE/effects:NONE 或已断线就清理 Sink |
| `shutdown.recorder_grace_ms` | 30000 ms | 若 Recorder ShutdownHandle 尚未完成（含未 DONE 的领域 Stop、Writer/Cleanup/ResourceMonitor/Executor Lease），记录超时原因并报告 UNSAFE_TO_DESTROY；**不得进入 Recorder destructor 或释放其依赖** |
| `shutdown.stream_grace_ms` | 2000 ms | 停止重试发最后一帧，直接关闭/终止仍待发送 SSE |
| `shutdown.read_grace_ms` | 5000 ms | 取消尚未开始的只读任务并回收其租约；已在领域 API 中执行的只读任务若未退出，保留 RecorderLease，过期后转入 UNSAFE_TO_DESTROY，不因超时释放引用 |

**ReadDispatcher 的 Lease 关闭屏障：** 从 QUIESCING 开始，ReadDispatcher 关闭新的 Recorder 领域查询 admission，原子取消/摘除**尚未进入 Recorder** 的排队查询；这些查询的 ResponseSink 可以返回服务关闭/查询不可用的协议错误（客户端断开时只清理 Sink），对应请求和 RecorderLease 均按拥有关系安全释放。已进入 Recorder 只读 API 的 Worker 则进入 `READ_RUNNING`，其 RecorderLease **只能由真正退出该查询作用域的 Worker 释放**；HTTP Deadline 或 `shutdown.read_grace_ms` 仅终结网络等待，不中断底层不安全的文件 I/O，也不能使 Lease 归零。Shutdown 在判断 Recorder 可以析构前必须证明 `active_read_jobs=0`、`active_snapshot_jobs=0`、所有 RecorderLease/回调引用均已归还；仅靠 ReadDispatcher 队列为空不够。

到达 read_grace 后如仍有不可终止的读任务，Runtime 应记录 `UNSAFE_TO_DESTROY`，保留该 Worker 及其依赖的 Recorder、output_root lock、相关线程与仍需的 Context/Executor；停止独立可安全关闭的监听与新 admission，**禁止** detach Reader、强制销毁 Recorder 或谎报 STOPPED。若它之后自然返回，只有在其他 Recorder-domain/Writer/ROS/SSE/Lease 关闭屏障也全部满足时才能恢复到真实收尾；若永久卡死，则按本节的进程级故障终止与监督恢复策略，不把读超时当作安全释放所有权的凭据。RecorderLease 的守恒必须覆盖 SUCCESS、FAILURE、UNKNOWN、客户端断连、队列取消、Shutdown 重入与部分初始化失败。

**有界等待 != 可以安全回收。** 同步 MCAP/存储调用被底层内核 I/O 永久阻塞时，同进程 C++17 不能普遍安全强制抢占其任意调用栈。超过 recorder_grace 后，Runtime **禁止走正常 Destructor 收尾和 STOPPED**。V1.0 默认失败退出方案为：先向受控日志/监督服务写最小错误与相关记录、阻止进一步请求，在最后的进程级故障处理边界由**外部进程监管者通过终止进程并重启**处理不可靠的受阻进程；不要在堵塞线程旁继续析构它引用的对象。若配置明确允许 Runtime 对不可恢复故障执行自身立即进程终止，也必须使用经验证的进程级终止路径（而非 detach + C++ 普通 return），并承认进程级退出无法完成正常 Destructor；残留 Manifest/Segment 交给下次 Recovery。**不能保证这些不可中断场景仍优雅退出**。

关于最后的终止策略，本文选择**优先维护进程内存安全与存储证据、不可伪造成功退出**的决策。具体外部 Supervisor（systemd/docker 等）及最终等待阈值属于部署/验证规范；不得用更大超时掩盖永久阻塞。测试要求使用可控的虚拟阻塞 Writer/FS，不以真实不可控挂盘作为唯一验收。

## 2.5 在途请求、订阅与重复 Shutdown

Shutdown 并发情形的线性化：

| 竞争双方 | 预期行为 |
|---|---|
| 新 Start vs QUIESCING | admission 顺序决定先受理还是明确拒绝；已进入 Recorder 者不可被默默忽略 |
| Stop 已由 Control Worker 提交 vs Shutdown | Runtime 通过 Recorder `request_shutdown()` 提交系统意图，Recorder Sequencer 与已运行 Stop 合并为同一任务终结；Server 不创建第二个领域执行器或第二个 Writer Close 所有者 |
| Topic Add/Remove 等待 Executor vs 停 Executor | 必须先完成/隔离 Topic 调用，确保依赖 Executor 未被抢先停止 |
| GET History 正在扫描 vs 释放 output_root lock | Read Worker 必须实际退出并归还 RecorderLease 才允许正常析构/解锁；超时未退出进入 UNSAFE_TO_DESTROY，不能假装读取完成或放弃 Lease |
| SSE 推送 vs Listener/Handler 关闭 | 标记订阅 Closing，停止生成新发送；在发送执行域进行在途发送终结，然后归还 Stream/Subscription |
| 再次 SIGINT/SIGTERM / shutdown() | 所有请求协作同一共享 `ShutdownState`；同一 cleanup/领域终结不执行两次 |
| 部分初始化失败与 Shutdown | 仅销毁确已构造且未被引用的对象；不要凭布尔开关假设 Writer/Executor 创建成功 |

**对外结果：** 请求 HTTP 连接超时/中断，Client 操作 UNKNOWN；Server 若在 ResponseSink 仍可用时已经取得 `ControlHandle` 的 DONE Result，则按该领域 Result 回应。Sink 先超时后，Handle 最终 Result 只用于日志、状态刷新和受限 Topic Evidence，不维护可供查询的通用操作终态注册表。Recorder 最终明确返回 `DRAIN_TIMEOUT` 时是已知 `FAILURE + effects:POSSIBLE`；只有 Server 在 HTTP 回复预算内尚未取得 Handle DONE，或传输使可信回执丢失，才使用 Protocol `504/UNKNOWN`。SSE 连接断开或 Client 退出不触发 Recorder Stop；只有显式控制、Recorder 内部策略或 Runtime Shutdown 可以启动终结流程。

状态诊断需区分 `active_domain_calls`、`queued_control_calls`、`read_jobs_pending`、`sse_clients` 与 `shutdown_stage`，可在 Server 运维日志/指标中观测；不把这些内部调度细节冒充 RecorderStatus 的权威字段。网络关闭后仍应可通过进程 Supervisor/日志区分成功退出与 UNSAFE_TO_DESTROY。

# 3. HTTP 控制、只读查询与错误映射

## 3.1 HTTP 路由、验证流水线及线程交接

Server 必须实现 `protocol.md` §2.1 的**13 条 V1.0 路由**，不得引入第二套 REST 动词表示相同控制动作。按职责分成：

| 类别 | HTTP Path | 执行域 |
|---|---|---|
| Runtime | `GET /api/v1/runtime` | Drogon I/O 中的短小只读已发布 Runtime 值对象 |
| Snapshot | `GET /api/v1/status` | 读取已发布 StatePublisher 对象；必要时经采样域刷新 |
| Graph | `GET /api/v1/topics/available` | ReadDispatcher → Recorder Graph 发现 |
| History | `GET /api/v1/recordings`、`GET /api/v1/recordings/{recording_id}` | ReadDispatcher → Recorder History API |
| Topic Evidence | `GET /api/v1/recordings/{recording_id}/topic-operations/{operation_id}` | ReadDispatcher → Recorder Topic Evidence API |
| Controls | `POST /api/v1/control/start/stop/pause/resume`、`POST /api/v1/control/topics/add/remove` | ControlDispatcher → Recorder 原生 API |
| SSE | `GET /api/v1/events` | SseHub 建立有界订阅（Drogon AsyncStream） |

推荐路由统一走这条非阻塞流水线：

```text
Drogon Request
 → Body/Header/URI 长度与媒体类型检查
 → 规范 API Version、Method、解码一次的 Path
 → AccessGuard Authn/Authz + rate limit
 → JSON Schema/target + operation_id 格式校验
 → Runtime State / budget admission
 → 强所有权 Command 或 ReadQuery copy
 → 交给 ControlDispatcher / ReadDispatcher（立即返回事件循环）
 → Worker 在 Recorder 真实领域操作/查询完成后生成 ProtocolResult
 → ResponseSink 在可用网络生命周期内至多回应一次
```

**顺序要求**：安全配置校验在监听开始前完成；具体请求鉴权在可能泄露 Topic/History/Runtime 信息前完成；无效请求不能先消耗 Recorder 领域资源。对于认证失败且 body 非法的请求，错误优先级/是否回显 operation_id 应保护身份信息；不能让错误详情泄露存在的 recording_id 或目录。

GET Status 必须与 SSE 取**同一** immutable PublishedState，而不是在两个 Drogon Handler 各自读取 Recorder 再发布两个同 revision 不同数据的版本。最初状态不存在时，GET 返回 STATE_UNAVAILABLE，SSE 握手不得伪造空 Recorder 当前状态。

不得在 Handler 中持有 `HttpRequestPtr` 的裸指针或引用跨异步边界；请求 Context 在 admission 前已复制验证。Framework Callback 可移动到受控 CompletionSink；如果 Client 先断开，Sink 释放网络资源但不取消已提交的 Recorder 领域请求/ControlHandle。

## 3.2 控制队列、Admission 与唯一外部提交槽

**唯一外部控制提交槽**：同一个 Recorder 实例对应一个 ControlDispatcher；其单一 Worker 同时最多向 Recorder 提交一个外部 mutation，并在该 `ControlHandle` 领域 DONE 前保持占用。真正的副作用与内部 Timed/Rotate/Disk/Shutdown 事件都由 RecorderControlSequencer 统一线性化，因此 Server Worker **不是第二个领域状态机或 Writer Close 所有者**。Dispatcher 状态为 `ACCEPTING → CLOSING → CLOSED`，独立于 Recorder 的 IDLE/STOPPING；使用有界 FIFO + 一个正在等待领域完成的项。不得用每条路由各自 mutex、线程池顺序概率或只在 POST 验证阶段锁保护来冒充跨路由串行化。

V1.0 初始执行预算建议：

| 配置/指标 | 建议默认 | 达到上限 |
|---|---:|---|
| `control.queue_capacity` | 32 个等待中的请求，不包括唯一运行项 | 受理前返回 HTTP 429、`CONTROL_QUEUE_FULL`、`FAILURE + effects:NONE` |
| `control.admission_timeout_ms` | 0（非阻塞尝试） | 立即拒绝；不阻塞 Drogon I/O |
| `control.max_queue_wait_ms` | 5000 ms | 若**能证明**仍未进入 Recorder，移出 FIFO 并返回 `HTTP 503 / QUEUE_TIMEOUT / FAILURE / effects:NONE` |
| `control.domain_response_timeout_ms` | 30000 ms | 仅网络回复预算：等待 Recorder `ControlHandle` DONE。到期时返回 `UNKNOWN`（若连接可用），Worker 继续等待领域真实终态，不启动下一个冲突操作 |
| `control.max_outstanding_responses` | 64（含排队与正在等待回应） | 429/503 拒绝新 admission；防止失联连接持有无限 sink |

**值的定位**：上表是可调整、可测量的初始内存/时间预算；不代表已实现或 OPEN-02 性能验收通过。慢 Writer 时 ControlHandle 可能长期未 DONE，后续请求必须有界排队和退避/拒绝，不允许开第二个 Worker 绕开前一个未完成领域操作。

**响应槽层次与公平性：** `http.max_outstanding_responses=128` 是**尚未完成的普通 HTTP 请求及 SSE 握手响应**的全局上限；已建立 SSE Stream 的长期连接不继续占用这一短响应槽，而计入单独 `sse.max_clients` 及 Stream 字节/连接预算。`control.max_outstanding_responses=64` 是上述全局预算中允许由控制 POST 占用的上限（含等待领域结果的响应和网络尚未完成的应答），不是额外可叠加的 64 个 HTTP 槽；Read Query 和握手共享剩余全局槽，必须有受控的读/控制公平性与必要保留配额。一个请求占用一个全局槽；Control POST 再占用一个控制配额，在 ResponseSink 终结、断连被确认或按生命周期完成释放时一次性归还。**Recorder 领域请求可能在应答超时后继续占据唯一 Control Worker，但不继续占据已终结的网络响应槽；其运行责任由单独 domain-running 计数/RecorderLease 管理。** 同一 gate 同步点先验证全局槽及所需类别配额，再按 control FIFO 容量 admission；取得多个配额时必须原子预留或失败回滚，不能让一半成功的 Reserve 泄漏。全局容量不足须明确拒绝未提交的请求，且不能挤占已经获得的 HTTP/SSE 资源。

**完整 Admission 不变量**：

1. 请求验证/授权通过且 Dispatcher ACCEPTING；与 QUIESCING 切换同一受理同步点。
2. **先获得全局 HTTP 响应槽，再获得控制类别配额，最后取得 FIFO 等待槽**；操作必须可原子提交或回滚，避免两个不同预算分别占满造成漏计。额外保留 read/SSE 握手响应能力；任一 Admission 限额拒绝时均明确未执行（effects:NONE）。
3. 操作进入 owned FIFO 时标记 QUEUED，原子取得唯一入队序号；HTTP 接收顺序本身不是执行顺序。
4. Control Worker 从队列取出时，先原子标记 STARTED，再在**实际进入 Recorder 之前**复核 Runtime ID、recording_id + generation、Topic epoch 与 Shutdown gate。Recorder 领域仍进行最终状态裁定。
5. 从此处调用 Recorder mutation 只取得共享 `RecorderControlHandle<Result>`；**Handle 返回不等于领域完成**。Control Worker 保持唯一外部 mutation 执行槽并等待该 Handle 的 `DONE`，直到最终 Result 已捕获前不得提交下一项外部状态变更。
6. HTTP ResponseSink 的 deadline/断线与 Recorder Handle 生命周期独立：Sink 可以先结束并向 Client 返回/形成 UNKNOWN，但 Worker 仍等待 Handle DONE。只有 Handle DONE 后，结果可用于安全日志/状态刷新，且 Worker 才能消费下一项队列任务。
7. 如果某请求的网络 deadline 已过，它的最终领域 Result 可用于日志和状态重新采样，但不能在未经可信关联的其他连接中“补回”一个成功的操作回执。

推荐状态图（只有 Server 队列内存，**不是持久 Operation Registry**）：

```text
VALIDATED
 → QUEUED ────────→ REJECTED_NOT_STARTED
     │                (queue timeout / shutdown; effects NONE)
     ↓
   STARTED → HANDLE_ACQUIRED → DOMAIN_RUNNING → DOMAIN_DONE
                     │                   │
                     │                   ├─ SUCCESS → ResponseSink (if active)
                     │                   └─ FAILURE → ResponseSink (if active)
                     └─ HTTP deadline → HTTP UNKNOWN / sink abandoned,
                                        Domain request continues to real DONE
```

Server 保留当前队列节点直至对应 ControlHandle 真正 DONE，**不**为了满足 HTTP timeout 把仍在 Recorder 中执行的领域请求解离成没有生命周期管理的后台任务。存活的请求对象数量仍受 queue + running 控制，单次领域操作永久不完成是进程故障/关闭屏障问题，不靠无限堆积请求解决。

**Recorder 内部控制顺序器的隔离：** Server 的唯一 ControlWorker **只串行外部 POST 的网络受理与等待**，不执行或替代 Recorder 内部 Timed Stop/Rotate/Disk 状态机。外部请求取得 `RecorderControlHandle` 后，真正的 mutation 与内部安全事件都由 RecorderControlSequencer 统一线性化；Server ControlWorker **不与 Sequencer 形成第二个并发领域执行域，也不竞争 Writer Close/Rotate 所有权**。Producer deadline/disk gate 可以在无阻塞准入点原子拒绝新消息，但只提交/合并 Sequencer 事件，不直接推进 Writer/Manifest 状态。Server 不能把 Recorder 内部事件排进可能长期堵塞的外部 HTTP FIFO，也不能把外部控制请求交给 ROS Executor Timer 阻塞执行。关闭 Runtime 时 Server Worker、Recorder Sequencer 与所有 Handle/Lease 均须真实结束，不能在任一执行线程中 join 自身。

## 3.3 超时竞态、请求目标再验证与结果语义

| 竞态 | 线性化/证据 | HTTP 输出 |
|---|---|---|
| Full Queue 前 Server 可证明未调用 Recorder | Control admission 失败 | 429 FAILURE, effects:NONE |
| QUEUED 和排队 deadline 同时发生 | 队列锁下比较“仍为 QUEUED”与 Worker STARTED；先赢者确定 | 未开始 → HTTP 503/QUEUE_TIMEOUT/FAILURE/effects:NONE；已可能执行且终态未知 → HTTP 504/UNKNOWN |
| QUIESCING 同时收到新 POST | 与 admission 同一互斥/CAS 门 | gate 先关闭 → 拒绝；先入队 → 只允许完成/有证据的未开始拒绝 |
| Stop 的 ControlHandle 超过 HTTP deadline 仍未 DONE | ResponseSink 最多一次 UNKNOWN，Control Worker **继续持有唯一外部提交槽** | 504 UNKNOWN；之后任务进入 IDLE 也不能更改旧回执 |
| Stop 超时 + 内部 Timed Stop 到 IDLE | Recorder 内部与外部交错；外部实际终态未知 | 不能将 IDLE 当该 Stop SUCCESS |
| Stop 超时 + Disk Pause 到 PAUSED | 领域内部安全策略可先发生 | 不能仅凭 PAUSED 判 Stop FAILURE |
| Add Topic 已 durable PREPARED、终态 fsync 失败 | Recorder 只读证据 = UNCERTAIN | 能同步拿到 `EVENT_COMMIT_FAILED` 就 FAILURE/effects:POSSIBLE；HTTP 结果丢失仍 UNKNOWN |
| Old recording_generation 排队时已切换 Task | Recorder Sequencer 在线性化点最终验证发现 STALE_RECORDING | 409 FAILURE/effects:NONE，不作用于新的 Task |
| Old topic_epoch 仍同名 Topic | Recorder 域校验 Epoch | STALE_TOPIC_EPOCH，绝不删除同名新 Epoch |
| Recorder ControlHandle 已 DONE/SUCCESS，但网络发送失败 | 可信成功仅在 Server 内存日志；Client 未见回执 | Client UNKNOWN，Server 不推定 Client 收到了成功 |

**QUEUE_TIMEOUT Protocol 映射：** 当请求曾成功排队但**确定尚未进入 Recorder**，且在队列状态锁下原子移除成功，响应 `HTTP 503 / QUEUE_TIMEOUT / FAILURE / effects:NONE / retryable:false`；满队列且未受理仍为 `HTTP 429 / CONTROL_QUEUE_FULL`；已经可能进入 Recorder、但无可信终态，仍为 `HTTP 504 / OPERATION_RESULT_UNKNOWN / UNKNOWN`。任何队列超时都不构成控制 POST 自动重试许可。

**Recorder 目标守卫是强制接口，而非可选优化：** §3.3 的外部 Worker 二次验证不能解决 TOCTOU。Recorder 对 Server 暴露的 Stop/Pause/Resume/Add/Remove 领域入口必须携带 §1.3.1 定义的 Recording/Topic Guard，并由 RecorderControlSequencer 在领域状态转移线性化点联合校验目标身份；不带 Guard 的旧式入口不得作为 Server 调用路径。Server 已取得 ControlHandle 后也不能依赖先前 `status()` 的身份验证。Stale target 必须在任何本次副作用前确定拒绝且 `effects:NONE`，内部安全事件另走明确受控 Task Lease。交叉测试需在“Server 最后一次验证后/Recorder 线性化前”触发 Generation 变化，证明旧请求绝不作用于新 Task。

**保护检查到 Recorder 线性化的竞态**：Server 可以用一次 Control Worker 上的状态快照做预拒绝，但若随后 Recorder 内部 Timed Stop/Disk Pause 改变状态，只有 RecorderControlSequencer 的最终状态/Guard 线性化点有效；任何“Server 检查后一定生效”的承诺都不合法。不能持 Coordinator 内部 mutex 横跨 JSON 或网络 I/O；目标身份守卫必须由 Recorder 自身的原子/锁状态条件或等价最终验证支持，否则该保护存在 TOCTOU。

**operation_id 保留源值**：Client POST 给出的 operation_id 经验证后原样传递 TopicManager/RecordingCoordinator，不以 request_id 替换。Server 不对请求自动添加“重试一次”的行为，也不缓存“operation_id 已成功”作为幂等键。重复发同 ID 可能在 Recorder 中形成冲突，由其 Event/Domain 返回可信结果，而非 Server 暗中吞掉第二次调用。

**独立 Deadline Scheduler：** `domain_response_timeout_ms` 必须由**不在 Control Worker 上运行**的轻量 Timer/Deadline 调度器触发（可以复用受控的 Drogon timer/Server 调度域）。Control Worker 在 `RecorderControlHandle::wait_for()`/等价 Completion wait 中等待领域 DONE；Deadline Timer 只可 CAS ResponseSink 状态并尝试发送 `504/UNKNOWN`，**不得调用 `cancel_if_not_started()`、取消 Recorder Request、析构 RecorderLease 或释放唯一 Control Worker**。Handle 后续 DONE 时 Worker 捕获真实结果；若 Sink 已完成，只记录安全诊断并触发状态刷新，不重复发送。这一 Deadline 事件执行本身也必须有界且不参与 Recorder 锁等待。

**ResultSink 一次性：** 每次 HTTP Request 无论领域 Handle DONE、deadline、客户端断线、Shutdown 交错，response promise/callback 只能完成/释放一次；重复完成是实现错误。HTTP UNKNOWN 是**本次传输可观察的请求结果状态**，不自动改变 Recorder state 或 durable Topic Event。

**Deadline Scheduler 的完整竞态闭环：** 排队 deadline 同样由独立 Timer 检查，且必须与 Control Worker 的 `QUEUED→STARTED` 在同一队列状态同步点竞争；只有抢先完成 `QUEUED→REJECTED_NOT_STARTED` 并安全摘链时才允许 `QUEUE_TIMEOUT/effects:NONE`。Domain response deadline 只能 CAS Sink 的 `PENDING→UNKNOWN_SENT/ABANDONED`，不得转移 Dispatcher 当前外部提交槽；Worker 先观察到 ControlHandle DONE 并抢到 Sink 时正常发送 SUCCESS/FAILURE，Timer 输掉竞争则不再发送 UNKNOWN。网络回执尝试之后的一次性槽释放与 ControlHandle/RecorderLease 的完成释放分别计数，不得为了节省响应槽销毁仍被领域请求引用的 RequestState。

## 3.4 查询执行、Topic Evidence 与路径安全

GET /status 使用已发布 immutable 快照，不对每个客户端调用 RecorderStatus。其余 Recorder 查询必须走隔离的 ReadDispatcher，初始建议 **2 Workers、队列容量 32、每查询绝对 wall-time 预算 3000 ms**，并对 Graph、History、Topic Evidence 各设置并发与扫描字节/条目上限。超时只限制网络回应，无法安全中断的阻塞文件查询仍须持有合法 RecorderLease，不能杀掉使用对象的线程来释放队列容量。**Read Worker 的执行期与 HTTP ResponseSink 的生命周期独立：** HTTP 超时或断线只完成一次网络请求终态，并停止新的响应尝试；Reader 必须继续持有领域 Lease 直至原生查询真实返回，Shutdown 超过 `read_grace_ms` 时按 §2.4 标记 UNSAFE_TO_DESTROY，不允许释放仍由 Reader 使用的 Recorder。排队而未开始的查询则可在可证明的队列同步点安全取消并释放其配额。

| Query 类型 | 输入 | 领域调用与边界 |
|---|---|---|
| `GET /topics/available` | 有界 name/name_prefix、offset/limit（默认 100，最大 200） | Recorder Graph 的只读发现；多类型、无发布者、未发现、频率无效由 Recorder 结果映射 |
| `GET /recordings` | safe name_prefix、RFC3339 时间、bounded offset/limit | Recorder 域历史索引；不在 Server 重新扫描/解析 Manifest |
| `GET /recordings/{id}` | URL decode once + 安全录制 ID | Recorder 返回部分缺失/UNKNOWN 的只读详情；相对 Segment ID/Path 已验证 |
| `GET /recordings/{id}/topic-operations/{operation_id}` | safe Task ID、UUID operation ID | Recorder `lookup_topic_operation` 领域只读证据；Server 仅映射其判断 |
| `GET /runtime` | 已授权的 read | 返回 Server 控制面能力/Readiness，不泄漏 Host/环境变量 |

**Topic Evidence 是 Recorder 的正式只读领域接口：** Recorder §1.3/§2.4 定义 `lookup_topic_operation(recording_id, operation_id)` 或等价能力，由 Recorder 统一解释 `CONFIRMED_SUCCESS/CONFIRMED_FAILURE/UNCERTAIN/NOT_FOUND`，并处理 fsync 失败、截断和冲突事件等可信性问题。Server **不允许**另建 JSONL Parser 或自行寻找事件行就认定成功。若某开发构建尚未提供该能力，则 `GET /runtime` 必须如实报告 `topic_events_lookup:false`，对应 Topic Evidence GET 返回 `503/EVIDENCE_UNAVAILABLE`；正式 V1.0 的 capability 必须为 true 并通过故障注入验证，不能通过长期设置 false 绕开 Protocol MUST。Recorder 接口的线程、路径安全、扫描预算及持久化可信语义均须经集成测试证明。

ReadDispatcher 的有界磁盘扫描预算应使用 Recorder 接口自己的容量/停止检查，不允许 Server 独自对文件系统建立任意任务扫描策略；如果 Recorder 无法完成有界查询，按协议错误/UNCERTAIN 返回，**不得把扫描不完整误报为 NOT_FOUND**。

路径安全：HTTP 不能接受 output_root 路径、存储文件绝对路径、JSONL 文件路径、ROS Bag 内部文件名来触发读写。录制 ID 只能在受保护的 output_root 下解析，阻断 URL 双重解码、软链接穿越等；物理目录位置由 Recorder 统一管理。需要展示路径时，Server 默认仅返回 `recording_id` 或相对路径，远程 read 不返回宿主绝对路径；本机是否开放带绝对路径的扩展字段须通过独立显式权限与日志审查，V1.0 基础响应不启用。

## 3.5 HTTP Schema 映射和错误处理

HttpApi/ProtocolMapper 的唯一外部规范是 `protocol.md`，尤其 §1.2/§2/§3/§4：

- 顶层 `api_version:"1.0"`；GET 的 request_id + data、POST 的 request_id + operation_id + outcome；JSON snake_case；int64 身份/计数采用十进制字符串，不被 JsonCpp/JavaScript 转为失精度 double。
- Start 成功的 `effective_config.compression` 使用 Recorder 确认的实际生效枚举，禁用时为 NONE；非适用 segment/retention 时长为 null；Timed 总时长和剩余期限有权威 Recorder 来源。
- Start Mapper 只构造 Recorder `StartCommand(recording_id, mode, duration, segment_duration, retention_duration)`；HTTP 不接受 compression/timestamp/QoS Profile 覆盖。`effective_config` 逐字段取 Recorder `StartResult.effective_config`，不得重新读取 server.yaml/mcap.yaml 猜测实际值。
- Pause/Resume 响应原样映射 Recorder `PauseReasons`；Resume SUCCESS 不保证 state=RECORDING，系统磁盘原因仍存在时必须返回 PAUSED + 剩余原因。
- Recorder Status 的 `written_live`、`replayed_to_new_segment` 分开；不得把 Snapshot Replay 加入 live；`cleanup_queue_depth`、`cleanup_pending_total`、`cleanup_queue_full_count` 各表达不同来源，不能在 Server 调整字段统计含义。
- 缺失/不可信资源采样采用 null + valid/diagnostic；不可把 CPU 未采样写 0，或把 History INCOMPLETE/UNKNOWN 写 COMPLETE。
- HTTP 原生失败/未知与 ErrorCode/effects 一起映射：400/409/413/415/422/429/500/503/504 等协议表；无授权响应不能泄漏 recording_id 是否存在。
- GET 不增加 Recorder 领域副作用，SSE GET 可以建立**有界网络订阅对象**，不得因此启动 Recorder Stop/Recovery/Cleanup。

**Topic Evidence 的二维裁定：** Server 必须保留 POST 的原始 `outcome/effects` 与随后 Evidence GET 的 `evidence_status` 两个独立事实。特别是 EVENT_COMMIT_FAILED 的 POST 仍为失败，不能因为重启后 Recorder 重新验证持久文件而改写旧 HTTP 的日志；Recovery 验证过的终态只能说明**现在可确认的持久化领域副作用**。Server 不在内存/Redis 创建补偿成功回执，也不以 Channel/状态推断领域成功。

**异常边界**：C++ JSON ParseError、std::bad_alloc、Drogon Callback 抛错、Recorder 返回 error、文件读取异常都需要在适当 Worker 转为稳定 Protocol ErrorCode 和安全日志，不能让第三方异常越过 Drogon I/O EventLoop 或 ROS Callback 栈。Recorder 已进入领域后的错误映射**唯一服从 `protocol.md` §4.2.1**；Server Mapper 必须对 RecorderErrorCode 做穷尽匹配，不提供“未知枚举默认 500 后继续”的发布路径。OOM 时最低限度是停止 admission/降级服务并记录故障，不假设还能成功序列化复杂 JSON 响应。

**请求体/序列化预算**必须独立校验：最大 HTTP Body 字节、字段数组/嵌套深度、Topic 名称/URI/operation ID、状态 JSON 输出上限、每次序列化最大 heap bytes。若状态超限，应返回协议定义的 STATE_TOO_LARGE 或安全关闭 SSE；绝对不能用遗漏部分 Topic/录制身份的 JSON 冒充“完整 Snapshot”。验证 JSON 保留未知可选字段的兼容性，同时拒绝未知必需的修改语义。

# 4. 状态采样、版本与 GET/SSE 同源

## 4.1 SnapshotSampler：成本预算、单一采样与降级

SnapshotSampler 是**Server 的唯一周期性 RecorderStatus 采样入口**。HTTP GET /status、SSE Client 数量增长不会增加周期性 RecorderStatus 采样次数。控制命令 `ControlHandle` DONE 后可以向采样域发出合并的“尽快刷新”提示，但不直接从 Control Worker 把临时局部状态投递给 SSE。

建议初始配置与状态：

| 参数 | 初始默认 | 约束/理由 |
|---|---:|---|
| `status.sample_interval_ms` | 1000 | 候选 1 Hz；OPEN-02 最终可依据测量调整 |
| `status.min_resample_interval_ms` | 200 | 多控制变化合并，避免每个消息/操作唤醒 status() |
| `status.max_sample_age_ms` | 5000 | Server 认定采样已失效的候选预算 |
| `status.healthy_refresh_interval_ms` | 2000 | 业务不变时发送同 revision 完整快照的新鲜度确认，严格小于 max_sample_age |
| `status.sample_warning_p95_ms` | 100 | 调查触发阈值，不是发布 SLA：相对 1 s 周期 10% |
| `status.max_consecutive_errors` | 1（采样错误立即标注，直到 stale 才发送 STALE） | 不把无效样本提升为 CURRENT |
| `status.max_snapshot_bytes` | 786432 | 初始限制 768 KiB 的 Protocol State Data，保留 Envelope / SSE Frame 的字节空间，实际仍需逐帧校验 |

上述周期与阈值是**可执行初值，未经过硬件验证**。Server 不得用加速采样掩盖 status() 的锁争用；如果 P95 超过周期 10%，先检查 RecorderStatus 分层采集及锁等待，再审采样节奏，不能简单拉长预算使 Client 永远看不到安全变化。

**采样启动规则**：Server 必须取得第一份可信完整 RecorderStatus，并映射完整 Protocol State 后才允许向 SSE 注册首次 CURRENT；首次采样失败则返回 STATE_UNAVAILABLE/NOT_READY，控制是否开放另依据 Runtime/Recorder 安全就绪检查决定。样本必须拥有全部字段有效性信息，即使某些 CPU/RSS/Publisher 数据当前 unavailable/null。

**取样失败规则**：保留最近有效 PublishedState，记录最近成功的 steady 时间；不要将失败时间写入 observed_at。已有完整可信基线且状态 age 超过 max_sample_age 时，发布相同领域数据、`sample_validity: STALE` 的新 state_revision；若无基线或已损坏/Runtime 身份变更，拒绝构造虚假状态并要求 SSE 重同步/关闭。恢复成功重新发布 CURRENT 新版本。ResourceMonitor 某个子系统状态已变 UNKNOWN 也应按 Recorder 真实快照映射，不与 Server 整体采样失效混为一谈。

Server status() 采样线程与 HTTP 读池不能在一个高负载历史列表请求下同时饥饿；采用独立 Worker 或在只读工作池中保留不可被 History/Evidence 使用的采样令牌。每次采样耗时、Coordinator 锁等待（如 Recorder 可观测）、P50/P95/P99、采样错误计数均属于 Server 诊断，不能在 SampleMutex 持有期间输出阻塞日志。

## 4.2 PublishedState 的不可变性、revision 与时间

发布域在每个 Runtime Instance ID 中独占递增 `state_revision`（十进制字符串，从 "1" 起）。**不同 Runtime 的 revision 不可直接比较**。`recording_generation` 是 Recorder Task 身份，不是状态版本；SSE 的 `stream_id/stream_sequence` 是网络传输身份，也不参与发布版号。

规范性状态差异的判定范围：

- Recorder 稳定状态、Task ID/Generation、Mode、Timed duration、用户/系统暂停原因、关键错误诊断；
- Topic ID/Epoch、QoS、Publisher 和实际频率有效性、累计收发/丢弃/写入计数；
- `written_live` 与 `replayed_to_new_segment` 独立变化；
- Topic `actual_hz/frequency_validity` 及 Protocol `gap:true/false/null` 的评价变化；gap 只有 Recorder 可计算，Server 只能映射其真实 nullable 值；
- `topics_removed_recent` / `topics_removed_total`、Queue/Pool、Cleanup 三项计数及 Disk/Process 资源状态和有效性；
- Server 整体 `sample_validity` 的 CURRENT↔STALE 安全变化。

允许变化而不增加 revision 的字段仅是**真正重新采集的纯元信息**，例如 `observed_at` 或源采样时刻，且所有领域字段/有效性均与该 revision 先前发布值一致。**不要把计数器增长、elapsed_wall_seconds 增长、采样有效性变化说成“只是 observed_at 变了”**，它们进入新 revision，但按采样周期合并、不按 ROS 高频消息单独生成版本。

推荐使用不可变对象：

```cpp
struct StatePayload {
    ProtocolState domain;          // Owns all strings, arrays and counter values.
    SampleValidity validity;
    // Domain equality excludes only strictly observational timestamps.
};

struct PublishedState {
    RuntimeInstanceId runtime;
    DecimalRevision revision;
    UtcTimestamp observed_at;
    SteadyTime sampled_steady;      // Not serialized.
    std::shared_ptr<const StatePayload> payload;
    std::shared_ptr<const std::string> serialized_status_data;
    std::shared_ptr<const std::string> serialized_sse_envelope;
};
```

这个类型只是内存/缓存建议，**并不意味着不同 observed_at 的 PublishedState 可被原地修改**。每次成功采样生成一个新 immutable 发布对象，领域内容未变时沿用 revision，重新生成包裹 observed_at 的两个序列化 Envelope；旧对象仍允许被在途 HTTP/SSE Handler 读取。若域字段变化，由唯一发布域分配新 revision。**只有一处**持有比较旧/新 canonical domain 的权力，GET 与 SSE 不分别递增。

状态的两个时间界线：

- `sampled_steady` 用于超时判断与 Deadline，不能通过 ROS Clock 或系统墙钟跳变伪造新鲜度；
- `observed_at` 作为 Protocol 所需 UTC 墙钟展示值，必须确实对应那次 Recorder 成功采样。客户端也应结合本地单调接收时间判断连接是否持续更新；跨设备 UTC 时钟偏差可能存在，不能把 Client 本地与 Server UTC 时间差当成唯一新鲜度证明。

## 4.3 单一发布序列与订阅竞态线性化

**发布操作一次只由一个发布域执行**，并发工作线程提交的状态样本按实际完成/采样顺序由发布域裁定，不能让一个更旧的 RecorderStatus 晚到后覆盖已发布的新状态。V1.0 默认只有一个周期采样任务在途：`sampling_in_flight` 防止重复并发快照，控制“尽快刷新”只是合并置位标记，不能启动另一个不受控的采样。

处理流程：

```text
RecorderStatus sampled on SnapshotWorker
  → build complete Protocol State off locks
  → validate boundedness / run identity / valid fields
  → compare canonical domain with latest StatePayload
  → (new domain? new revision : same revision, newer true observed_at)
  → immutable PublishedState published to atomic/shared pointer
  → SseHub notify publisher event
  → GET /status loads identical published object
```

要求两个线性化点：

1. **PublishedState 发布点**：新的完整对象原子替换旧对象，可由 GET 读取；此时已被“发布”但不代表 SSE 客户端收到。
2. **Subscriber Register 点**：在同一个发布协调序列里，登记新 subscriber 并复制当时最新的 PublishedState，将其作为该连接**第一个业务状态帧**入队。其后发布的 Revision 只能在这个初始基线之后入队，不能越过。

因此，GET /status 和 SSE 的同 revision 业务内容必须一致；GET 可能恰好比 SSE 早/晚观察到某个版本，这是合法的实时竞态。Client 依据 Runtime ID + revision 排序/替换，不能因网络到达顺序不同而让旧快照覆盖新快照。

**采样空窗处理**：订阅注册成功后发起或保证进行一次**订阅后**采样；如果注册前 Recorder 已改变但协调器尚未观察到变化，后续采样必须把它纳入 published domain。发布协调器不要求一个完整事件历史，也不为每条 ROS 消息生成 revision；Client 仅保证最终获得最新已观察状态的完整快照。

首次注册期间如果基线 JSON 超过预算，不得注册一个“等待永远不会发出的快照”的 SSE 订阅；优先在握手前返回 STATE_TOO_LARGE。已注册且待发送初始快照挤占队列达到限制时，只允许以更新的完整基线取代尚未输出的初始帧，或者 resync_required/断连，不能发送没有身份的残缺 JSON。

## 4.4 Status 的有界序列化与观察指标

HTTP Status / SSE StateEnvelope 是 Server 映射产物，不能把 RecorderStatus C++ 类型或完整 Topic Runtime 引用暴露给 Client。每次采样到状态后，Mapper 应产生**同一协议版本**的纯值对象；所有动态数组有明确 Recorder/Server 上限，转换超出预算时拒绝发布、标记状态服务不可用并保留最近可信值用于 STALE（若可安全发布）。

示例 1 MiB 状态预算是**完整序列化 JSON 的字节上限**，不是“如果太大就截断 Topics”。计算包含 UTF-8 编码后字节、JSON 转义、全部 Topic 数组和 ErrorRef；按实际序列化上限验证，而不只靠 topic 数 × 平均字节粗估。重复同一序列化数据给多个 Subscriber，可共享 `shared_ptr<const string>` 以减少应用态拷贝，但不能把这些共享引用漏算进生命周期与资源预算。

| 指标 | 归属 | 重点 |
|---|---|---|
| `status_sample_duration_ms` P50/P95/P99 | SnapshotSampler | 录制高负载时是否干扰 Recorder |
| `status_sample_failures_total` / `status_sample_age_ms` | SnapshotSampler | 失效/恢复可追踪 |
| `state_revision` / `state_publish_count` | StatePublisher | 版本递增是否来自实际领域变化 |
| `sse_refresh_same_revision_total` | StatePublisher/SseHub | 空闲但健康时更新真实 observed_at |
| `sse_resync_required_total` | SseHub | 基线、慢发送/无法保持一致性 |
| `status_bytes` / `status_too_large_total` | ProtocolMapper | 状态对象有界且不截断 |
| `status_get_snapshot_age_ms` | HTTP mapper | GET 过期是否正确报 STATE_UNAVAILABLE |

**GET 新鲜度策略**：客户端读 GET Status 时，如果当前状态对象 sample_age 已超过 max_sample_age，优先请求合并的采样并在安全响应预算内等待最新样本；若无法得到有效 CURRENT，返回 `STATE_UNAVAILABLE`（或仅在协议明确允许的状态用途下返回显式 STALE，不能默认为当前）。SSE 可以发送带 STALE 的完整旧样本供展示诊断；不因 HTTP GET 频率提高 Recorder 快照采样频率。

# 5. SSE Hub：Drogon Streaming、有界缓冲与重新同步

## 5.1 异步 Stream 接口与连接所有权

Drogon v1.9.14 的基础能力是 `HttpResponse::newAsyncStreamResponse(callback, disableKickoffTimeout)`、`ResponseStream::send()`、`ResponseStream::close()`。**它们不是完整 SSE 框架**：帧编码、Subscriber Registry、连接回收、慢客户端预算、事件身份和重同步由本项目实现。选择异步 Stream，不使用 `newStreamResponse` 的同步拉取循环，也不复制官方“每个 SSE 连接启动线程”的教学示例。

建议独立 `SseSession` 生命周期为：

```text
HTTP GET /events validated / read access / capacity available
  → reserve subscriber slot and initial frame budget
  → register with StatePublisher; capture immutable baseline
  → construct Drogon async HTTP stream response
  → callback obtains owned ResponseStreamPtr
  → Session transitions HANDSHAKING → STREAMING
  → on designated Stream send domain: send initial state.snapshot
  → flush bounded latest-state notifications / comment heartbeat
  → stream failure / timeout / shutdown / overload
  → mark CLOSING, unlink registry, forbid new send
  → close stream once if still valid, release queued frames
  → CLOSED, return slot and all subscriptions
```

推荐 Session 单向拥有 Stream（unique_ptr/框架分配），SseHub Registry 持有 Session 的受控强引用；异步 Callback 捕获 weak lease 或受控共享 Session，避免 Session ↔ Hub ↔ Stream 互相保活。Context 含 `stream_id`、下一 `stream_sequence`、最后完整发送帧、BufferedSnapshot、PendingBytes、closing flag、最后成功送入 Stream 的时间等。注释心跳不增加 stream_sequence；resync_required 占一次序列号。

**Callback 时序需要实验确认**：Drogon AsyncStream callback 和底层连接关闭回调发生顺序可能与 HTTP Handler 生命周期不同；不能依赖某个 undocumented destructor 作为唯一退订手段。必须覆盖“请求已经鉴权，但 Stream callback 从未启动”“刚取得 ResponseStreamPtr Client 断开”“send(false)”“close 重复”“Runtime Shutdown 与 callback 交错”，确保每个 Reserve 都有单次 Release。Callback 未建立 Stream 也需要握手 Deadline 回收 slot，不能悬空永久注册。

HTTP 响应必须为 `text/event-stream; charset=utf-8` + no-cache/no-transform，并确保实际 Async Stream 的 chunked/Connection 语义按目标版本处理。**无控制面身份也不得升级为 SSE**。不应对 SSE 输出启用代理压缩或不受控反向代理缓存，不能借响应 Header 声称已完成网络发送。

## 5.2 SSE 帧编码、心跳及连接有效性

SSE Encoder 仅接收完全准备好的 Protocol StateEnvelope 或受限 resync reason；字段顺序与 JSON 内容遵守 `protocol.md` §5：

```text
id: <stream_id>/<stream_sequence>
event: state.snapshot
data: {"api_version":"1.0","state":{...完整 Protocol State...}}

: heartbeat

id: <stream_id>/<stream_sequence+1>
event: state.changed
data: {"api_version":"1.0","state":{...完整 Protocol State...}}

```

以上 `...` 为伪代码占位，**不是合法完整的实际 JSON 报文**；生产 Encoder 生成完整、单行、正确 JSON 转义的数据并在尾部加一个空行。禁止把不同 JSON 分块的边界与 SSE 业务消息边界混淆：即使底层 ResponseStream::send() 用 HTTP Chunked 传输，Client 收到的仍应是按照 SSE 空行分隔的完整事件。

| 帧类型 | 触发 | Revision |
|---|---|---|
| `state.snapshot` 初始基线 | Subscriber Register 串行同步点 | 取已发布值的 revision |
| `state.changed` | 某个观测到的规范性领域状态改变 | 由 StatePublisher 递增 |
| `state.snapshot` 同 revision 新鲜度确认 | 业务字段不变、Recorder 再次成功采样且即将到新鲜度预算 | 同一 revision，不虚增版本；observed_at 来自真采样 |
| `stream.resync_required` | 连接无法保证有效完整基线或缓冲连续性 | 事件帧 ID 新增，但**不含伪造的 Recorder State** |
| `: heartbeat` | 连接长期空闲或控制代理保活 | 没有 event/id/data，**不表示采样新鲜** |

初始建议 `sse.heartbeat_interval_ms=15000`，但如果每 2 s 已发送全量快照，可以省略多余心跳或限制频率，不必在每次正常状态变化时额外发 heartbeat。Parser 合规性须测试 **注释心跳、空 data、LF/CRLF、UTF-8、U+2028/U+2029、异常终止**；Server 自己只选择注释行作为心跳，不能因此教 Client 忽略 SSE 标准中合法的空 data 事件。

**只允许在同一 Stream 序列化域里调用 send()**，除非锁定版本的官方实现与实测证明跨线程 send 安全。状态采样/Control Worker 只能通过任务投递和共享 immutable Frame 触发发送，不能直接从这些线程握住 ResponseStreamPtr 写出。

## 5.3 首次订阅与重连的不丢失基线

必须满足 `protocol.md` §5.3：“注册 Subscriber”和“取得初始 PublishedState/将其置为首帧”处于同一个单一发布域顺序。推荐使用两个阶段：

**Phase A：admission + 创建预发布订阅。** AccessGuard 检查 read 权限、版本、限流与 SSE 总容量；StatePublisher 串行注册 Subscriber，得到 Last PublishedState immutable 引用。初始帧先放进固定大小的优先槽，后续变化进入 pending 槽。不得先放弃状态锁、再异步去请求一个可能已经过期的 Snapshot 然后当新 Client 的基线。

**Phase B：获得 Stream + 异步送出。** Drogon Callback 将合法 Stream 实例移交给 Session；按序发送初始 `state.snapshot`，随后从 pending 最新完整快照继续发送。每次注册后由 SnapshotSampler 安排一次合并的**订阅后采样**，消除“Recorder 在最近一次发布后改变，发布域尚未看到”的空窗。既有 Subscriber 的发布不会被新 Client 阻塞。

如果 Phase A→B 期间多个状态变化发生，所有消息仍是完整快照；可合并等待中的过期状态，**绝不能让后续 state.changed 越过初始 state.snapshot**。若初始 slot 无法保证内存/发送预算，则拒绝握手或断开该 Session 重新同步，而非发送没有完整 Topic/Task 身份的残缺状态。

Last-Event-ID：客户端带旧连接的 Event ID 不用于历史重放；每次新 GET /events 创建新 `stream_id` 且首个业务事件为完整 `state.snapshot`。Server 不维护跨连接持久事件日志；状态 revision 可帮助 Client 判断新旧状态，**不代表中间所有变化已送达**。

## 5.4 慢客户端与三层发送缓冲

必须分别核查：

1. **应用订阅缓冲**：SseSession pending 完整帧的个数、字节、合并/丢弃决策，Server 可以严格计数。
2. **Drogon/Trantor 内部缓冲**：AsyncStream 将 Chunk 排入框架发送队列后的驻留字节；不能从 `send()==true` 推断这一层已腾空。
3. **Socket/内核发送缓冲与中间代理**：受 TCP 窗口/客户端读取速度影响；超时关闭和缓慢消费者内存增长需要在真实网络下验证。

**生产准入必须覆盖三层**，否则“应用队列有界”不能宣称 SSE 总体有界。若锁定 Drogon API 不能观测或充分约束内部待发送字节，应通过可验证的最大发送速率/发送前拦截及强制断连维持上界；如果仍无法证明慢客户端资源安全，应阻断 Drogon SSE 生产准入，评审局部适配或替代实现，不得默许内存无限增长。

初始候选预算（所有数字需按 OPEN-02 实测复核）：

| 资源/指标 | 默认候选 | 满额/超时动作 |
|---|---:|---|
| `sse.max_clients` | 64 | 握手前 HTTP 429/503 |
| `sse.max_event_bytes` | 1 MiB（包括 frame header/UTF-8 data） | 拒绝完整状态发布、标记 STATE_TOO_LARGE；不得截断 |
| `sse.pending_frames_per_client` | 2 | 等待中的完整状态优先替换为最新完整状态；无法安全替换时断开 |
| `sse.pending_bytes_per_client` | 2 MiB | Coalesce 或 resync_required / close |
| `sse.global_pending_bytes` | 64 MiB | Admission/广播必须服从全局原子预算；不足时拒绝或释放慢连接 |
| `sse.global_egress_rate_bytes_per_sec` | 8 MiB/s 初始保护值 | 拒绝新订阅、降低有效连接数或合并未发全量帧；不得无限排队 |
| `sse.handshake_deadline_ms` | 3000 | 尚未得到 Stream 的 Session 回收 slot |
| `sse.stalled_send_ms` | 10000 | 判定该连接不可持续保有资源，发可发的 resync_required 后关闭，或直接关闭 |
| `sse.max_send_batch_frames` | 2 | 每次分批发送后让出 Server 发送域，避免某个 Client 占满 EventLoop |

**预算耦合**：64 Clients × 1 MiB 全量状态 × 0.5 Hz 刷新 ≈ 32 MiB/s，明显**超过**示例 8 MiB/s 全局预算。因此 64 是连接槽上限，不是“64 个最大尺寸状态 Client 可同时按 2 秒刷新”的服务承诺。Admission 必须根据实际当前帧大小、刷新期限、可用发送预算和已有 Clients 动态计算可维持容量；预算不足时拒绝新连接或明确断开慢连接，**不能通过推迟所有 Client 的新鲜度刷新超过预算来维持名义连接数**。可使用共享完整帧降低用户态重复编码成本，但实际 Socket 传输的 N 倍字节不能省略核算。

**可实施的 Admission / Egress 策略（待 OPEN-02 验证）**：

1. 在握手前取得最新**实际编码后**的全量 `state.snapshot` 帧字节 `B`（包括 SSE header、JSON 转义和空行），取安全刷新间隔 `T = status.healthy_refresh_interval_ms / 1000` 秒（初始 **2 s**），而**不是** `status.max_sample_age_ms / 1000`（初始 5 s）的最后容忍期限：每连接基础保活流量约 `B/T`，还须为 `state.changed` 峰值、握手初始帧、心跳、JSON 字段增长与调度抖动留显式余量。OPEN-02 若调整候选健康刷新值，则重新计算 T 与 Admission 预算；不能把 5 s 的失效边界直接当作持续保活承诺来虚报可服务并发数。对当前 Subscriber 的承诺速率总和不能超过可用的全局 Egress 预算，不能只数连接槽。
2. 以全局 Token Bucket/等价速率控制器限制**实际排入 Stream 的编码字节**；每个 Subscriber 又受 pending-frame、pending-bytes、握手及单次发送批量限制。应用级计数必须包括尚在调度中的帧；若网络 API 不能提供下游真正已排空的证明，该速率限制**不是**内部/Socket 缓冲已解除背压的证明。
3. 对每个 Subscriber 维护 `last_actual_sample_time`、`last_frame_submitted_time`、下一次最迟健康刷新期限及受控 stall 诊断。发送调度首先保证没有 Subscriber 超过 `status.max_sample_age_ms` 的承诺；在距离期限只剩调度/编码/网络安全余量时，如果预计不能交出有效完整帧，**优先标记 Closing 并断开该慢连接**，不能无限维持心跳或拖慢全部连接。
4. 正常 `status.sample_interval_ms=1000`、`healthy_refresh_interval_ms=2000`、`max_sample_age_ms=5000` 是候选值：可实测 `sample_duration + publish_delay + enqueue_delay + network_stall_guard < max_sample_age` 才能声称存在安全刷新余量。**Server 只能证明已提交完整帧给流式传输，不保证 Client 一定接收**；Client 仍依赖自己的超时/版本判断。
5. 如果 status 帧在现有 `sse.max_event_bytes` 下不合规或编码上限增长，使可维护的并发数变小，动态拒绝新订阅/受控驱逐较慢客户端；当三层缓冲缺少可观测的下游进展信号，必须通过真实慢读测试证明强制关闭能阻断无限缓冲，否则 SSE 生产准入失败。

**候选 Stall 时限不应覆盖新鲜度期限**：例如 `sse.stalled_send_ms=10000` 大于 `status.max_sample_age_ms=5000`，所以十秒仅是网络 stall 的粗粒度诊断上限；**新鲜度预测或超过五秒预算的 Client 要更早触发重同步/断连**。不能等满十秒才让仍宣称 CURRENT 的连接退出。

**完整帧校验位置：** ProtocolMapper 先生成完整 `StateEnvelope` JSON，再由 SseEncoder 加 `id:`/`event:`/`data:`/空行，按**最终编码后的 UTF-8 字节数**比较 `sse.max_event_bytes`，并计入 `sse.pending_bytes_per_client`/全局 ByteBudget。`status.max_snapshot_bytes=786432` 是 Protocol State data **已编码为 JSON/UTF-8 后的实际字节数**的初始上限，不代替 GET JSON/完整 SSE 帧的各自校验；编码预算包含 Json Escape/Topic 字符变化。`sse.max_event_bytes=1048576` 是最终**整个 SSE 业务帧**（`id:`、`event:`、`data:`、业务 JSON 与结束空行）的上限，不是 JSON Payload 上限。

**256 KiB 余量的精确含义：** `1048576 - 786432 = 262144` 字节，只是**两个独立预算之间的静态差额**，不是对最坏情况 JSON 转义、Unicode、嵌套字段、协议包装或未来扩展的充分性证明。所有 State 字段先按统一 JSON 编码规则形成真实 Data；GET Envelope 和 SSE Envelope 分别编码，然后在 SSE Encoder 加帧头并再次计算 UTF-8 总字节，三道上限均独立通过才允许发送。极端 Topic 名称或错误诊断字符串令任一上限超标时按协议明确拒绝/关闭，不能假定余量够用、先提交半帧或静默截断。若在预编码阶段要做成本保护，保守估算只可作为提前拒绝，最终判定始终以实际编码字节为准。GET 已取得有效状态但超出响应限制时按 Protocol 返回 `503/STATE_TOO_LARGE`；SSE 若在握手前发现超限则返回握手前 503，已建立 Stream 则尽力发送 resync_required 后关闭或直接断连。**不能截断必须完整的 Topic/身份字段，也不能分拆业务状态为多个“半完整” SSE 事件。**

**Coalesce 规则**：仅合并**尚未交给 Drogon 内部缓冲**且保有完整帧边界的 pending `state.changed/state.snapshot`。可替换为较新的完整 PublishedState（保留新 revision、observed_at）；被部分提交至底层 AsyncStream 的帧不再重新使用其 Buffer，也不能和另一个帧做字节拼接。每次合并增加诊断计数；如果无法观察当前已交出的缓冲和真实消费进度，不能无限 `send()` 仅靠 pending 变空自认为无背压。

慢连接终止：先把 Session 标为 Closing、阻止新入队/生成，再在受控的发送执行域 close Stream 一次、注销 Subscriber、归还全部预算。无法写出 `stream.resync_required` 的场景直接断连是符合协议的；Client 断连后必须重新拿首帧全量状态。

## 5.5 断连检测、发送公平性与关闭竞态

SseHub 必须保证：每个订阅连接有唯一 Stream 生命周期身份，退订和资源归还**幂等**，任何 late publish callback 只可看见 weak/closed Session 结果。不得依赖 Client 主动发送“退出请求”做回收；HTTP/2、多路复用或浏览器 EventSource 连接管理不属于 V1.0 网络传输基线。

Drogon `send()` 返回失败可作为关闭线索，但**成功不代表对端仍在线**；应结合实际框架断连可观察回调、受控 Stream 生命周期、网络探测/超时和慢读压力测试。对 HTTP/1.1 `Connection: close`、代理关闭、Half-open、RST/FIN、突然 Client Exit、Server 重启均须做真实互操作，不能只测“正常 SSE 一分钟”。

发送公平性：

- 任何一位订阅者不能占有长时间阻塞锁或 I/O 线程；每轮发送有界批量。
- 快照序列化由发布域提前完成；EventLoop 不为每个 Client 重做大 JSON 编码。
- 广播遍历 Subscriber Registry 时只复制受控的 Session Handle 集合，锁外分发；避免在 Registry Lock 下调用发送。
- 新订阅者永远先拿有效 full state.snapshot；旧客户端错过中间 revision 后接受新的合法完整状态并替换，不把 Revision gap 解释为 Topic Event 永久丢失。
- 如果状态来源已经 STALE，SSE 可以发送基于旧可信快照的 STALE 全量版本，但**不能**用只有注释心跳的连接假装 CURRENT。
- Runtime 进入 QUIESCING 时停止新订阅，旧连接最多进行有界收尾，然后关闭；**即使网络没有被关闭，Recorder 也不能因为某个 SSE Subscriber 在线而延迟自身保护性 Stop**。

**交付帧的新鲜度保证边界**：Server 能在自身调度域确认“某个时刻成功取得新 Recorder 样本并尝试把完整帧交给 Drogon”，但没有 Client ACK。文中“健康刷新必须在期限前成功发送”的可执行意思是**期限前完成受控的有效采样、编码和本地非阻塞帧提交，且经过已验证的慢消费者资源限制**。若无法证明这一条件，不能继续无条件保留 Subscriber；Client 的真实接收期限由 `client.md` 独立判定。任何 `send()==true` 都不能冒充应用层 Delivered ACK。

**SSE 诊断**至少包括 active/handshaking/stalled Clients、pending frames/bytes、global bytes、encode failures、send failed/close reason、coalesced updates、last full snapshot time、runtime/state revision、subscriber lifetime、最大与 P95 帧大小/广播延迟。日志不得记录完整 Topic Payload 或任何认证 Secret。

# 6. 安全、配置、观测与验证

## 6.1 访问控制、TLS 与 Web 安全边界

`protocol.md` §6 定义 `read`、`control` 权限；Server 落实认证、授权和部署默认。V1.0 默认**仅监听 loopback 地址**（例如 `127.0.0.1`，如启用 IPv6 必须显式验证 `::1`，不可默认为任意接口），但 loopback 也不匿名绕过 Runtime 鉴权。所有 Runtime HTTP/SSE 请求都验证预共享不透明 Bearer Token。任何 `0.0.0.0`/公网/容器端口映射/反向代理开放都属于远程可达场景，除 Bearer Token 外还必须满足 TLS 与受信代理配置；不满足时配置校验拒绝启动，而不是“启动后发出警告”。

| 安全控制 | Server 约束 |
|---|---|
| 默认监听 | Loopback、本机/用户信任边界；仍要求 Bearer Token，不提供匿名 Runtime API |
| Runtime 鉴权 | 预共享高熵不透明 Bearer Token；CredentialProvider 返回稳定 subject 与 `read`/`control` scope，`control` 包含 `read`；缺失/无效为 401、scope 不足为 403 |
| 远程访问 | `remote_enabled=true` 时除 Runtime Bearer Token 外必须启用 Drogon TLS，或配置显式受信的 TLS 终止代理；代理不替代 Runtime 鉴权 |
| 请求关联 | Request ID 由可信 Server 生成；operation_id 校验 UUID/长度/字符并原样传递；Server 不维护去重 Registry，已知语义复用只记录诊断，由 Recorder Evidence 判断冲突 |
| Topic/History read | 不把 read 权限理解为无限文件浏览；筛选合法 Task/Topic 与受控元信息 |
| Token/凭证 | 不进入 JSON 状态、SSE、Error.message、普通访问日志；认证失败响应最小化 |
| 信任代理 | 只接受配置过的可信代理 IP、Header、TLS 终止链；不相信任意请求头的 X-Forwarded-* |
| CORS | 默认不向任意 Origin 开放敏感 API，跨域策略受显式配置与鉴权双重约束 |
| Web Session/CSRF | Starlette Web 后端负责浏览器身份与 CSRF；浏览器不直接拥有 Recorder Runtime 控制密钥 |
| Rate Limit | read/POST/SSE 按身份、来源及全局限额多级 admission，不能绕过控制串行化或查询预算 |
| 路径安全 | URI decode 一次后的录制 ID/Topic/operation_id 校验、root-only 文件读取、禁止任意路径字段 |
| Remote 日志/诊断 | 对用户路径、网络身份、凭证和错误堆栈按配置脱敏；不可从 404 差异窥探无权访问任务 |

**OPEN-04 的 V1.0 方案已经裁定。** Runtime 不引入用户数据库、OAuth/OIDC 或 Token 签发服务；采用预共享高熵不透明 Bearer Token，并将每个凭据绑定到稳定非敏感 `subject` 与 `read`/`control` scope。Token 只能由受控 `CredentialProvider` / Secret 文件装载，Secret 文件必须按最小权限部署，普通 `server.yaml` 只保存 Provider 配置或 Secret 引用，不保存可打印的明文 Token。远程 TLS 可采用 Drogon 受控 TLS Listener，或由显式受信代理终止；后一种情况下只信任配置的代理来源及转发 Header，Runtime 仍自行校验 Bearer Token。V1.0 Token 轮换可通过更新 Secret 后安全重启 Runtime 完成，不要求在线签发/撤销服务。该设计已闭合，但正式远程准入仍必须通过 AC-18 的证书、代理、Scope、泄漏、Web Session/CSRF 与跨 Origin 攻击测试。

**File CredentialProvider 契约。** V1.0 基线 Secret 文件格式固定为 YAML：顶层 `version: 1` 与非空 `credentials[]`；每项只允许 `subject`、`token`、`scope`。`subject` 是日志/限流使用的非敏感稳定标识，必须唯一；`scope` 只能是 `read` 或 `control`，其中 `control` 隐含 `read`；`token` 是 CSPRNG 生成的至少 256-bit 随机 Secret 的文本编码，不含业务含义。重复 subject、重复 token、未知字段/scope、空 token、Secret 文件权限过宽或解析失败均拒绝 READY。认证比较必须避免普通字符串早退造成明显时序泄漏，并且任何错误/日志只记录 subject（若已可信解析）或认证失败类别，不记录 Bearer 原文。

**Local 不等于完全无风险**：浏览器本机站点和跨 Origin 请求可能触达 loopback；即使用户只部署在本地，也必须检验 Host/Origin/CORS/Content-Type 限制与控制端点的鉴权/本机信任假设，不允许浏览器简单 GET 或 HTML Form 直接 Stop Recorder。API 不向外暴露 CSRF Session，而 Web 自己执行 CSRF 校验。

## 6.2 ServerConfig、默认预算与配置校验

V1.0 使用独立的 `config/server.yaml`，由 RuntimeHost 与 `config/recorder.yaml`、`config/mcap.yaml` 一并装载；文件内保留顶层 `server:` 命名空间。Server 配置不属于 Recorder Task Config，**运行期间不允许通过 HTTP 未授权热更改**。C++ 直接构造与 YAML 装载必须经过同一 `ServerConfig` 默认值和校验逻辑。下列 YAML 是可实现的**初始建议**，不是已测试的最终发布配置：

```yaml
server:
  listen:
    host: "127.0.0.1"
    port: 8765
    remote_enabled: false
  auth:
    provider: "file"
    secret_file: "/run/secrets/ros2_bag_tokens"
    allow_anonymous: false
  tls:
    mode: "disabled"          # 仅 loopback 合法；remote_enabled=true 时必须 direct 或 trusted_proxy
    cert_file: null
    key_file: null
    trusted_proxy_sources: []
  http:
    io_threads: 2
    max_request_body_bytes: 65536
    max_response_json_bytes: 1048576
    max_outstanding_responses: 128
  control:
    queue_capacity: 32
    admission_timeout_ms: 0
    max_queue_wait_ms: 5000
    domain_response_timeout_ms: 30000
    max_outstanding_responses: 64
  read:
    worker_count: 2
    queue_capacity: 32
    query_response_timeout_ms: 3000
    max_history_scan_entries: 10000
    max_event_scan_bytes: 8388608
  status:
    sample_interval_ms: 1000
    min_resample_interval_ms: 200
    max_sample_age_ms: 5000
    healthy_refresh_interval_ms: 2000
    sample_warning_p95_ms: 100
    max_snapshot_bytes: 786432
  sse:
    max_clients: 64
    max_event_bytes: 1048576
    pending_frames_per_client: 2
    pending_bytes_per_client: 2097152
    global_pending_bytes: 67108864
    global_egress_rate_bytes_per_sec: 8388608
    handshake_deadline_ms: 3000
    stalled_send_ms: 10000
    heartbeat_interval_ms: 15000
    max_send_batch_frames: 2
  shutdown:
    request_grace_ms: 2000
    recorder_grace_ms: 30000
    stream_grace_ms: 2000
    read_grace_ms: 5000
```

**全局 HTTP Budget 的计数范围**：`http.max_outstanding_responses` 包含普通 GET/POST 的等待、执行及待发送响应，以及 SSE **握手阶段**；Stream 握手资源成功转移为长期 Session 后释放短 HTTP Response Slot，随后由 `sse.max_clients`、pending 字节和 Stream 生命周期配额独立核算。`control.max_outstanding_responses` 是其中控制 POST 的子配额，永远不能独立再扩大 HTTP 总上限。Control Worker 正等待的领域 Handle 和已超过 HTTP deadline 的操作由单独生命周期计数，不因 ResponseSink 结束而取消。Admission 原子预留遵守 **Global HTTP → Control/Read/Handshake 类别 → 执行队列/Subscriber Slot** 顺序，失败回滚并释放预留；同时为只读和握手保留合理服务能力，避免 64 个慢控制等待者把所有读接口排空。

**配置验证是启动前的一等契约**：

- host/port 合法、端口冲突/Listener 错误有诊断；`auth.allow_anonymous` 在 V1.0 必须为 false，CredentialProvider/Secret 文件不可读或凭据集合为空时拒绝 READY。`remote_enabled=false` 时拒绝任意非 loopback 地址；`remote_enabled=true` 时必须同时满足 Bearer 鉴权和 `tls.mode=direct|trusted_proxy`，trusted_proxy 模式还必须配置受信来源，不能接受任意 `X-Forwarded-*`。
- 所有容量与限额为有限非负/正整数，区分可以为零的 admission_timeout 和必须正的 Worker/队列/数据大小；避免 bytes × clients、队列数等乘法溢出。
- `max_event_bytes <= max_response_json_bytes` 不足以保证完整状态帧可发送：Event 还需增加 SSE ID、event、data 行与 JSON 字符串转义的字节，必须对**实际 UTF-8 帧**重新校验大小。`status.max_snapshot_bytes=768 KiB` 与 `sse.max_event_bytes=1 MiB` 相差 256 KiB，但**不构成对所有合法输入都足够的保证**；Protocol State、GET Envelope、SSE 完整帧应分别独立计数并按各自上限拒绝，不得截断。参照 §5.4 的实际编码校验规则。
- `healthy_refresh_interval_ms < max_sample_age_ms`，且**Admission 与发送调度都必须校验**从采样到本地交出有效完整帧的最坏可接受预算加抖动余量；若无法满足，则拒绝该 Subscriber 或断开超限连接。`sample_interval_ms < max_sample_age_ms`。安全间距由 OPEN-02 实测决定，不能以两个配置数值相差 3 秒作为已通过证明。
- `sse.max_clients` 与待发送总字节预算、每连接预算、event 帧大小、刷新率之间须有容量可行性校验；静态校验不足时，运行时 Admission 必须拒绝无法维持新鲜度的超额连接。
- `status.max_snapshot_bytes` 应与 Recorder `topics.max_active_topics`、`topics.recent_removed_instances` 及结构化 ErrorRef 上限联动；极端状态超限时返回 STATE_TOO_LARGE，不能静默删 Topic 记录。
- `control.domain_response_timeout_ms` 不是 Recorder 强制 Stop Timeout；`shutdown.recorder_grace_ms` 也不是 Writer 已停止证明。
- ReadDispatcher 的扫描上限要由 Recorder 自己实施或可靠传递为领域请求参数；Server 提供附加预算，不能靠切断文件系统读线程实现硬超时。
- 安全配置修改只能在 Runtime 安全重启后生效，Listener/证书/权限/Handler 不应使用不一致的半新版本。

**不建议把配置默认值散落到不同 Controller。** 使用单个经过校验的 ServerConfig 值对象，C++ 直接构造和 YAML 装载必须具有一致默认值。Server 及受控版本 Trantor 等依赖必须纳入统一的 C++17/CMake/ament 构建与依赖锁定，不允许依赖未锁定的系统 Drogon 安装。

## 6.3 日志、指标、链路关联与故障诊断

Server 与 Recorder 的日志需要同时满足 `FR-DAT-10 / AC-26`：可从 Runtime 进程的日志输出/受控 journald 或容器收集系统持续获取、过滤和检索，具备 UTC 时间、级别、component、request_id/operation_id/recording_id/Topic ID/Epoch、ErrorCode 与 phase；不泄漏 token、认证 Header、绝对敏感路径、Message Payload 和原始异常堆栈到对外 API。

| 来源 | 关键观测项 |
|---|---|
| HTTP | request latency P50/P95/P99、status code/ErrorCode、Body/Response bytes、invalid/auth rate、inflight |
| Control | queue_depth、admitted/rejected、wait_duration、domain_duration、deadline_unknown_total、stale_target_total、active_domain_calls |
| Read | wait_duration、scan_budget_exceeded_total、read_jobs_pending、history/evidence duration、read_lease_age |
| Status | snapshot P50/P95/P99、published revision、last actual sample age、STALE transitions、max_state_bytes |
| SSE | live/handshaking/stalled、pending_bytes/frames、global buffer、coalesces、force_disconnect、subscription_lifetime、send_duration |
| Runtime | start phase/recovery findings、shutdown stage、grace expiration、remaining owners/leases、Context/Executor alive |
| Security | denied/auth failure/rate limit、TLS/proxy misconfiguration、policy violations（消息不含 Secret） |

**UNSAFE_TO_DESTROY 的最小故障诊断：** 即使 HTTP/Recorder 状态查询已经不可用，Runtime 仍应向进程级结构化日志、受控 stderr/journald 或 Supervisor 可采集的诊断通道尽力写出：`runtime_instance_id`、故障发生 UTC 与 monotonic elapsed、`shutdown_stage=UNSAFE_TO_DESTROY`、故障原因（例如 `DRAIN_TIMEOUT / READ_GRACE_EXPIRED / WRITER_BLOCKED`）、`last_completed_domain` 的操作类型/结束阶段与可信结果、`active_domain_calls`、`active_read_jobs`、`active_snapshot_jobs`、各类未归还 `RecorderLease` 数量、Writer/Executor/ROS Context 存活情况、输出根目录锁是否仍持有，以及未完成的响应/订阅计数。每项值若不可安全读取，明确写 `UNKNOWN`，**禁止为了补齐日志而访问已经卡死的 Recorder/Writer 锁或引发新的阻塞**。优先使用预保留的 bounded 诊断对象与非热路径错误通道；最终写日志本身也不是 Writer 已完成的证据。

Supervisor **不必**为 V1.0 引入特定 JSON 日志 SDK 或新监控子系统，但部署必须提供可判定的**失败退出/未正常 STOPPED** 证据，例如非零进程退出状态、受保护的结构化故障记录与 Supervisor 重启日志；这些字段格式可以在部署验收中细化，而不能只依赖普通 HTTP Server 进程已关闭的事实。进程强制终止时日志可能无法全部刷盘，故以最佳努力记录和独立监督事件交叉判断，不伪造完整审计链。

**日志最小关联模型**：

| 请求/事件 | 必须记录的可用身份 | 不允许的推断 |
|---|---|---|
| 控制 POST 的 Validated/Admitted/HandleAcquired/DomainDone/ResponseAttempt | 同一条尝试的 `request_id`、客户端原始 `operation_id`、控制命令、runtime_instance_id、目标 recording_id/generation；Topic 操作还记录已知的 topic_id/epoch | HTTP 完成并不表示 Client 收到结果；operation_id 不是去重/查询 Registry 主键 |
| Topic Evidence GET | 独立的 GET `request_id`、路径中的 `recording_id + operation_id`、领域返回的 `topic_id/epoch`（仅可信可用时）、`evidence_status`/ErrorCode | NOT_FOUND/UNCERTAIN 不证明原 POST 未执行，不根据同名 Topic 认定成功 |
| SSE Stream/Subscriber | 连接 `stream_id`、发布 `runtime_instance_id + state_revision`、Event 序列、订阅关闭原因 | SSE event id 不等于任何 Recorder 操作 ID |
| Runtime Shutdown | shutdown_stage、触发来源、inflight domain/lease、相关 request_id/operation_id（已知才记） | 未完成领域工作时不能记录 STOPPED/COMPLETE |

所有阶段采用结构化键值，并把未获取的 Topic 身份标为缺失而非虚构零。不同 HTTP 尝试可以携带同一 operation_id；日志允许按其聚合，但**不形成服务端持久 Operation Registry**。

request_id 是**一次 HTTP 尝试**，operation_id 是逻辑操作证据身份；不能把二者合并，也不能将 HTTP 在途时间与 Recorder ControlHandle 生命周期等同。Server 日志可记录“response attempted”，不能记录“Client confirmed received”除非存在真实端到端确认（V1.0 不提供）。HTTP timeout UNKNOWN 时，日志应给出阶段 `QUEUED / HANDLE_ACQUIRED / DOMAIN_RUNNING / DOMAIN_DONE` 供排障，但不对外提供通用持久操作查询服务。

Server 指标采集不能反向影响 Recorder 数据路径：日志聚合/Prometheus 等监控框架不是 V1.0 必需品；可通过结构化日志 + 有界内存度量 + 受限部署采样满足最低验收。公开 Protocol Runtime/Status 响应只包含规范字段，不得为了新增内部度量而私自破坏 Protocol Schema。

## 6.4 测试矩阵、注入点与生产准入

测试策略按**同步/身份正确性 → 资源有界性 → 线程生命周期 → SSE 真实互操作 → 性能准入**逐层进行。Server 调度可用可控 FakeRecorder 做确定性交错验证；真实 ROS 2 Humble/Jazzy 和 Drogon v1.9.14 的集成验证不得全部用 Fake 代替。

| 故障注入/测试 | 注入同步点 / 输入 | 必须断言 | 上位来源 |
|---|---|---|---|
| HTTP 路由/版本 | 13 条路径，Method、Content-Type、错误 JSON、超长 Body | 正确 Status/ErrorCode；无 Recorder 副作用 | FR-CTL-01 / AC-11 |
| 单控制串行化 | Start 与 Stop/Topic Add/Remove 并发到来 | 同时最多一个 Server 提交的外部 ControlHandle 未 DONE；真正 mutation 由 Recorder Sequencer 唯一线性化；队列/完成顺序可追踪 | FR-CTL-02 / AC-12 |
| Full Queue | 挂住 Stop ControlHandle，持续送新 POST | 达到控制队列上限后立即 429/effects NONE；内存有界 | NFR-PERF-04 / AC-12 |
| Queue Timeout vs STARTED | 在出队/timeout 线性化点挂起 | 只有真正 QUEUED 移除才返回 `503/QUEUE_TIMEOUT/FAILURE/effects:NONE`；开始后结果未知仍为 `504/UNKNOWN`；以当前 Protocol 定义为准 | FR-CTL-03 / AC-11 |
| HTTP 丢包/取消 | Recorder 已进入 Stop，Client 断开或超时 | UNKNOWN 且不取消 ControlHandle、不提交第二个冲突外部 mutation | FR-CTL-03/08 / AC-11 |
| Timed Stop / Disk Pause | 外部 Stop 时 Recorder 内部事件先发生 | IDLE/PAUSED 与该请求 SUCCESS/FAILURE 不互推 | FR-CTL-03 / AC-11 |
| Stale Target / Epoch | 请求入队后 Runtime/Recording Generation 或同名 Epoch 改变 | 执行前再检查；不作用于新任务 | FR-CTL-02 / AC-11 |
| Topic Evidence | Event durable PREPARED/终态、fsync 失败、冲突、截断、重启，以及 capability 不可用的诊断构建 | 查询调用 Recorder API，可信/UNCERTAIN 正确；capability=false 时 GET 为 503/EVIDENCE_UNAVAILABLE，不可宣布正式 READY/生产准入；Server 不另写 JSONL Parser | FR-CTL-03 / AC-11 |
| GET/History 安全 | 恶意 ID、路径软链接、巨大扫描、并发 Cleanup | 只读/有界，未完整读取不能报 NOT_FOUND/COMPLETE | FR-DAT-09 / AC-23 |
| status() 高频采样 | 64 Topic，Writer/Rolling 持续进行，GET/SSE Clients 增多 | Snapshot 查询次数不随订阅数线性增加；P95 量化、没有 Recorder 热路径退化 | FR-DAT-06 / AC-10 |
| 同 revision 变化 | observed_at 单独更新、Counter/elapsed 增长、STALE↔CURRENT | 仅纯采样元信息允许同 revision 更新；GET/SSE 业务内容一致 | FR-CTL-04/05 / AC-13 |
| 首次 SSE 竞态 | Register 后/首帧前依次发布两个 Revision | 首帧完整 baseline、之后最新完整状态可达；无永久旧状态 | FR-CTL-04/05 / AC-13 |
| SSE 空闲/STALE | Recorder 状态不变且正常采样，再注入长采样失败 | 同 revision 完整健康刷新；随后只用可信旧快照发 STALE 或断开，恢复新 CURRENT | FR-CTL-04/05 / AC-13 |
| SSE Parser 兼容 | 注释心跳、空 data、CR/LF/CRLF、Unicode、断流 | 实际 Client httpx-sse 正确解析或隔离/修复，不凭空造假事件 | FR-CTL-06 / AC-13/14 |
| SSE 慢消费者 | Client 不读、半开、速率骤降，持续改变状态 | App/Trantor/Socket 资源增长有明确上限、能断连/释放；不能只看 send(true) | NFR-PERF-04 / AC-13 |
| SSE 容量数学 | 64 Clients、1 MiB Snapshot、2 s Refresh、8 MiB/s Budget | 用实际编码字节/全局 Token Budget 验证可维持 N；容量不足拒绝或断开，不能延迟到超新鲜度时限；三层资源增长受测量约束 | OPEN-02 / AC-13 |
| Runtime Startup | 锁冲突、Recorder/Writer 错误、Recovery partial、Port/TLS 失败 | 安全时可诊断继续；危险时不开放控制；资源逆序释放 | NFR-REL-01/04 / AC-17 |
| Shutdown | Stop ControlHandle 长期未 DONE、Reader 永久阻塞且仍持有 Lease、SSE 尚有 Pending、重复 SIGTERM/SIGINT | 控制/Reader 超时只结束网络等待，未完成 Reader/Writer 不 detach、不提前释放 Recorder/Executor/锁，未终结不报 STOPPED；UNSAFE_TO_DESTROY 具有进程级诊断 | NFR-REL-04/05 / AC-17 |
| Auth/Remote | 缺失/错误 Bearer、read Token 调 control、host 改为 0.0.0.0 无 TLS、恶意代理 Header、跨 Origin/Web CSRF | 401/403/配置拒绝正确；Token 不泄漏；direct/trusted-proxy TLS 与 Scope 边界经 AC-18 验证后才允许远程发布 | NFR-SEC-01～03 / AC-18 |
| 目标平台 | Humble/Jazzy × AMD64/ARM64，原生 Server 构建、启动、关闭 | Drogon/Trantor 依赖可复现，MCAP/Recorder 数据面未受网络干扰 | NFR-COMP-01～04 / AC-19/21 |
| 日志/诊断 | Server 503、原生 EVENT_COMMIT_FAILED、SSE forced close、SIGTERM | 按 operation_id/request_id/phase 检索，Sensitive 字段脱敏 | FR-DAT-10 / AC-26 |
| HTTP 额度分层 | 全局短响应槽 128、控制子配额 64、Read/SSE 握手抢占，超时后领域 Handle 仍可能未 DONE | 先全局后类别再队列地原子 Reserve/回滚；握手转 Stream 后只计 SSE 长连接；领域请求继续运行但不占已结束 ResponseSink；无槽位泄漏或读饿死 | NFR-PERF-04 / AC-12/13 |
| Readiness 状态映射 | STARTING/READY/DEGRADED/QUIESCING/DRAINING/UNSAFE_TO_DESTROY | Runtime 只输出 Protocol 四个 readiness 枚举；QUIESCING/DRAINING→STOPPING，UNSAFE/STOPPED 不提供正常 HTTP；未交付 MUST 不误报正式 READY | FR-CTL-01 / AC-11 |
| SSE 完整帧字节限制 | 768 KiB 状态 Data、额外 JSON Envelope / SSE id 和 Unicode Escape、帧尺寸越界 | 按编码后全部 UTF-8 字节校验，握手前 503/STATE_TOO_LARGE 或已建 Stream 关闭，不截断业务 State | FR-CTL-04 / AC-13 |
| 日志双身份 | Client 重用 operation_id 的多次 HTTP POST、Topic Evidence GET、有 topic_id/epoch 的结果 | 每次独立 request_id + 原 operation_id，Topic Evidence 记可信身份/状态，无法推断通用 Registry；日志不泄密 | FR-DAT-10 / AC-26 |
| Read Lease Timeout / Shutdown | History/Evidence Reader 在 Recorder API 内永久挂起，read_grace 到期，稍后 Reader 自然返回 | 超时先终结 Sink 而非 Lease，UNSAFE_TO_DESTROY 不释放 Recorder/根目录锁；Reader 真实返回且所有屏障满足后方可正常收尾；否则走进程级监管 | NFR-REL-04/05 / AC-17 |
| SSE 真实编码膨胀 | 极端 ROS Topic 名称、引号/控制字符与 Unicode、GET/SSE Envelope、完整 SSE Frame | 分别实测 State JSON、GET JSON、SSE 业务帧全部字节；256 KiB 不能充当最坏情况证明，超限明确拒绝不截断 | FR-CTL-04 / AC-13 |

**必测时序**：Control Worker 刚从 FIFO 取出、目标身份预检完成但 Recorder Sequencer 尚未线性化、Stop ControlHandle 等待 Writer、StatePublisher 发布成功但 SSE 尚未取得 Stream、Session close 与 send 同时发生、最后 RecorderLease 归零前插入可控制 Barrier。不能只依赖长时间随机压力测试碰运气复现 UAF/死锁。

**Drogon SSE 生产准入**：技术选择并不代表已通过。应使用真实慢读/断连/代理/长连接压力及 Client 实际 httpx-sse Parser 验证协议完整性、内存驻留高水位、回收时限、相互阻塞与 Runtime Shutdown。框架不能提供可验证资源上界且局部适配无效时，必须重新评审选择，不能放宽“业务正确性必须成立”的要求。

## 6.5 交付边界、待量化事项与设计评审

Server 实施必须完整交付以下组成；缺少任一项时只能作为受限开发/诊断构建，不能宣称满足 V1.0 Server：

1. **RuntimeHost**：统一 Owner 图、启动/Recovery 分类、受控信号与关闭屏障；所有正式入口遵守同一生命周期顺序。
2. **Drogon 接入**：受控 v1.9.14/Trantor 版本、13 条路由、ServerConfig 与认证/限流入口。
3. **ControlDispatcher**：唯一 Worker、有界 Admission、排队/领域完成/网络 deadline 区分、生命周期安全 ResponseSink。
4. **ReadDispatcher**：Graph/History/Topic Evidence 受控读取，Recorder Domain-owned 解析，读写任务引用安全。
5. **SnapshotSampler/StatePublisher**：1 Hz 初始节拍、单一 revision、不可变 PublishedState、GET/SSE 同源、STALE/健康全量刷新。
6. **SseHub**：Drogon AsyncStream 的帧编码、无空窗基线、慢连接合并/断连/资源回收，完成三层背压准入。
7. **安全与观察**：Bearer CredentialProvider、read/control Scope、TLS/direct-or-trusted-proxy、Web 安全边界、不泄密、日志/指标可追踪，并完成 AC-18。
8. **测试产物**：确定性竞态、真实 HTTP/SSE 互操作、Drogon 内部缓冲压力、生命周期故障注入与多平台编译验证。

当前需求仍有如下发布前量化事项：

| OPEN | 当前状态 | Server 交付责任 |
|---|---|---|
| OPEN-01 | CPU 参考硬件/CPU 计量边界待冻结 | 状态采样/网络 CPU 占用纳入总 CPU 口径；参考机和阈值由验收规范决定 |
| OPEN-02 | HTTP 延迟、SSE 可见时延、连接上限/容量待量化 | 验证 1 Hz / 5 s / 2 s 候选状态周期和 SSE 预算，正式冻结量化 Profile 与慢客户端测试 |

**开发阶段维护：** 本文设计可以直接优化，遇到原生 Recorder Guard/Topic Evidence、HTTP Mapping、状态发布或 SSE 资源边界不合适时，应同步修改依赖模块的设计与验证。所有运行指标和生产准入结论必须来自真实 Drogon/Trantor 集成测试，不以设计完成代替实现证据。
