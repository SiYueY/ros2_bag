# ros2_bag 产品需求规格

| 属性 | 内容 |
|---|---|
| 文档版本 | v0.3.4 |
| 文档状态 | **DRAFT — V1.0 开发阶段，随时修订；规范尚未批准用于完整实施** |
| 适用范围 | ros2_bag V1.0 |
| 上位架构 | docs/architecture.md（开发稿，可直接联动修改） |
| 技术选型依据 | `docs/system_technology_selection.md` |

# 1. 产品定位与范围

## 1.1 目标用户与使用场景

`ros2_bag` 面向 ROS 2 机器人研发、调试、测试与运行诊断，允许操作者在不介入 ROS 2 消息数据路径的情况下，选择所需 Topic，持续或定时录制为可离线分析的 MCAP，并从本机或受控远程界面执行管理操作。

V1.0 覆盖以下典型场景：

- **故障复现：** 对关节、底盘、控制反馈、IMU、感知等标准或自定义 Topic 按需录制，保留异常前后的完整可用数据及诊断信息。
- **实验采集：** 按指定时长录制，运行中暂停、恢复、添加或移除 Topic，并在结束后通过常见 ROS 2 分析工具读取数据。
- **长时间运行：** 采用 Rolling 分段和保留策略，限制磁盘持续占用；磁盘风险出现时优先保护已有数据。
- **远程操作：** CLI、TUI、Web 经统一 Client/Protocol 查询状态和控制录制，网络中断不改变 Recorder 的运行状态。
- **跨环境分析：** 在不同受支持 ROS 2 发行版上读取 MCAP；对自定义消息验证脱离原消息包的解析能力。

## 1.2 需求级别、验收口径与权责

本文定义用户可观察的功能和质量要求，不规定 C++ 对象结构、线程池、HTTP 路由、JSON 字段或 UI 像素级细节。层次划分、所有权和数据面隔离以 `architecture.md` 为准；技术选择以 `system_technology_selection.md` 为准。具体行为由 `recorder.md`、`protocol.md`、`server.md`、`client.md` 及各 Application 文档细化。

- **MUST（必须）：** V1.0 发布阻断项。关联验收用例失败时不能宣称满足 V1.0。
- **SHOULD（建议）：** 有明确价值但不单独阻断 V1.0；偏离需记录原因和替代措施。
- **OPEN（待裁定事项）：** 不是第三种需求级别，而是独立于 MUST/SHOULD 的待决问题，标记尚需产品决策或量化条件的事项；按 §5.2 区分尚待设计决策与发布阻断项，不能作为已通过的标准。
- **AC：** 验收用例 ID；验证必须记录环境、输入、结果、日志和证据位置。负向测试与竞争测试同样属于验收。

架构与需求都处于开发阶段；如果发现产品目标或实现安全性需要调整，应同步修改架构、需求、模块设计和验收标准。本文不记录任务进度、既往测试通过率或发布状态。

## 1.3 V1.0 范围及非目标

V1.0 由 **Recorder、Server、Client、Application（CLI/TUI/Web）** 构成，Protocol 为 Server 与 Client 的独立契约。一个 Recorder Runtime 是单个 C++17 进程，承载一个 Recorder 实例；同一时间仅允许一个活动录制任务，任务可包含多个 Topic 和多个存储分段。

V1.0 必须提供可用 Topic 发现与动态管理、录制任务及历史索引、状态与诊断日志、MCAP 输出、保守恢复、资源保护、HTTP/JSON 控制、SSE 状态通知、Python Client，以及可实际操作的 CLI、TUI 与 Web。**大载荷录制的正确性与丢弃可追溯性属于 MUST，但大载荷下的固定吞吐、时延或零丢弃不属于 V1.0 发布阻断指标。**

**不包含：** ROS 2 Service/Action 录制、多 Recorder 并发或分布式集群、云端录制存储、ROS 2 数据流经 HTTP/SSE 转发、浏览器直接接入 ROS 2、独立消息 Broker、C++ Client SDK、Python/C++ Binding、WebSocket/gRPC、插件式通用工作流引擎、内置诊断包打包下载服务、完整 ROS 2 Runtime 捆绑发布。未来版本是否支持不属于本文件承诺。

# 2. 功能需求

## 2.1 录制与 Topic

