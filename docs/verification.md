# ros2_bag V1.0 验证与准入实施规范

| 属性 | 内容 |
|---|---|
| 版本 | v0.1.2 |
| 状态 | **DRAFT — 验证规格仍可调整，实际测试未执行** |
| 依据 | requirements.md 的 AC-01～27，以及本文件 V-* 场景；recorder/protocol/server/client 当前开发稿 |
| 适用 | Recorder、Runtime/Server、Protocol、Python Client、CLI/TUI/Web、MCAP 分析端 |
| 非目标 | 不把历史报告的 PASS 直接继承到新源码，不把单平台 Mock 结果充当四平台发布证据 |

# 1. 验证规则、平台和证据格式

## 1.1 证据与判定分离

**测试结果与 Gate 决策分层管理。** 单个测试记录 `Test Status = PASS / FAIL / BLOCKED / NOT_RUN`，并绑定源码 SHA、构建制品、配置和环境依赖闭包。Gate 层存在两类不同判定：开发/集成准入使用 `Admission = ALLOWED / ALLOWED_WITH_CONDITIONS / BLOCKED`，正式发布裁决使用 `Decision = APPROVED / PENDING / REJECTED`；**G0～G2 的规范性退出字段是 Admission，G3 的规范性退出字段是 Decision**，两组枚举不得互换或用同名字符串代替。**文档评审完成、G0 允许继续开发、某项 Sanitizer 无告警，都不构成产品发布 PASS。** Humble Fast DDS/TSan 风险直接记录在本文 §3.2；旧的本地测试证据不能替代当前代码和平台的新测试。

统一用例记录（JSON/YAML 或 Markdown 同等字段）：

```yaml
test_id: "EXAMPLE-TEST-ID"
requirements: ["AC-07"]
source_commit: "<git sha>"
build_artifact_sha256: "<sha256>"
platform: "ubuntu22-humble-amd64"
cpu_model: "<hardware or VM profile>"
ros_distro: "humble"
rmw: "rmw_fastrtps_cpp"
dependency_lock_sha256: "<sha256>"
config_sha256: "<sha256>"
injector_version: "<commit>"
seed: 20261025
start_utc: "<UTC RFC3339>"
duration_sec: 600
expected: "<specific invariant / threshold>"
observed: "<metrics and evidence refs>"
status: "NOT_RUN"
evidence_files: []
reviewer: "PENDING"
notes: ""
```

任何“结果未取得”“依赖测试未执行”“测试环境无目标硬件”必须保留 BLOCKED/NOT_RUN，不填 0、跳过或自动 PASS。若发生需求或实现目标冲突，先记录评审裁定，不在测试脚本中降低 MUST 阈值。对于计数测试保留原始消息发布序号、时间戳、源 DDS 配置和原始 MCAP，而不只保存终端截图。

## 1.2 平台、分析矩阵与构建级别

四个原生 Runtime 平台：Ubuntu 22.04/Humble × AMD64/ARM64；Ubuntu 24.04/Jazzy × AMD64/ARM64。每个平台至少原生编译、链接、启动、Topic Add/Stop、独立 Segment 读回和 shutdown；容器镜像跨架构构建**不能代替原生硬件的运行验收**。Python 3.10/3.12 与系统 Python/ROS2 ABI 解耦，但要验证其 wheel 在各目标平台的实际安装和 HTTP/SSE。

MCAP 的录制端与分析端两维 2×2（H→H、H→J、J→H、J→J）独立于 CPU 架构的运行矩阵；所有涉及自定义消息的分析环境不安装原自定义消息包。选用的 PlotJuggler/MCAP Reader、rosbag2/Storage Plugin/LZ4、ROS 2、Fast DDS、Trantor/Drogon/httpx/httpx-sse 版本和 SHA 入证据。一个 Segment 能被 ros2 bag info 列出不等于压缩 Chunk/嵌套 Schema 可独立解析。

测试执行级别：
- L0：Codec/Pool/Queue/State 模型单测及可控 Fake Clock/Mock Transport。
- L1：目标发行版 SDK/ROS 基础镜像构建与单平台集成。
- L2：Humble/Jazzy × AMD64/ARM64 原生或受控架构 runner 的真实编译/运行；跨网络 DDS 实际消息。
- L3：恢复、Sanitizer、长稳态负载、Slow IO、真实 HTTP/SSE 压力、发布候选验证。
- L4：候选制品重装、依赖/签名摘要、回归与 G3 Release Decision。

这些 L 级是新的验证组织，不意味着必须保留旧 UDS/Session/Docker 目录或旧原始命令原封不动。


## 1.3 标准性能 Profile 的可重复测量口径

**标准负载（MUST）**：10 Topics × 每 Topic 1000 messages/s × 每消息 1024 Bytes Serialized Payload × 600 seconds；连续发布序号必须由真正 Publisher 在 CDR 内生成，初始 Seed、Topic 顺序、QoS/History/Depth、CPU Affinity、Compression、Storage、System Governor、ROS Middleware、网络模式均记录于 Test Config。需在采集前确定发布端平均及每 Topic 频率允许偏差区间；如果 Publisher 未实际达到约定标准负载，则该端到端 AC-16 **INVALID/BLOCKED 而非 PASS**，该次数据可另作分层诊断。非基准的 Camera/LiDAR 大消息正确性按 AC-27 独立验证，不能以大消息降吞吐修改标准 Profile。