| ID | 级别 | 需求 | 验收 |
|---|---|---|---|
| FR-REC-01 | MUST | 支持标准及自定义 ROS 2 Topic 的序列化录制，不要求将业务消息类型静态编译进 Recorder 二进制；目标 ROS 2 环境必须提供所需类型支持，缺失时返回可诊断错误。 | AC-01、AC-06 |
| FR-REC-02 | MUST | 基于 ROS Graph 解析 Topic 名称、类型与发布端信息；同名 Topic 若可见多种类型，拒绝加入并返回冲突类型诊断；不可观测或类型不可解析时不伪造唯一类型。 | AC-02 |
| FR-REC-03 | MUST | 支持适配发布端 QoS，并可通过配置明确 QoS 策略；不兼容或无法订阅的情况有可识别错误和状态。 | AC-02 |
| FR-REC-04 | MUST | 活动任务期间允许动态添加、移除 Topic；同名重添加须区分 Topic 生命周期，不能混入旧 Topic 实例的数据。**同一 recording task 内同名 Topic 的 type + serialization format + schema encoding + 完整 Schema SHA-256 必须保持一致**；Remove 后以不同类型或不同 Schema 重加必须在 Writer/Subscription 副作用前拒绝，类型变化通过新 recording task 承载。 | AC-03、AC-06 |
| FR-REC-05 | MUST | 支持 Normal 模式，接受显式 Start/Stop；无消息录制也能正确结束和表达空结果。 | AC-04 |
| FR-REC-06 | MUST | 支持 Timed 模式，使用 steady clock 计算总期限；暂停仍计入期限。TaskIngressState 保存不可变 steady deadline，**每个 Producer 在准入线性化点联合校验 generation/gate/deadline**；ROS Timer 只负责尽早触发 Stop，调度延迟不得扩大截止后的准入窗口。已经在截止前通过准入线性化点的消息仍可完成写入。因此“倒计时为 0”表示接入期限已到，不等于任务已完成/已进入 IDLE。 | AC-04 |
| FR-REC-07 | MUST | 支持 Rolling 模式，分段形成可独立读取的 Bag，并对达到保留条件的已完成分段执行自动清理；暂停期间保留窗口继续推进。 | AC-05 |
| FR-REC-08 | MUST | 支持 Pause/Resume/Stop，提供可识别状态与暂停原因；不得在已停止或安全禁止的状态下错误恢复接入。默认 `auto_resume=false` 时，系统危险解除且稳定后必须保持明确的“等待人工 Resume”暂停原因，使用户可**一次直接 Resume** 恢复；Resume 不得清除仍生效的磁盘危险。 | AC-04、AC-08 |
| FR-REC-09 | MUST | 录制链路具备可配置的消息/字节预算，超限按确定性策略处置；应区分队列预算、分配或入队失败所致的接入丢弃，与消息接纳后的 Writer/存储失败，提供对应计数与故障诊断，不得静默丢弃或无界缓存。 | AC-07、AC-12、AC-27 |
| FR-REC-10 | MUST | Stop 关闭新消息接入并等待已接纳消息写入或显式记录失败；不允许静默丢弃已接受队列数据并报告成功。 | AC-04、AC-07 |
| FR-REC-11 | MUST | 支持按配置保留必要的 Snapshot 及 Rolling 分段边界数据，遵守容量上限和 Topic 世代隔离。 | AC-05、AC-07 |
| FR-REC-12 | MUST | 提供统一的可用 Topic 发现/列表能力，至少返回 ROS Graph 可见的名称、类型、可见发布者数量或等效发布信息及可观测性状态；区分“Graph 可见但当前无发布者”和“Graph 未发现”，不把未发现断言为永不存在。频率估计仅在具备观测样本时可提供，未测量须明确标记未知。 | AC-02、AC-22 |
| FR-REC-13 | MUST | 录制消息以 Recorder **实际接收时的系统时钟 Unix 时间**为时间基准；MCAP 的 `log_time` 与 `publish_time` 均写入该值，其中 `publish_time` 仅为格式兼容赋值，不表示源端真实发布时间。V1.0 不解析 `Header.stamp`，也不使用 ROS/use_sim_time 作为接收时间；Timed/Rolling/Timeout 使用 steady clock。系统墙钟可能跳变，因此不承诺跨 Topic 或单 Topic接收时间严格单调。 | AC-24 |
| FR-REC-14 | MUST | 每次 Start 形成不可变的任务有效配置：mode/TIMED duration/ROLLING segment+retention 来自本次 Start 请求及 Runtime 默认，压缩、QoS 默认策略和 RECEIVE 时间戳策略来自启动时已验证的 Runtime 配置。V1.0 HTTP 不按任务覆盖压缩/QoS/时间戳策略。任务启动后这些值不得热变更；Topic Add/Remove 仅改变显式动态内容。 | AC-25 |

## 2.2 数据产物、状态与恢复

| ID | 级别 | 需求 | 验收 |
|---|---|---|---|
| FR-DAT-01 | MUST | 录制文件使用 MCAP，保存有效 Topic/Channel/Schema 等解析所需信息，支持 ROS 2 CDR 消息回读。 | AC-06 |
| FR-DAT-02 | MUST | MCAP 应保存可解析的自定义消息定义（Schema）及传递依赖，使指定分析工具在未安装原始自定义消息包时仍能正确解析嵌套结构与字段；不能以“写入成功”代替独立分析端解析验收。 | AC-06 |
| FR-DAT-03 | MUST | 提供录制任务、分段和 Topic 事件的持久化事实，准确表达完成、删除中、已删除、不完整及不确定状态。Segment/Task 标记 COMPLETE 前，除 Writer Close 外还必须按支持的本地 POSIX 故障模型同步 MCAP、metadata、必要目录项，再提交 durable Manifest；Manifest fsync 不能替代数据文件同步。 | AC-05、AC-09 |
| FR-DAT-04 | MUST | 非正常终止后保守恢复：保留可能仍有价值的 INCOMPLETE 数据，不自动删除损坏或未知状态片段；有依据才可确认清理已完成。Rolling retention 的新删除授权不得由墙钟差值产生：进程内使用 steady age；重启后 durable COMPLETE 重新计满完整 retention，只有崩溃前已 durable DELETING 才可直接继续删除。 | AC-05、AC-09 |
| FR-DAT-05 | MUST | 磁盘安全保护覆盖空间临界及采样不可知/过期；安全状态不足时禁止不安全启动或恢复，并提供原因诊断。 | AC-08 |
| FR-DAT-06 | MUST | 对外提供录制、Topic、消息计数/丢弃、Queue/Pool、磁盘、CPU/RSS/线程/FD 等诊断信息；Topic 计数应区分实时成功写入（`written_live`）和 Snapshot 跨 Segment 成功重放（`replayed_to_new_segment`），不得将重放当作新接收消息；Cleanup 应暴露 `cleanup_queue_depth`、`cleanup_pending_total` 和 `cleanup_queue_full_count` 等可诊断计数，并区分队列等待项与持久化待删项。未采集数据与零值须可区分。 | AC-10 |
| FR-DAT-07 | MUST | 区分录制领域事实、Recorder Snapshot 与网络通知；Snapshot 不冒充完整历史事件，存储异常须可被查询和诊断。 | AC-09、AC-10、AC-13 |
| FR-DAT-08 | MUST | 同一输出目录不得被两个相互协作的 Recorder Runtime 同时用于可能冲突的恢复、录制或自动清理。 | AC-09 |
| FR-DAT-09 | MUST | Recorder 领域提供限定在配置输出根目录内的**只读历史任务索引/查询能力**，支持按 ID/名称、时间范围检索，并返回录制身份、路径、状态及可获得的起止时间、分段和消息摘要；Server 仅做鉴权、调用与 Protocol 映射，不重复实现 Manifest/Recovery 解析。单任务 Segment 详情必须分页且 Manifest Segment 历史有明确容量上限；只允许将最老的 durable REMOVED 连续前缀压缩为汇总计数/高水位，其他状态不得为省空间丢弃。达到上限且无法安全压缩时停止继续创建 Segment，而不是无界增长。缺失的时间、计数等元数据显式为未知，不能伪造零值。 | AC-23 |
| FR-DAT-10 | MUST | Recorder Runtime 与 Server 提供可获取的诊断日志，至少包含事件时间、级别、组件、主要错误或操作关联信息；通过部署日志收集或可配置日志输出实现留存与检索。不得泄露凭证或原始敏感数据；V1.0 不强制实现内置诊断包导出功能。 | AC-10、AC-26 |
| FR-DAT-11 | MUST | Camera/LiDAR 等大载荷消息遵守数据完整性约束：已接纳并成功写入的消息必须在 MCAP 中保留正确序列化载荷、Schema 与时间戳；接入预算丢弃须有准确计数，接纳后的 Writer/存储失败须有独立可诊断结果。**数据正确性不等于零丢弃或固定吞吐保证**。 | AC-27 |

## 2.3 统一控制、网络协议与 Client

| ID | 级别 | 需求 | 验收 |
|---|---|---|---|
| FR-CTL-01 | MUST | 通过 HTTP/JSON 提供录制任务、Topic 控制和权威状态查询，输入校验、错误语义与 API 版本一致且有文档。 | AC-11、AC-13 |
| FR-CTL-02 | MUST | 同一 Recorder 的外部状态变更具有可判断的调用顺序和有界受理能力；内部定时停止、磁盘保护等变化可安全交错。Recorder mutation 通过进程内受管 Completion/ControlHandle 表达真实 DONE；HTTP ResponseSink 超时可以先结束，但 Server 在该 Handle DONE 前不得释放唯一外部 mutation 执行权去启动下一项冲突操作。 | AC-11、AC-12、AC-17 |
| FR-CTL-03 | MUST | HTTP 超时、断线或取消不等于 Recorder 操作失败。**原始请求结果**只由该次请求的可信同步回执判定；后续与具体 `recording_id + operation_id + 领域身份` 精确关联的持久化证据只能确认相应**领域终态/副作用**，不得倒填或改写原始 HTTP `OperationResult`。仅时间窗口、当前状态值相符或事件序列位置均不足以证明某次请求的归属或成功。缺少可信同步回执时原始结果保留 `UNKNOWN`；Topic Add/Remove 可另行查询精确关联的领域证据，Start/Stop/Pause/Resume 在 V1.0 不提供通用请求级终态查询。Client 不得自动重试有副作用的命令。 | AC-11 |
| FR-CTL-04 | MUST | SSE 向订阅者通知录制状态变化；首次连接和重连能建立完整状态基线，慢客户端与订阅建立期间的缓冲超限不致破坏状态一致性。 | AC-13 |
| FR-CTL-05 | MUST | SSE 不承担原始 ROS 2 消息流与可靠事件日志；客户端事件缺口、Runtime 代际变化或连接恢复时可重新同步权威状态。 | AC-13 |
| FR-CTL-06 | MUST | 提供统一 Python Client SDK，支持同步和异步控制/查询、SSE 消费、超时、错误映射与资源关闭。 | AC-14 |
| FR-CTL-07 | MUST | Client/Application 退出或网络断开不自动停止录制；Recorder 不能依赖某个 UI 在线运行。 | AC-11、AC-14 |
| FR-CTL-08 | MUST | 操作结果为 `UNKNOWN` 时，Client 应重新查询并展示权威 Recorder 状态，保留该请求终态未知的提示，供用户显式确认或决定后续操作；状态刷新不得自动将请求标记为成功/失败，也不得默认重试非幂等或幂等性未证明的命令。 | AC-11、AC-14 |