**逐 Topic 去重与损失**：对每个 Topic i 记 `P_i`（Publisher 实际成功发布且有序号证据的 distinct sequences）、`R_i`（所有 MCAP Segment 中真实可读、CDR 类型正确且 Sequence 属于本 Topic Publisher 的 distinct sequences，跨 Rolling Snapshot 去重）。通过条件 `(P_i - R_i) / P_i < 0.01`，并在每个 Topic 中分别满足；对于 `P_i=0` 直接判该次 Profile 无效，而非损失率零。计数差异由 Publisher 到 DDS、DDS 到 GenericSubscription、Recorder Admission、Writer、MCAP 可读性分层归因；`written_live`、`replayed_to_new_segment` 不得相加后充当无重复 Publisher 序号。Reader 按所有 Segment 遍历，记录打不开/Schema 不足的失败，不能只用 `ros2 bag info` 的条目总数。

**CPU/RSS 指标**：OPEN-01 关闭前，初始候选“CPU 单核归一化 ≤30%”不是正式发布阈值。测试负责人必须先在 Profile 中冻结 CPU 型号/核数/频率、省电策略、Recorder 与 Server 同进程是否计入、是否包含 DDS 线程、Procfs/cgroup 采样来源、采样间隔、Aggregate P95/Mean/Peak、100% 表示一个核还是全机器。RSS 记录曲线/Peak、Writer/Pool/Queue/Snapshot/Server SSE 内存，最少每秒采样；连续 5 min RSS 趋势 >1 MiB/min 是**泄漏调查触发线**，而不是直接判定内存泄漏或自动 FAIL。长稳态 Topic 压力的 3000 次增删同样需要逐秒 RSS/FD/线程/TopicInfo/MCAP Channel 的差分。

**Network/SSE OPEN-02 量化**：固定 HTTP Post/GET 的接收/应答时刻、最大挂起请求、SSE Server Recorder Sample 时间→Client 完整状态到达时间（不能将 send()==true 当 Client ACK），定义 P50/P95/P99、sample_age、queue wait、Stream backlog；在 1/8/32/64 Client、最小/典型/最大合法完整状态帧、50/100/500ms 网络延迟和慢读/完全不读情况下记录资源曲线。64 Client ×1MiB ÷2s≈32MiB/s 超过候选 8MiB/s，全局 admission **必须**拒绝不可能满足健康新鲜度的连接或安全断开。新鲜度 Budget、Frame 字节数、连接上限及具体合格 SLA 需 OPEN-02 正式签审，不得将示例默认参数冒充发布已批准指标。

**四平台公平性**：同一个 Source SHA、相同业务 Profile 的可比配置，在每个原生平台独立测量并保存发布序号源、MCAP Reader 数据、perf/stat/cgroup 原始样本、所有错误计数和依赖锁。Jazzy/ARM64 如果尚不可运行时保留 BLOCKED，不能拿 AMD64 结果填表。

# 2. Recorder 算法、并发与数据完整性

## 2.1 Pool、Queue 与 ProducerToken（V-REC-POOL、V-REC-QUEUE）

| ID | 前置条件/注入 | 精确断言 | 关联 |
|---|---|---|---|
| V-REC-POOL-01 | 固定容量饱和、Fallback 成功、1024 上限、注册 insert 失败、分配失败 | 绝不暴露未登记 Handle；失败回滚完整；容量不足无 abort | AC-07 |
| V-REC-POOL-02 | V1.0 global_mutex：8～16 Producer/Consumer、跨线程归还、Stop 与 Fallback 并发 | `capacity=in_use+global_free`，静止时 in_use/fallback/registry=0、global_free=capacity，所有释放恰好一次，无 TLS Owner | AC-07/17 |
| V-REC-POOL-03 | 人工注入 NodeIndex 越界、DoubleRelease、Stopped 后 release（独立子进程） | 精确 PoolFatal/非零 abort；正常分配不足不 abort；不能只靠 NDEBUG assert | AC-07/17 |
| V-REC-POOL-04 | global_mutex 正式基线按标准负载测锁等待/CPU/RSS/P99；另可运行 TLS 实验 A/B | global_mutex 必须先满足正确性和已确定性能门槛；TLS 只形成优化数据，不是 G3 必测功能，除非以后重新纳入正式配置 | AC-07/16 |
| V-REC-QUEUE-01 | ≥10 Producer、每次预算不足、Pool失败、入队拒绝、Writer失败、Stop | 任何时刻消息数/字节不超预算；最终 used_messages/bytes=0，Reserve/Release 守恒 | AC-07 |
| V-REC-QUEUE-02 | 危险旧 ProducerToken 持有期间 Stop→Start 复用 Executor 线程；人为延迟 callback | 不碰已析构旧队列，不写新 Task；正常切换 dropped_stale_generation=0 | AC-03/17 |
| V-REC-QUEUE-03 | 对已有 Handle 所有权移交注入 Writer/Queue 完成失败 | 原 Payload 与 MessageHandle 在最后持有者退出前有效，无 UAF/双释放/伪写入 | AC-07 |