## 2.4 CLI、TUI 与 Web

| ID | 级别 | 需求 | 验收 |
|---|---|---|---|
| FR-APP-01 | MUST | CLI 支持录制控制、Topic 管理、状态查询和自动化调用，成功/失败/未知结果具有清楚的输出与退出码。 | AC-15 |
| FR-APP-02 | MUST | TUI 支持查看录制、Topic 与资源状态，执行常用控制，并明确显示网络断开、状态陈旧及系统保护原因。 | AC-15 |
| FR-APP-03 | MUST | Web 提供录制控制、状态展示、历史录制列表与详情入口；通过 Web 后端及 Client 访问 Recorder，不要求浏览器直接连接 ROS 2。 | AC-15、AC-23 |
| FR-APP-04 | MUST | 三类 Application 使用同一 Protocol/Client 语义，不应对同一操作出现相互冲突的成功、失败或 UNKNOWN 解释。 | AC-14、AC-15 |
| FR-APP-05 | MUST | Rolling 清理、磁盘暂停、数据 INCOMPLETE、操作 UNKNOWN 等影响数据安全的情况应明确提示，不得误显示为正常完成。 | AC-15 |
| FR-APP-06 | MUST | CLI、TUI 和 Web 均通过统一 Client/Protocol 显示可用 Topic 列表，提供名称、类型、可见发布者信息及是否可录的诊断；不得各自实现独立 ROS Graph 发现语义。可用频率估计若存在，应标识估算来源和未知状态。 | AC-15、AC-22 |
| FR-APP-07 | MUST | CLI 和 Web 提供历史录制列表、按标识或时间查找及摘要查看；TUI 的历史列表为 SHOULD。不能将 INCOMPLETE 或状态未知的录制显示为已正常完成。 | AC-15、AC-23 |

# 3. 非功能需求

## 3.1 性能、资源与容量

| ID | 级别 | 指标或约束 | 验收条件 |
|---|---|---|---|
| NFR-PERF-01 | MUST | 使用统一标准负载衡量端到端录制能力：10 Topics × 1000 Hz × 1 KiB 序列化载荷，10,000 messages/s，持续 600 s。 | AC-16；发布者、Executor、RMW、MCAP、硬件及软件版本、压缩参数必须固定并保存。 |
| NFR-PERF-02 | MUST | 在**发布端实际达到规定标准负载并处于事先约定频率偏差范围内**时，每 Topic 消息损失率 **小于 1%**；不得用总平均掩盖单 Topic 丢失。 | AC-16；发布端未达到约定频率时该次端到端负载验收无效，可另作分层诊断；按序号与计数分离 Publisher、DDS 和 Recorder 归因。频率偏差须在测试方案中事先冻结。 |
| NFR-PERF-03 | MUST | V1.0 发布需具备针对标准负载的 Recorder Runtime CPU 使用率验收门槛；**具体数值待 OPEN-01 决定**（初始候选：单核归一化 ≤30%，未被确定为验收门槛）。 | AC-16；固定参考硬件、Recorder/Server 计入范围、CPU 百分比定义、采样与聚合口径后锁定正式阈值；OPEN-01 关闭前不能完成发布验收。 |
| NFR-PERF-04 | MUST | Queue/Pool/Snapshot 以及 HTTP 控制队列、SSE 客户端缓存与并发连接数均应有明确的配置或实现上限；达到上限要有可识别策略。 | AC-07、AC-12、AC-13；观测最大占用和丢弃/拒绝计数，不得无限增长。 |
| NFR-PERF-05 | MUST | 内存验收覆盖 ROS 2、DDS、线程栈、Queue/Pool、Writer、Server 和 Runtime；队列字节预算不等于进程 RSS 预算。 | AC-16；记录 RSS 峰值与时间序列，循环 Start/Stop、反复订阅/断连后无持续无法解释的资源增长。 |
| NFR-PERF-06 | SHOULD | 连续 5 分钟 RSS 滑动趋势超过 **1 MiB/min** 时触发泄漏调查。该数值为**诊断触发条件**，不是单凭 RSS 斜率判定泄漏或发布失败。 | AC-16；保存趋势样本与调查结果。 |

标准负载定义以 `tests/performance/standard_profile.json` 为候选试验配置来源；其队列、池和采样参数属于**基准负载条件**，不是任意环境或大图像/点云录制的统一系统上限。大载荷性能需要独立 profile 和结果。CPU 目标与未指定硬件有关，必须在正式验收前补齐条件。

## 3.2 正确性、可靠性与故障隔离

| ID | 级别 | 约束 | 验收 |
|---|---|---|---|
| NFR-REL-01 | MUST | 不出现消息跨任务/Topic 世代混写、UAF、重复释放、竞态破坏或将未完成写入报为成功。 | AC-03、AC-07、AC-09、AC-17 |
| NFR-REL-02 | MUST | Writer 故障、磁盘不可知、资源超限与异常恢复必须呈现可解释的降级或失败，不得静默破坏已持久化数据。 | AC-08、AC-09、AC-12 |
| NFR-REL-03 | MUST | Server 请求执行和 SSE 订阅不得阻塞 ROS 2 高频回调或 Writer；无界慢客户端不得造成录制背压。 | AC-12、AC-13、AC-16 |
| NFR-REL-04 | MUST | Runtime Shutdown 必须先停止新控制，协调在途操作，并维持必要的 ROS 2 Executor/Context 直到 Recorder 清理完成。 | AC-17 |
| NFR-REL-05 | MUST | 不将网络断连、HTTP 超时或界面崩溃转换为隐式 Stop；重新连接后仍可获得权威录制状态。 | AC-11、AC-13、AC-14 |
| NFR-REL-06 | MUST | 对丢失、失败、拒绝及恢复异常有足够诊断证据，便于区分系统问题与上游发布/DDS 问题。 | AC-10、AC-16 |

## 3.3 安全、兼容性与部署

| ID | 级别 | 约束 | 验收 |
|---|---|---|---|
| NFR-SEC-01 | MUST | Server 默认仅监听受限本机地址；Runtime API 使用预共享不透明 Bearer Token 执行 `read` / `control` scope 授权，`control` 包含 `read`。Token 仅从受限 Secret 文件/Provider 装载，不进入普通配置输出或日志；远程访问还必须使用 TLS（Drogon 直接 TLS 或受信反向代理终止），且 Runtime 仍自行验证 Bearer Token。 | AC-18 |
| NFR-SEC-02 | MUST | Web 的会话与 CSRF 防护独立于 Recorder HTTP 控制面的认证授权；输入、Topic 和文件路径参数必须校验。 | AC-18 |
| NFR-SEC-03 | MUST | 不在普通 API、SSE 或日志泄露密钥；录制输出目录受文件系统权限与独占所有权保护。 | AC-09、AC-18 |
| NFR-COMP-01 | MUST | 验证 Ubuntu 22.04/ROS 2 Humble 与 Ubuntu 24.04/ROS 2 Jazzy，均覆盖 AMD64、ARM64。 | AC-19；原生构建、运行、MCAP 读回与最小功能回归。 |
| NFR-COMP-02 | MUST | 验证 MCAP 在 Humble/Jazzy 录制端与分析端的 **2×2** 组合中可按固定插件版本解析；纯自定义消息闭包须在分析端不安装原消息包时验证。 | AC-06、AC-20 |
| NFR-COMP-03 | MUST | 提供原生 Runtime 可执行制品及独立 Python Client/Application 安装方式，明确系统 ROS 2/RMW/MCAP 插件依赖及许可/版本信息。 | AC-19、AC-21 |
| NFR-COMP-04 | MUST | 构建与发布可复现，第三方依赖有固定来源、版本和许可证记录；发布包不含必须依赖开发源码树才能运行的隐含路径。 | AC-21 |

# 4. 验收标准与证据

## 4.1 验收用例矩阵

下列 AC 是**发布时必须执行的验收场景**，不代表它们已经实现或已经通过。各模块设计应将具体测试输入、预期状态/错误与脚本实现落实为可重复执行的检查。