每个注入点至少记录相同随机 Seed 下的可复现消息数/字节序列，ASan 与不含 DDS 的项目自有 TSan 用例都要执行。不得以单纯睡眠引发“偶发通过”，应有 Barrier（进入 acquire 后/登记后/入队前/Writer 出队后/Pool BeginStop 前）。

## 2.2 频率、Gap 与 Queue/Publisher 来源（V-REC-FREQ）

时钟采用可注入 steady clock，以精确逐消息 timestamp 计算参考频率，并用 10ms 有界 bucket 对比。配置默认 W=1s/bucket=10ms；重点注入 t-W、恰好桶边界、W 前后的溢出、时间靠近零、0 Publisher、Pause/Resume、Epoch 重加、低频/稀疏消息、暴发与稳定 1kHz。

| ID | 场景 | 通过判据 |
|---|---|---|
| V-REC-FREQ-01 | 新 Topic、未收到任何消息及第一完整窗口前 | actual_hz=null、gap=null；provisional 不参与 gap；不伪造 0 Hz |
| V-REC-FREQ-02 | 10ms 桶环形复用、各种边界与大于 W 的空窗 | 不累加陈旧桶，不越界，Ring 分配与时间复杂度有界 |
| V-REC-FREQ-03 | 精确 timestamps 参考 vs 稳态桶值 | 频率精度目标：平均相对误差 <2%、P95 <5%，保留 Max；接近零时用预先冻结的绝对误差 |
| V-REC-FREQ-04 | 两个独立完整 W 周期低于 expected×0.5 | 第一个完整低周期尚不报告 gap=true，第二个才 true；不是两次 10ms status() |
| V-REC-FREQ-05 | 恢复到阈值、USER/System Pause、Publisher UNKNOWN、Remove/Add 新 Epoch | 稳定周期恢复 false；无法判定时 null；不跨 Epoch 继承旧判定 |

**已观察到的源码偏差**：当前 `src/topic_manager.cpp` 以 `current_slot` 是否改变推进 consecutive_low，可能把两个相隔 10ms 的高度重叠采样当作两个完整周期。需要修正并由上述 FakeClock 回归；不能把 2%/5% 直接称为当前 PASS。

## 2.3 Topic、模式、Writer、磁盘与恢复