| AC | 验收场景与通过条件 | 需求覆盖 |
|---|---|---|
| AC-01 | 标准和未静态编译进 Recorder 的自定义消息在类型支持可用时均能录制，字段与发布值一致；类型支持缺失时返回清楚错误。 | FR-REC-01 |
| AC-02 | ROS Graph 可见无发布者、未发现、多类型冲突及 QoS 兼容/不兼容场景有可重复的发现、列表与错误结果；不把当前未发现误报为绝对不存在。 | FR-REC-02、FR-REC-03、FR-REC-12 |
| AC-03 | 动态 Add/Remove/同名重添加与 Stop 并发；无旧 epoch 串入新实例、无错误身份或 UAF；同任务内 Remove 类型 A 后同名 Add 类型 B/Schema 变化在任何新 Writer/Subscription 副作用前拒绝，同签名重加可正常产生新 Epoch。 | FR-REC-04、NFR-REL-01 |
| AC-04 | Normal/Timed/Pause/Resume/Stop 正常、空数据、错误状态及 Timed 暂停时长边界可重复验证；人为延迟 Timed Timer/Executor 后，deadline 后 Producer 仍无法准入；默认系统暂停恢复安全后用户可一次直接 Resume。 | FR-REC-05、FR-REC-06、FR-REC-08、FR-REC-10 |
| AC-05 | Rolling 分段独立可读；Retention 仅对获得 steady 删除授权的 COMPLETE 分段清理，墙钟前跳/回跳不提前删除；重启后未授权 COMPLETE 重新计满 retention，durable DELETING 可继续；暂停时窗口推进，Snapshot 与清理结果正确。 | FR-REC-07、FR-REC-11、FR-DAT-03、FR-DAT-04 |
| AC-06 | MCAP CDR 与 Schema/Channel 可读；纯自定义消息及其传递依赖在分析端不安装原消息包时能正确还原嵌套结构、字段和值。 | FR-DAT-01、FR-DAT-02、FR-REC-01、NFR-COMP-02 |
| AC-07 | Queue/Pool/Snapshot 预算极限、并发消息、溢出和停止 Drain 测试；无无界内存、无已接纳数据静默丢失。 | FR-REC-09、FR-REC-10、FR-REC-11、NFR-PERF-04、NFR-REL-01 |
| AC-08 | 磁盘 WARNING/CRITICAL/UNKNOWN、采样超时及恢复条件；危险状态拒绝不安全 Start/Resume。默认 `auto_resume=false` 时安全稳定后进入可识别的 DISK_RECOVERY_PENDING，用户无需先 Pause 即可一次直接 Resume；活动危险原因不得被 Resume 清除。 | FR-DAT-05、FR-DAT-06、NFR-REL-02、FR-REC-08 |
| AC-09 | 硬退出、模拟数据文件/metadata/目录/Manifest 各同步阶段失败、部分 Event 写入、INCOMPLETE、DELETING、重复 Recovery；只有 MCAP/metadata/目录项和 Manifest 均满足持久化顺序才可 COMPLETE，数据不误删，输出根目录互斥有效。 | FR-DAT-03、FR-DAT-04、FR-DAT-07、FR-DAT-08、NFR-REL-01、NFR-REL-02、NFR-SEC-03 |
| AC-10 | 录制/Topic/丢弃/磁盘/CPU/RSS/线程/FD 指标可观察；分别核对 `written_live`、`replayed_to_new_segment`，重放不计入原始接收量；Cleanup 队列满时 `cleanup_queue_depth`、`cleanup_pending_total`、`cleanup_queue_full_count` 可见，待删意图不因拒绝入队而消失。未采样、零值与错误状态可区分；诊断日志可关联关键错误。 | FR-DAT-06、FR-DAT-07、FR-DAT-10、NFR-REL-06 |
| AC-11 | 并发 Start/Stop、Timeout/断连及内部定时停止、磁盘保护交错。**子场景 A：Stop 请求超时，同时内部定时 Stop 使状态变为 Idle，不能凭 Idle 认定该请求成功；子场景 B：Stop 请求超时，同时磁盘保护触发 Pause，最终状态为 Paused，不能直接认定该请求失败**。缺乏可信同步回执时原始 `OperationResult` 保持 UNKNOWN；刷新 CurrentStatus 只能说明当前状态。对 Topic Add/Remove，仅精确关联的 durable Topic Evidence 可另行确认领域终态/副作用，且不得把原始 HTTP UNKNOWN 改写为 SUCCESS。Client 提示用户且不自动重试有副作用的操作。 | FR-CTL-01、FR-CTL-02、FR-CTL-03、FR-CTL-07、FR-CTL-08、NFR-REL-05 |
| AC-12 | 控制面过载、任务队列满、慢请求、并发查询；请求有界拒绝且 Recorder 录制数据面可持续工作。 | FR-CTL-02、NFR-REL-02、NFR-REL-03、FR-REC-09、NFR-PERF-04 |
| AC-13 | SSE 首次连接/重连/订阅同步空窗/超限/慢客户端/代际变化；无永久陈旧状态，必要时重新全量同步。 | FR-CTL-01、FR-CTL-04、FR-CTL-05、FR-DAT-07、NFR-PERF-04、NFR-REL-03、NFR-REL-05 |
| AC-14 | Client 同步/异步接口、错误分类、资源释放、SSE 重连；UNKNOWN 后刷新状态但不伪造操作终态或自动重试；CLI/TUI/Web 共享一致的操作语义。 | FR-CTL-06、FR-CTL-07、FR-CTL-08、FR-APP-04、NFR-REL-05 |
| AC-15 | CLI 自动化及退出码、TUI 状态和断线提示、Web 控制与会话保护；Topic 列表和历史入口符合应用能力约定，异常状态可见且不误报成功。 | FR-APP-01、FR-APP-02、FR-APP-03、FR-APP-04、FR-APP-05、FR-APP-06、FR-APP-07 |
| AC-16 | 固定标准负载与资源观测；发布端频率须处于预先约定的偏差内，否则标准端到端负载验收无效；有效试验逐 Topic 序号损失率小于 1%，并记录 CPU/RSS/存储及 Publisher/DDS/Recorder 分层归因。 | NFR-PERF-01、NFR-PERF-02、NFR-PERF-03、NFR-PERF-04、NFR-PERF-05、NFR-PERF-06、NFR-REL-03、NFR-REL-06 |
| AC-17 | SIGINT/SIGTERM、重复 Shutdown、并发 Stop、Reader/Writer 生命周期和 Executor 自等待风险；普通 mutation 的 ControlHandle 在 HTTP/调用方等待超时后仍可保持领域 Lease 直到真实 DONE，唯一外部 mutation 执行权不会提前释放；无死锁、UAF 或遗留活跃写入。 | FR-CTL-02、NFR-REL-01、NFR-REL-04 |
| AC-18 | 覆盖缺失/错误 Bearer Token、read-only Token 调用 control、Token 泄漏检查、TLS 缺失/证书错误、伪造 `X-Forwarded-*`、非受信代理、Web Session/CSRF、跨 Origin、恶意输入与路径；全部拒绝不安全操作且不泄露 Secret。 | NFR-SEC-01、NFR-SEC-02、NFR-SEC-03 |
| AC-19 | Humble/Jazzy × AMD64/ARM64 原生构建、启动、Stop、MCAP 读回及依赖检查。 | NFR-COMP-01、NFR-COMP-03 |
| AC-20 | PlotJuggler/MCAP 解析器在两发行版的录制端 × 分析端 2×2 实机/容器组合验证字段和值，并保存插件与 Schema 信息。 | NFR-COMP-02 |
| AC-21 | 独立发布包在干净目标环境安装，启动/配置/依赖/许可和功能检查完整，不借助开发树隐藏资源。 | NFR-COMP-03、NFR-COMP-04 |
| AC-22 | CLI/TUI/Web 从相同发现接口列出可用 Topic，名称/类型/发布者数一致；未发现、可见但无发布者、多类型冲突及频率未测量均正确呈现。 | FR-REC-12、FR-APP-06 |
| AC-23 | 生成 COMPLETE、INCOMPLETE 和 FAILED（或不可判定）历史录制；通过 Recorder 只读索引与 CLI/Web 查询 ID/时间、路径和元信息，与 Manifest/Recovery 一致，缺失时间/计数为未知而非零。长期 Rolling 超过 Segment 记录上限时仅压缩 durable REMOVED 前缀，详情以 `segment_limit/segment_offset` 分页并返回 compacted summary；无法安全压缩时阻止继续创建 Segment。Server 无须重新解析录制持久化格式。 | FR-DAT-09、FR-APP-03、FR-APP-07 |
| AC-24 | 使用 `Header.stamp` 与系统接收时刻不同的消息，并令 use_sim_time 未初始化、暂停和回跳；Humble/Jazzy 读取 MCAP 验证 `log_time = publish_time = system_clock 接收时间`，Timed/Rolling 仍按 steady clock 推进，且文档不把 publish_time 解释为源端发布时间。 | FR-REC-13 |
| AC-25 | Start 返回的 effective task config 与 Recorder/Manifest/Status 实际配置一致；RuntimeConfig 在进程启动时一次加载，修改磁盘配置文件不会改变当前或下一 Task，安全重启 Runtime 后才重新加载；HTTP Start 仅覆盖 mode/duration/rolling 时长，不接受 compression/QoS/时间戳覆盖。 | FR-REC-14 |
| AC-26 | Recorder/Server 日志可由部署方式持续获取与检索，包含时间、级别、组件和错误关联信息；异常能够关联日志定位，密钥/敏感信息不泄露。 | FR-DAT-10、NFR-SEC-03 |
| AC-27 | 大载荷样本分别验证正常写入、预算超限与 Writer 故障。**PASS 条件：**成功写入 MCAP 的消息载荷、Schema 与时间戳和接纳时的记录一致且可读；按测试注入和接入/入队观测边界核对接入丢弃计数准确；Writer/存储故障单独报告，未写入成功的已接纳消息不得被报告为成功，也不得混同于接入丢弃。**不以零丢弃或固定吞吐作为 PASS 条件**。 | FR-REC-09、FR-DAT-11 |

## 4.2 性能计算与复现口径

**固定负载：** 每个 Topic 一个发布者，发送可验证单调序号和确定性载荷；发送频率每 Topic 1000 Hz，共 10 Topic；持续 600 s。测试方案必须预先给定频率允许偏差和计算窗口；若发布端实际速率未达约定范围，本次标准端到端损失率验收判无效，不能依据低负载结果给出 PASS。序列化 payload 按 `tests/performance/standard_profile.json` 的 1024 字节配置记录实际大小。消息定义、发布时序、QoS、RMW、Executor、压缩、磁盘与 CPU 型号均须纳入试验记录。

每 Topic 的损失率按下式计算，并报告源端发送计数、订阅接入计数（如可得）、Recorder 丢弃计数和 MCAP 最终可读取计数：

`loss_rate = (expected_sequence_count - distinct_recorded_sequences) / expected_sequence_count`

通过条件：对所有 Topic，`loss_rate < 0.01`。`distinct_recorded_sequences` 以每 Topic 的实际发布序号在**全部 Segment 中去重**后的可读消息集合计算；Rolling Snapshot Replay 所复制的既有序号不能重复计入，也不能弥补不存在的原始发布序号。重复序号、乱序、丢失、计数器溢出、Publisher 未达发送频率等必须单独记录；**不能在发布端自身未满足负载时以较低接收损失率宣称 Recorder 达标**。归因于 Publisher/DDS 的故障须明确标记试验无效或分层评估，不能直接归为 Recorder 问题或将其隐藏。

CPU、RSS 分别报告 Recorder Runtime 与 Publisher，区分进程 CPU 总百分比、单核归一化百分比以及采样窗口。CPU ≤30% 作为**待确认的初始候选阈值**，参考硬件和测量边界确定后才能冻结为定量发布验收门槛。不将 1 KiB 标准负载的吞吐结论推广到 Camera/LiDAR；大载荷正确性按 AC-27 验收，固定吞吐数据仅作为独立性能优化记录。

## 4.3 验收规则

- **功能正确性：** 任一 MUST 功能缺失或对应 AC 不通过，V1.0 不得验收。
- **数据安全：** MCAP 不可读、Schema 闭包不完整、丢失异常无证据、危险数据被自动误删、并发导致 UAF/死锁，均为阻断项。
- **平台及交付：** 四种原生平台组合、跨发行版解析和干净环境安装均需留存验收证据。模拟构建或单平台通过不能替代全矩阵。
- **状态一致性：** HTTP/SSE 的断线、慢客户端及超时场景不得把未知操作报告为成功，也不得使界面长期维持错误状态而无重新同步途径。
- **测量证据：** 每个 AC 记录可复现输入/配置、预期与实际、环境版本、通过/失败结论和证据路径；证据缺失按未验证处理。
- **开放要求：** OPEN 是待裁定事项而非需求级别，按 §5.2 约束冻结或发布阶段；待确定的性能阈值在参考环境和口径锁定前不得称为已验收。决定取消或降级须记录需求评审依据。