| ID | 注入/边界 | 通过判据 | 关联 |
|---|---|---|---|
| V-REC-TOPIC-01 | Graph 未发现、可见零发布者、多类型、QoS Offered 不兼容/后加入 | 发现与可录性分离，QoS EXPLICIT 不被 AUTO 覆写 | AC-01/02/22 |
| V-REC-TOPIC-02 | 已注册 Channel 但 subscription 失败，PREPARED 终态 fsync 故障 | 不开放 ingress、终态不可信时 UNCERTAIN，Event 身份可信 | AC-03/09/11 |
| V-REC-TOPIC-03 | 3000 次 Add/Remove，同名再加与跨 Stop | Epoch 独立；按秒 RSS/FD/线程/Schema/Channel/TopicEvent 统计，无无界资源增长 | AC-03/16 |
| V-REC-TOPIC-04 | Task 内先 Add `/x` 类型/Schema A→Remove→同名 Add 类型 B；另测同类型名但完整 Schema SHA-256 改变；对照同签名 A 的合法重加 | B/Schema 变化在 TOPIC_ADD_PREPARED、Writer register、Subscription create 前拒绝；同签名重加产生新 Epoch 且数据不串代 | AC-03/06 |
| V-REC-MODE-01 | Normal、Timed 到期、Paused 仍计时、Stop 重入 | 不错误延长 Timed，Stop 完成包含 Writer/Manifest | AC-04 |
| V-REC-MODE-02 | Rolling 空流、全部 Topic 删除、Pause 无消息、空段打开后 Stop | 不持续创建无界空段，首消息合法建段，Manifest 与真实文件匹配 | AC-04/05 |
| V-REC-MODE-03 | Snapshot 达单 Topic/全局上限、Rolling 恰好边界、Writer Replay 失败 | 双预算/驱逐一致；Replay 计数独立，失败不伪报 Live 写入 | AC-05/07/10 |
| V-REC-RETENTION-01 | COMPLETE Segment 后系统 UTC 前跳 24h/回跳 24h；进程重启分别发生在 COMPLETE 未授权与 durable DELETING 两种状态 | 墙钟变化不改变删除授权；同进程只按 steady age；重启后 COMPLETE 重新计满完整 retention，DELETING 直接幂等续删 | AC-05/09 |
| V-REC-HISTORY-01 | Rolling 产生超过 `history.max_segment_records_per_task` 的 Segment，大量最老 Segment 已 REMOVED；另造无可压缩 REMOVED 前缀的满容量状态 | 仅 durable REMOVED 连续前缀被汇总；segment_limit/offset 分页有界且 summary 计数守恒；无法安全压缩时不创建下一 Segment并进入明确容量失败 | AC-05/23 |
| V-REC-DISK-01 | 首次 statvfs 失败、连续失败、样本超过有效年龄、阻塞、CRITICAL、恢复、USER Pause | 起始 UNKNOWN 不允许 Start，系统恢复不解除 USER Pause；Recorder 热锁不等待 statvfs | AC-08 |
| V-REC-CLEAN-01 | Cleanup 删除接口注入 35s、队列满、CleanupWorker 2s 退避唤醒与 DELETING 重启 | 不阻塞 Coordinator/ROS Executor、不新增线程/Timer；durable pending 不丢，Worker 退避后安全重新 claim，不重复删除，不误删 INCOMPLETE | AC-05/09/10 |
| V-REC-RECOVERY-01 | ≥10 次约 30s 录制后 SIGKILL/重启/读回 | 保存发布/接收/录入序号、丢失窗口、Manifest/Event/Recovery，未知不宣称 COMPLETE | AC-09/16/17 |
| V-REC-STOP-01 | Writer、Cleanup remove_all 或 ResourceMonitor statvfs 永久阻塞；Runtime 重复信号 | request_shutdown O(1) 返回 Handle；Runtime deadline 仍运行并进入 UNSAFE_TO_DESTROY；不 detach、不进入 Recorder destructor、不假 STOPPED/COMPLETE | AC-17 |
| V-REC-TIMER-01 | Humble/Jazzy 单 Executor Worker：Rolling Timer 命中时人为阻塞 Writer.Close；并行 Topic 10×1000Hz；Timed Stop/外部 Stop 竞争同一 Task | Timer 只发布 O(1) 事件，ROS callback 不同步等待 Writer/Executor；内部 Sequencer 有界去重、Stop 唯一所有者，不出现活锁、UAF、双 Close、错误重开 ingress | AC-03/04/07/16/17 |
| V-REC-TIMER-02 | 1000 个空 Rolling 周期、1000 个 Pause 周期、首次 Live 消息在长期空闲后、Replay-only Segment | 最多一个空物理 Writer，`segments[]` 只包含真实打开文件且 segment_id/path 从 0 同号、严格连续；首次 Live 合法写入且归属正确，重放不触发无限空段 | AC-04/05/10 |
| V-REC-SEQ-01 | 外部 Stop 已排队但尚未 Sequencer claim，同时内部 TimedStop/WriterFailure/Shutdown 注入 | RecorderControlSequencer 给出唯一合法顺序；系统安全事件不因外部队列满丢失；Writer Close/Finalize 至多一个所有者 | AC-03/04/17 |
| V-REC-EXEC-01 | ExecutorCall 分别在 PENDING、CAS STARTED 后、Subscription 已创建后制造等待超时；随后 Stop/Shutdown | 仅 PENDING 可取消为未执行；STARTED 必须继续到 DONE并保留 Lease；Stop 不释放 Topic/Queue/Node 造成迟到 UAF | AC-03/17 |
| V-REC-EVENT-01 | 两个外部 Topic 操作误用相同 UUID；随后 Stop 自动移除多个 Topic | 外部复用产生 CONFLICT/UNCERTAIN；内部移除 origin=INTERNAL、operation_id=null，不污染 Topic Evidence | AC-03/11/14 |
| V-REC-START-01 | 任务目录预先存在、两个 Start 同时请求、首个 statvfs 永久阻塞 | 目录不被复用/覆盖；最多一个 Start 取得启动权；Start 有界返回 DISK_STATE_UNKNOWN，Recorder Control/ROS Executor 不执行 statvfs | AC-04/08/17 |
| V-REC-CONFIG-01 | Runtime 配置为 LZ4/rolling 默认 60/600；启动后篡改全局 YAML；依次 Start NORMAL/TIMED/ROLLING | 当前 Runtime 不受文件变化影响；StartCommand mode 正确覆盖任务模式，ROLLING 缺省取 60/600；Manifest/Status/StartResult effective_config 完全一致；重启后才加载新 RuntimeConfig | AC-04/05/25 |
| V-REC-PAUSE-01 | `auto_resume=false`：DISK_CRITICAL 后用户 Pause，磁盘安全稳定；再直接 Resume。另测 `auto_resume=true` 的纯系统暂停 | 危险解除后 false 路径为 USER+DISK_RECOVERY_PENDING；一次 Resume 在新鲜安全复核后清 USER/PENDING 并恢复；true 路径仅在无 USER 时自动恢复 | AC-04/08/13 |
| V-REC-CLEAN-02 | Task A 仍有持久 DELETING pending 时 Stop A/Start B；旧项失败+Worker 2s Retry+Shutdown/重启 | 旧任务目录不被新任务字段替换，Claim 按 Task/Segment/Manifest 身份去重，不跨根目录，Writer/SSE 不等待删除，Shutdown 不释放仍在用的 Lease | AC-05/09/17 |




## 2.4 MCAP Schema 闭包测试夹具（V-MCAP）

建议仓库测试资源实现以下类型，具体 .msg 字段名可在测试 fixture 审核时固定并随 Release SHA 锁定：

```text
sample_a/msg/Leaf.msg
  uint32 counter
  string label
sample_b/msg/Inner.msg
  std_msgs/Header header
  sample_a/Leaf leaf
  float64[3] vector
sample_c/msg/Outer.msg
  sample_b/Inner[] items
  sample_a/Leaf[2] fixed
  uint32 sequence
  string<=32 note
```

输入序号必须递增、特定叶子标签包含 UTF-8/引号/空字符串、覆盖空数组和多个嵌套元素。Field 级 Golden JSON/CSV 与原始发布消息保持一一对应，所有段都带 Schema/Channel/序列化 CDR。分析端测试前确认 sample_a/b/c 的 ROS 包、Python module、typesupport 共享库均**不在环境**，避免偶然读取本地定义通过。至少覆盖 Humble→Humble、Humble→Jazzy、Jazzy→Humble、Jazzy→Jazzy；PlotJuggler/MCAP parser 各版本固定。任何路径只显示 TopicMetadata 但不能还原嵌套字段均 FAIL。

| ID | 场景 | 判据 |
|---|---|---|
| V-MCAP-01 | 全闭包与独立分析 | 所有嵌套字段值/数据类型、序号完全一致 |
| V-MCAP-02 | 缺 Leaf/Inner 定义、不一致 type hash、Humble Resolver 返回 UNKNOWN/空定义 | Add/Segment 注册前安全拒绝并诊断，不开放 Channel ingress；Humble MCAP Plugin 不得凭 create_topic 成功绕过 preflight |
| V-MCAP-03 | Rolling 新段/新 Topic、Schema 注册/Writer close 失败 | 每个独立 Segment 都可单独读回或显式 INCOMPLETE；不假 COMPLETE |
| V-MCAP-03A | Humble Topic Add 成功后、Rolling 新段前替换/移除安装的定义资源（测试沙箱） | 新段 preflight 与冻结 hash 不一致即拒绝轮转/进入安全失败；不生成空 Schema Segment；正式部署定义资源为只读 |
| V-MCAP-04 | Camera/LiDAR 大 Payload 与 Budget 拒绝 | 成功写入完全正确；未接纳丢弃与 Writer 错误独立计数，AC-27 |
| V-MCAP-05 | 同发行版 ros2 bag play | 真实 Subscriber 获取字段/时间戳，不能只看 ros2 bag info |


## 2.4.1 任务守卫、时间戳与新鲜度测试

| ID | 注入场景 | 判定 |
|---|---|---|
| V-REC-GUARD-01 | Server 最后一次目标检查后旧任务停止、新任务开始，旧 Stop 延迟进入 | Recorder native 拒绝 STALE_RECORDING；不停止新任务；无本次副作用 |
| V-REC-GUARD-02 | 两个 Start 竞争，同时存在 Timed Stop | 至多一个 Start 获取领域启动权；非法/已存在 recording_id 无副作用拒绝；不能覆盖原 Task/目录 |
| V-REC-GUARD-03 | Topic Remove 的旧 Epoch 与同名重新加入的新 Epoch 交错 | 旧 Remove 不可销毁新订阅 |
| V-REC-TIME-01 | ROS use_sim_time 暂停/回跳，业务 Header.stamp 不等于接收时刻 | RECEIVE 仍取 Unix 系统时间；MCAP log_time=publish_time；Timed/Rolling 用 steady clock |
| V-REC-TIME-02 | Timed deadline 前人为阻塞/延迟 ROS Timer/Executor 5 s，Subscription Producer 仍持续执行；另让 Sequencer 忙于其他 Native 等待 | 每个 `try_enter(now)` 自己检查不可变 steady deadline；deadline 后新 Producer 均拒绝，不依赖 Timer callback 已运行；deadline 前已准入消息可 Drain，最终 Stop 可延迟 | AC-04/17 |
| V-REC-PAUSE-02 | USER Pause 与 DISK_CRITICAL 以两种顺序交错；磁盘安全稳定后再 Resume | 原因集合不互相覆盖；USER 不被系统恢复清除；auto_resume=false 将解除的磁盘原因转成 DISK_RECOVERY_PENDING；一次 Resume 清 USER/PENDING 后才可重开 ingress | AC-04/08 |
| V-REC-PAUSE-03 | 仅 DISK_UNKNOWN 导致 PAUSED：危险仍活动时直接 Resume；随后安全稳定进入 PENDING 再直接 Resume；已有 USER 时重复 Pause | 活动危险时 `DISK_UNSAFE/effects:NONE`；安全 pending 后一次 Resume SUCCESS 并恢复；重复 USER Pause 为 ALREADY_PAUSED/effects:NONE | AC-04/08 |
| V-STATE-01 | 同 revision 的完整 SSE 状态先后具有较新/较旧 observed_at，或偷偷改变业务值 | 有效新采样刷新 observed_at，不回退时间；业务值变动必须增加 revision |
| V-PROTO-EXAMPLE-01 | 解析 `protocol.md` 中全部完整 State 示例，按 `(runtime_instance_id,state_revision)` 聚合规范性业务字段 | 同一身份+revision 不得出现不同业务状态；示例中的 revision 演进合法，避免文档自身违反状态不变量 | AC-13 |