# 5. 需求维护与待决事项

## 5.1 需求与设计追踪

需求 ID 长期稳定；同一要求的澄清不重新编号。需求范围、通过条件或优先级改变时，必须同步评估关联 AC 与模块规范。

| 需求范围 | 主责设计文档 | 验证主线 |
|---|---|---|
| FR-REC、FR-DAT | `recorder.md`（包括只读历史索引接口）（包括恢复检查与证据）；`protocol.md` 定义历史查询 API，`server.md` 负责转发和鉴权 | AC-01～10、AC-16/17、AC-22～27 |
| FR-CTL | `protocol.md` 定义 UNKNOWN、当前状态与领域 Evidence 的独立语义；`server.md` 负责协议映射；`recorder.md` 明确可关联的 Topic 操作证据能力；`client.md` 定义 SDK 的 OperationResult/StateView/Evidence 语义；Application 层在实现前补齐展示、退出码、用户确认和再次操作规则，不以刷新结果替代原始请求结果 | AC-11～14、AC-17/18 |
| FR-APP | Application 设计在对应实现前补齐，可采用一份统一文档或按 CLI/TUI/Web 拆分；共享 Client 语义以 `client.md` 为准，不因文档拆分方式改变 | AC-14/15/18、AC-22/23 |
| NFR-PERF、NFR-REL | `requirements.md` 定门槛；Recorder/Server 文档落实机制 | AC-07～17 |
| NFR-SEC | `protocol.md`、`server.md`；浏览器 Session/CSRF 必须在 Web Application 实现前形成明确设计与攻击面验收，不要求预先固定为独立文件 | AC-18 |
| NFR-COMP | 技术选型规范、构建/发布工程规范 | AC-06、AC-19～21 |

架构决策以 `architecture.md` 为准；技术选择与版本以 `system_technology_selection.md` 为准。不要求兼容任何尚未正式发布的历史设计；需求变更直接修改本文及对应的设计/验证用例。

## 5.2 V1.0 范围裁定与发布前待量化事项

| ID | 决策或待决事项 | 阶段 | 结论与后续设计责任 |
|---|---|---|---|
| OPEN-01 | 标准性能负载硬件与 Recorder/Server CPU 统计边界、历史 ≤30% 候选阈值 | 发布验收前量化 | 保留 CPU MUST 门槛；由性能验证规范固定硬件 Profile、测量定义与具体阈值 |
| OPEN-02 | HTTP 控制与 SSE 延迟、订阅/连接数、缓冲上限 | 发布验收前量化 | `protocol.md` / `server.md` 定义定量目标及负载与极限测试 |
| OPEN-03 | 超时后控制操作是否需通用可查询终态 | **已裁定：V1.0 不要求** | 不引入通用 Operation Registry；Topic Add/Remove 可利用 `topic_events.jsonl` 中与请求身份对应的 `operation_id` 及明确终态事件，Start/Stop/Pause/Resume 不要求持久化请求级操作回执。精确关联的 durable Evidence 只能确认相应领域终态/副作用；原始 HTTP `OperationResult` 仍只由该次可信同步回执决定，缺失回执时保持 UNKNOWN。单独时间窗口/序号/当前状态相符不足以建立请求归属。`protocol.md` 约束证据及外部语义，`client.md` 与 Application 设计约束刷新、提示及操作结果展示 |
| OPEN-04 | 远程授权、角色权限、TLS 部署与 Web 会话边界 | **已裁定：V1.0 使用预共享不透明 Bearer Token + read/control scope；远程强制 TLS** | `control` 包含 `read`；Token 仅由受限 Secret 文件/Provider 提供，Runtime 对所有 API/SSE 自行鉴权，不信任 Web Cookie 或代理 Header 代替认证；TLS 可由 Drogon 直接提供或由显式受信代理终止；Web Session/CSRF 独立。正式发布仍须通过 AC-18 攻击/泄漏/证书/代理测试 |
| OPEN-05 | Camera/LiDAR 等大载荷固定吞吐是否阻断 V1.0 | **已裁定：V1.0 不阻断** | 大载荷成功写入数据的 MCAP/Schema/时间戳完整性、接入丢弃计数与已接纳消息 Writer 故障诊断仍为 MUST（AC-27）；独立大载荷固定吞吐、零丢弃与延迟性能不作为 V1.0 发布门槛 |

**当前范围决策：** OPEN-03/04/05 已形成 V1.0 设计裁定，不是不可调整的历史兼容要求；OPEN-01/02 仍是发布前必须量化的性能/容量决策。OPEN-04 的设计已闭合不等于安全准入已通过，远程模式仍必须以 AC-18 的真实攻击与部署验证为发布证据。发现范围、阈值和架构不合理时可直接同步修订需求、模块设计及 AC。测试未通过或范围未明确，不得宣称版本可发布。