这些测试使用确定性 Barrier/Clock 注入，不以随机延迟或仅 Server 层 DTO 校验充当领域原子性证据。

## 2.5 持久化故障注入的不可替代窗口

| 同步注入点 | 预期保守行为 | 留存证据 |
|---|---|---|
| Writer Close 返回后、MCAP 数据文件 fsync 失败 | Segment 不得 COMPLETE；Reader 暂时可读也不能替代持久化证明 | MCAP SHA、fsync fault log、Manifest/Recovery |
| MCAP fsync 成功、metadata fsync 失败 | Segment 不得 COMPLETE；保留数据和失败诊断 | MCAP/metadata SHA、fault log |
| 文件 fsync 均成功、segment directory fsync 失败 | 新文件名/metadata 目录项 durable 未证实；不得提交 COMPLETE | 目录快照、fault log |
| 新 Segment/task 目录项创建后，task/output_root directory fsync 失败 | 不得把新目录的存在当作 durable；Start/Finalize 走失败/INCOMPLETE 路径 | parent-dir fault log、Recovery |
| Manifest 临时文件写入后、fsync 前崩溃 | 旧 Manifest 仍可被 Recovery 安全识别；临时文件不是完成证据 | 文件系统目录、fsync fault log |
| Manifest fsync 已成功、rename 前崩溃 | 同上；临时文件不可作为已提交状态 | 完整旧文件/Temp SHA |
| rename 后、目录 fsync 前失败 | 新旧版本究竟 durable 未知；不能向调用者报 COMPLETE | old/new 版本校验和与诊断 |
| Topic ADD PREPARED fsync 前失败 | 副作用必须在安全序列中禁止或保留不可信证据 | Event tail/operation_id |
| Topic PREPARED 已 durable，Writer 注册后但 ADDED 未提交 | UNCERTAIN；Channel 可存在但 Client 不可误认请求成功 | Event+MCAP Channel+Reader |
| TOPIC_ADDED 行可读但终态 fsync 失败 | 进程内已知失败时 Event Evidence 不可报告 CONFIRMED_SUCCESS | Event/Coordinator fsync injection result |
| Writer Close、MCAP/metadata/目录同步均成功，但 Manifest COMPLETE 提交失败 | 仍不能宣称 Recording Task COMPLETE；数据可 durable/readable 也不替代任务终态 | Writer log/data sync log/manifest/recovery |
| Segment durable DELETING、删除前 SIGKILL | Recovery 保留删除意图，安全重试完整 Segment 删除 | Segment/Manifest SHA |
| 删除系统调用已完成，REMOVED durable 前 SIGKILL | 再次恢复是幂等删除，不能把不存在目标误当损坏/删错文件 | 容器/目录差分 |
| ERROR/Stop 时强制退出，重新启动 | 任何未完成段保持 INCOMPLETE 和诊断，不凭局部 MCAP Header 做完整性推断 | Recovery Report/Reader field test |

测试注入必须用可控 Barrier/故障文件系统/系统调用 Fault Wrapper 表达精确窗口，不能仅用随机 kill 在 600s 压测中碰运气。Writer/Manifest/Event/fsync 的逻辑证据按源代码状态判定，不能因为某次重启后文件“碰巧存在”就推断旧请求已得到成功回执。

# 3. HTTP、SSE、Client 与应用验收

## 3.1 请求证据、状态身份和 Deadline

| ID | 注入 | 判据 | 新 AC |
|---|---|---|---|
| V-SRV-01 | 控制 32 排队满、Control Worker Native 阻塞 | 429 受理前拒绝；503 已排队但确定未开始；504 已可能进入且结果未知 | AC-11/12 |
| V-SRV-02 | Stop HTTP 超时同时 Timed Stop→IDLE 或 Disk Pause→PAUSED | 两种终态均不归因为该请求，UNKNOWN 保留 | AC-11 |
| V-SRV-HANDLE-01 | Recorder `stop()` 立即返回未 DONE ControlHandle；HTTP 30s deadline 先到，领域在 35s DONE；其间第二个外部 Resume/Start 已排队 | 30s 可发 504 UNKNOWN 并释放 ResponseSink，但唯一外部 mutation 槽继续被首 Handle 占用；第二请求不进入 Recorder；35s DONE 后才推进下一项，无 UAF/双完成 | AC-11/12/17 |
| V-SRV-MAP-01 | 参数化遍历 RecorderErrorCode 与 Server admission/timeout 原因 | 每项恰好匹配 Protocol §4.2.1 的 HTTP/ErrorCode/outcome/effects；新增未映射枚举使契约测试失败；Recorder DRAIN_TIMEOUT=明确 FAILURE，只有 Handle 未 DONE 才 504 UNKNOWN | AC-11/12 |
| V-SRV-03 | Topic PREPARED/ADDED fsync 故障、Topic Evidence 精确查询 | 只在 ID+Topic+可信持久证据一致时 CONFIRMED，UNCERTAIN/NOT_FOUND 不自动 POST | AC-11/14 |
| V-SRV-EVIDENCE-RESTART | 终态已写但 fsync 返回失败；先在同进程 GET，然后控制进程退出/重启后按真实幸存文件分别测试完整终态、截断尾行和冲突终态 | 进程内已知失败返回 UNCERTAIN；重启后真实完整可信事件可确认**现存领域终态**但不伪造原同步 fsync 成功/HTTP SUCCESS；坏记录 UNCERTAIN | AC-09/11/14 |
| V-SRV-04 | Status Revision 竞争、注册→采样→首帧、同 Revision 新 observed_at | GET/SSE 单源，完整基线先到、STALE/恢复的新 Revision 正确 | AC-13 |
| V-SRV-05 | SSE 慢读 64 Clients、1MiB 最大帧、8MiB/s 候选、Socket backpressure | Application/Trantor/Socket 有界，动态 admission 或断开，不让数据面受网络阻塞 | AC-12/13/16 |
| V-CLI-01 | HTTPX Connect/ReadTimeout、CancelledError、Redirect | 控制 1 次 POST，已经可能送出则 UNKNOWN、Attempt 保留 ID、无隐式 Stop | AC-11/14 |
| V-CLI-RECEIPT-01 | 401/403/400 malformed 等 pre-domain 拒绝省略 operation_id；对照 QUEUE_TIMEOUT/Recorder FAILURE/504 省略或篡改 operation_id | 前者仅在 status+ErrorCode+FAILURE+effects:NONE+request_id/api_version 全部合法时接受明确 FAILURE；后者缺失/不匹配 ID 一律保守 UNKNOWN/ProtocolViolation | AC-11/14/18 |
| V-CLI-EXAMPLE-01 | 对文档 sync/async 示例做静态/契约执行：Application 提供 CredentialProvider，Stop 使用 prepare+submit | 示例不绕过必填凭据、不调用不存在的裸 `stop(target)` 隐式 ID 路径；exactly one POST | AC-14/18 |
| V-CLI-02 | httpx-sse Issue #35、注释心跳、合法空 data、CR/LF/CRLF、Unicode、半帧断线 | Parser 标准性和业务过滤分别验证，无虚假 StateUpdate | AC-13/14 |
| V-CLI-03 | SSE Reader 静默阻塞、Watchdog 定时触发、Runtime ID 更新 | 主动 STALE/重新全量基线，不依赖下一帧到达才检查 | AC-13/14 |
| V-APP-01 | CLI Ctrl+C/UNKNOWN、TUI/Web 重连、Web CSRF | CLI 退出码按规范；UI 不误报成功；浏览器无 Runtime control secret | AC-15/18 |
| V-APP-TOPIC-01 | Graph 同名返回类型 A+B，分别省略 type、显式 type=A、显式 type=B 发起 Add | 三种均以 TYPE_CONFLICT 拒绝且无 Writer/Subscription 副作用；Client 不把显式 type 当选择器 | AC-02/03/22 |

HTTPX/Drogon 均以项目锁定版本运行，Server 单次 FULL Frame/全局 Byte Budget/下游慢消费者的资源上限不能由客户端 MockParser PASS 代替。OPEN-02 必须明确参考网络延迟/带宽、最大快照、连接并发与新鲜度测量方法/合格阈值；V1.0 Security 必须按 AC-18 验证 Bearer Token、read/control Scope、TLS/受信代理、Secret 泄漏、Web Session/CSRF 与跨 Origin 攻击。

## 3.2 Humble Fast DDS / TSan 开放风险

**状态：OPEN。** 历史 Phase-0 Humble AMD64 Executor TSan 曾报告 Fast DDS 2.6 / spdlog mutex 相关诊断；相同诊断类别在**不使用 ros2_bag Pool、Queue、WriterAdapter 的最小 ROS 节点**中也曾复现。这只能说明旧证据没有把告警归因到 ros2_bag，**不能证明依赖栈告警无害，也不能证明当前项目没有被其掩盖的竞态**。

历史本地证据曾包含 Humble Runtime、最小 ROS baseline、project-only core/payload 和 Jazzy Executor 结果，但这些文件不再作为仓库规范的一部分，也不能被新 Source SHA 继承为 PASS。G0/G3 必须重新产生当前证据：固定 Ubuntu/ROS/RMW/FastDDS/spdlog 版本与编译插桩，保留 full Humble run 的 unsuppressed TSan 输出；同时对 project-owned Queue/Pool/Control 域做聚焦 Sanitizer，以区分第三方诊断和项目自身 race。若尝试改变 Transport/RMW 规避告警，仍须重新验证 discovery、receipt、throughput、latency、CPU 与数据完整性。

发布前必须给出以下之一并形成 G3 证据：依赖或项目问题已修复；在固定依赖上可重复证明风险被有效缓解；或由 Tech Lead/Project Owner 明确记录剩余风险接受。**确认的数据损坏、UAF 或项目自身 race 不允许通过风险签字豁免。** 本节是该风险的唯一现行规范记录，不依赖已删除的独立风险文档。

# 4. G0–G3 审查门禁与可执行输出

## 4.1 Gate 定义

| Gate | 进入条件 | 允许退出状态 | 强制产物 |
|---|---|---|---|
| G0 / 领域安全基线 | Recorder Pool/Queue/Writer 模型、Type/Schema 的可行性、当前源代码风险 | ALLOWED / ALLOWED_WITH_CONDITIONS / BLOCKED（有条件仅允许继续开发） | g0_admission_review.md、源码 SHA、Sanitizer/TSan 风险 |
| G1 / 功能集成 | Core Topic/Writer/Recovery、HTTP/Protocol/Client 验收与标准负载初测 | ALLOWED/BLOCKED | g1_review.md、故障注入记录与环境清单 |
| G2 / 跨平台 | 两 ROS Distro × 两 CPU Architecture、MCAP 2x2、自定义无包解析 | ALLOWED/BLOCKED | g2_review.md、原生日志/MCAP/CI SHA |
| G3 / 发布 | AC-01～27 全量、当前 MUST/AC/V-* 全部闭合、OPEN-01/02、AC-18 Security、TSan 和包安装 | APPROVED/REJECTED；不允许 CONDITIONAL 当正式发布 | g3_release_review.md、Release Manifest/哈希/签审 |

本文 §3.2 的 Humble Fast DDS/TSan 风险保持 OPEN，直到当前 Source/Dependency SHA 的复测和处置证据完成。G0 的 `ALLOWED_WITH_CONDITIONS` 不得被 G3 当成“已解决”。出现确认的内存/数据完整性缺陷不能靠单人签字跳过；同一人可担任 Tech Lead/Owner 但必须如实标注。

## 4.2 每次 Gate 检查的机器校验条件

至少产出可机器处理的 `evidence-index.json`：包含 `gate`、规范性的 `gate_result_kind`（G0～G2=`admission`，G3=`decision`）、对应 `gate_result`、本阶段须执行 Test IDs、运行平台、Source SHA、Config/Dependency SHA、原始 log/MCAP/golden 的 sha256、每项 `PASS/FAIL/BLOCKED/NOT_RUN`、责任人与审查时间。Gate 校验脚本应拒绝：**验证规格中存在重复 `test_id`**、Gate 类型与枚举不匹配、必需 ID 缺失、非 PASS 伪装为通过、结果文件不存在、哈希不符、环境平台不匹配、版本未锁定、OPEN 未关闭却 G3 APPROVED。

验收汇总关联当前 AC-01～27、所有 V-* 场景和新增 V-* 测试；允许一个 AC 被多个 V-* 覆盖，反之亦然。不引入已废弃的 UDS 等历史接口；每个当前 MUST/AC 都需要可执行证据。每次 Gate 不得仅填“详见总体报告”而没有原始证据 URI/相对路径。

# 5. 待量化 Profile 与发布前阻断条件

| 问题 | 本规范状态 | 发布条件 |
|---|---|---|
| OPEN-01 CPU / 硬件 | PENDING；10 Topics ×1000Hz×1KiB×600s 的单 Topic <1% 丢失不可降低，CPU 的参考机/口径待决定 | 固定 CPU 核心归一口径、Recorder+Server 范围、采样/聚合与数值 |
| OPEN-02 HTTP/SSE | PENDING；Server 2s/5s、64 Clients/8MiB/s 为可验证候选 | 定量 SLA、资源上界、反压时断连能力的真实证据 |
| V1.0 Security | DESIGN CLOSED；Bearer Token + read/control Scope + TLS direct/trusted-proxy | AC-18 全量攻击/权限/泄漏/证书/代理/Web Session/CSRF 证据通过后才获得远程生产准入 |
| Humble FastDDS TSan | OPEN；现行风险记录见本文 §3.2 | 以当前 Source/Dependency SHA 分离第三方与自有 race、形成修复/缓解/明确风险处置并完成 G3 决策 |
| Topic Evidence | REQUIRED | Recorder Query、Event 身份链、fsync/截断/冲突故障注入全部通过 AC-11 对应 V-* 测试 |
| 频率/Gap | REQUIRED | 以 FakeClock 验证完整独立评价周期、边界误差和 Unknown 语义，并覆盖 AC-10/AC-22 |
| Cleanup durable pending | REQUIRED | Queue full、删除失败、重启、跨任务 pending 的持久授权与重试故障注入通过 AC-05/AC-09/AC-10/AC-17 |

本文件是**可执行验证计划/测试规格**，不是已运行测试报告；阈值注明候选时仍须按 OPEN 正式批准。后续 Code 实现/部署不得通过降低 Recorder 的数据安全、UNKNOWN 或 SSE 新鲜度来换取 PASS。
