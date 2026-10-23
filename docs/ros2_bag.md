# ros2_bag 通用机器人数据录制工具架构与实施设计

**文档版本：v4.3.3**

**产品版本：V1.0**

**文档状态：FROZEN — 架构及实施合同；Phase 0 技术决策具有强制退出门槛**

**目标平台：ROS 2 Humble / Jazzy × AMD64 / ARM64**

**操作系统：Ubuntu 22.04 / 24.04**

**开发语言：C++17、Python 3.10+、Shell**

**ROS 2 通信中间件：Fast DDS**

**存储格式：MCAP / rosbag2**

**正式 Recorder：`ros2_bag_recorder`**

**正式发布包：`ros2_bag.tar.xz`**

**交付原则：Phase 0–3 全部完成并通过验收后，一次性发布 V1.0；不规划后续功能版本**

---

# 1. 项目定位与需求

## 1.1 项目背景

机器人系统通过 ROS 2 Topic 发布关节状态、运动状态、传感器数据、算法结果及运行诊断信息。

在线可视化工具可以观察实时状态，但在机器人调试、故障定位、现场复现、算法验证和长时间稳定性分析等场景，仍需要高性能、低资源开销、可恢复的数据录制能力。

本项目开发通用工具 `ros2_bag`。

其核心由 C++17 实现，通过 `rclcpp::GenericSubscription` 接收 ROS 2 序列化消息，使用 `rosbag2_cpp` 和 MCAP 进行存储。

CLI 与 Web 使用 Python 标准库实现，通过 UDS 与 Recorder 通信。

### 1.1.1 核心目标

1. 录制标准与自定义 ROS 2 Topic。
2. 动态发现 Topic、消息类型和发布者 QoS。
3. 录制期间动态增加、移除 Topic。
4. 支持普通、定时、滚动三种录制模式。
5. 支持暂停、恢复、停止和异常恢复。
6. 避免 Recorder 新增不必要的完整载荷复制。
7. 将 RecordedMessagePool 作为正式数据路径组件。
8. 为消息、队列、对象池、进程和磁盘提供统计。
9. 严格限制关键资源使用。
10. 支持 MCAP 直接读取和 PlotJuggler 离线分析。
11. 支持 Humble/Jazzy × AMD64/ARM64。
12. 通过 Docker 和 CI 实现四平台可追溯构建。
13. 提供统一预编译压缩包。
14. 不要求用户重新构建 C++ 源码或安装项目 Python 依赖。

## 1.2 设计原则

### 1.2.1 Primitives, not Features

优先复用成熟组件：

- `rclcpp::GenericSubscription`
- `rosbag2_cpp`
- `rosbag2_storage_mcap`
- `moodycamel::BlockingConcurrentQueue`
- `sockpp`
- `nlohmann/json`
- `yaml-cpp`

不自行开发：

- DDS 中间件。
- ROS 2 序列化框架。
- MCAP 编码器。
- 通用任务调度系统。
- 通用 Actor 框架。
- 通用并发队列。
- 通用对象池库。
- 完整日志框架。
- 通用 Web 服务框架。
- 容器编排平台。

RecordedMessagePool 是面向本项目数据路径的专用组件，不扩展为对外公开的通用内存管理库。

### 1.2.2 高频数据路径全部采用 C++

Python、JSON、HTTP、SSE、UDS 不参与逐消息数据传输及存储。

回调路径只执行必要的：

- 消息所有权接收。
- 时间戳捕获。
- 身份与准入检查。
- QueueBudget 申请。
- RecordedMessage 获取。
- 消息包装。
- 非阻塞入队。
- 高频计数更新。

### 1.2.3 单一状态所有权

| 对象或状态 | 所有者 |
|---|---|
| 录制任务状态机 | RecordingCoordinator |
| Topic 订阅生命周期 | TopicManager，受 Coordinator 协调 |
| RecordedMessage 节点 | RecordedMessagePool |
| 消息队列配额 | QueueBudget |
| 当前 MessageQueue | Recording Task |
| ProducerToken | 当前队列及使用线程 |
| rosbag2 Writer | Writer Thread |
| Writer API 适配 | WriterAdapter |
| 资源采样 | ResourceMonitor |
| 磁盘保护状态转换 | RecordingCoordinator |
| status.json | Status Publisher |
| 分片实际删除 | Cleanup Worker |
| Session 资源 | Recorder 进程 |

不得通过其他组件直接修改 Coordinator 的录制状态。

### 1.2.4 正确性优先

必须保证：

1. SerializedPayload 跨回调返回具有合法所有权。
2. QueueBudget 双重配额统一线性化。
3. MessageHandle 和 BudgetLease 释放责任分离。
4. ProducerToken 不跨线程并发共享。
5. 对象池节点不丢失、不重复归还、不 UAF。
6. recording_generation 严格隔离不同任务。
7. topic_epoch 严格隔离订阅实例。
8. Writer 由单线程独占。
9. 操作的接受、生效和终态持久化边界明确。
10. 已接纳消息不因控制操作回滚而被随意清空。
11. 元数据持久化失败能够安全收尾。
12. 崩溃恢复不编造无法确认的成功状态或错误原因。

### 1.2.5 资源有界

必须限制：

- QueueBudget 字节和消息数量。
- RecordedMessagePool 固定节点容量。
- 对象池空闲载荷缓存。
- 队列外在途包装对象。
- 历史快照缓存。
- Writer 控制队列。
- TopicInfo 数量。
- PENDING 操作。
- 幂等终态缓存。
- 状态文件。
- IPC 请求及响应。
- SSE 载荷。
- Web 连接。
- Cleanup 待处理任务。

QueueBudget 不能代替整个 Recorder 的 RSS 上限。

### 1.2.6 构建可追溯

四平台构建必须记录：

- 源代码 SHA。
- 四个 third_party SHA。
- Docker 基础镜像 Digest。
- ROS 2 软件包版本。
- GCC 与 glibc 版本。
- 构建参数。
- ELF 动态依赖。
- 二进制 SHA256。
- 测试平台及测试结果。

### 1.2.7 V1.0 完整交付

Phase 0–3 是内部开发阶段，不是产品版本。

不规划 V1.1/V2.0 等后续功能版本。

已经确定的功能与验收不得以“后续完善”为由推迟。

## 1.3 功能范围

| 功能 | V1.0 正式要求 |
|---|---|
| Topic 发现 | 动态发现名称、类型、发布者及 QoS |
| 标准消息 | 支持 |
| 自定义消息 | 录制端具备类型支持时支持 |
| 数据路径 | GenericSubscription → MessageQueue → WriterAdapter → MCAP |
| 对象池 | 正式架构组件 |
| 动态 Topic | RECORDING/PAUSED 下增删 |
| QoS | 自动适配、显式配置及兼容诊断 |
| Normal | 手动开始、停止 |
| Timed | 单调时钟计时，到期停止 |
| Rolling | 独立分片及时间窗口保留 |
| CLI | 启停、状态、Topic 管理 |
| Web | 控制、状态、SSE |
| UDS | 统一控制协议 |
| Session | 同机实例隔离 |
| 消息统计 | 频率、接收、入队、写入和丢弃 |
| ResourceMonitor | CPU、RSS、线程、FD、系统内存、磁盘 |
| 磁盘保护 | 自动保护暂停、可选自动恢复 |
| 异常恢复 | manifest、事件、分片 |
| PlotJuggler | Humble/Jazzy 交叉分析 |
| Docker | 四平台构建/测试基础设施 |
| CI | 编译、测试、性能及发布验证 |
| 发布 | `ros2_bag.tar.xz` |

### 1.3.1 非目标

不实现：

- ROS 2 Service/Action 录制。
- CycloneDDS。
- iceoryx。
- Agnocast。
- Composable 传输层改造。
- 自研 DDS/IPC 零拷贝。
- 云端或分布式录制。
- 新的通用数据库。
- 完整 ROS 2 运行时打包。
- Kubernetes。
- 自研并行 MCAP 编码器。
- 复杂 Web 用户账户系统。

## 1.4 性能与可靠性指标

### 1.4.1 标准负载

```text
10 Topics × 1000 Hz
10,000 messages/second
持续 10 分钟
```

| 指标 | 目标 |
|---|---|
| 单 Topic 消息损失率 | 小于 1% |
| CPU | 固定参考硬件上，单核归一化目标 ≤30% |
| QueueBudget | 默认 64 MiB / 65536 条 |
| Pool 容量 | 默认 81920 节点 |
| Writer | 单线程独占 |
| 状态更新 | 1 Hz |
| 普通资源采样 | 默认 2 秒 |
| 磁盘采样 | 默认 2 秒 |
| 内存 | 无不受控增长 |
| Stop | 正常路径排空已接纳消息 |
| 对象所有权 | 无 UAF、重复释放或非法访问 |

CPU 必须记录：

- CPU 型号与核数。
- 消息类型。
- 消息尺寸分布。
- 发布频率。
- ROS/RMW/Fast DDS 版本。
- 存储设备。
- 压缩配置。
- MCAP Chunk 参数。
- Executor 线程数。
- Recorder CPU 与系统其他进程 CPU 的区别。

不能承诺任意硬件和任意载荷下无消息丢失。

### 1.4.2 可靠性约束

1. 回调不执行磁盘 I/O。
2. 回调不等待消息队列释放容量。
3. 不持有裸 RMW buffer 跨回调使用。
4. 不超过 QueueBudget。
5. Pool 全局锁不覆盖 Writer I/O。
6. 控制请求不会无限占用 Writer。
7. Writer 写入失败不造成消息所有权泄漏。
8. 状态写入失败不停止磁盘保护。
9. ResourceMonitor 文件系统阻塞不阻塞 Coordinator。
10. 临时运行统计不能替代最终 MCAP 实际可读取数量验证。

## 1.5 MCAP、Schema 与 PlotJuggler

### 1.5.1 分开验收两种兼容性

**PlotJuggler 直接打开 MCAP**

验证离线可视化和字段解析。

**ros2 bag play**

验证 ROS 2 环境中的消息回放。

二者独立，不互相替代。

### 1.5.2 PlotJuggler 2×2

| 录制端 | PlotJuggler 分析端 |
|---|---|
| Humble | Humble |
| Humble | Jazzy |
| Jazzy | Humble |
| Jazzy | Jazzy |

每个组合必须记录：

- ROS_DISTRO。
- PlotJuggler 版本。
- MCAP Loader 插件版本。
- ROS Parser 插件版本。
- rosbag2_storage_mcap 版本。
- 测试文件 SHA256。

### 1.5.3 Schema 合同

必须验证：

- Schema encoding。
- Schema data。
- 顶层消息定义。
- 直接依赖。
- 传递依赖。
- Channel/Schema 关联。
- 实际 CDR 字节与声明格式一致。
- 字段名称和值能够正确解析。

Schema 编码以固定 WriterAdapter/Reader 组合实际支持的 `ros2msg` 或 `ros2idl` 为准。

### 1.5.4 消息支持等级

| 等级 | 定义 |
|---|---|
| 基础录制 | 录制端具备类型支持即可写入 |
| 跨发行版分析 | 指定 PlotJuggler 组合能够解析 |
| 自包含分析 | 分析端没有原自定义消息包仍能解析 |

三个等级不得混为一个“支持”。

### 1.5.5 AC-55 纯自定义依赖闭包

最低验收集合：

```text
ros2_bag_test_msgs/msg/RobotState
    |
    +-- ros2_bag_test_msgs/msg/JointState
    |       |
    |       +-- ros2_bag_test_msgs/msg/MotorState
    |
    +-- ros2_bag_test_msgs/msg/TestTimestamp
```

建议消息定义：

```text
MotorState.msg
  int32 id
  float64 position
  float64 velocity
  float64 effort

JointState.msg
  string name
  ros2_bag_test_msgs/MotorState[] motors
  float64[] calibration_offsets

TestTimestamp.msg
  int32 seconds
  uint32 nanoseconds

RobotState.msg
  ros2_bag_test_msgs/JointState[] joints
  ros2_bag_test_msgs/TestTimestamp stamp
  float64[3] base_position
  bool enabled
```

测试包中所有自定义引用使用 rosidl 合法语法。

最低集合不依赖 `builtin_interfaces`。

### 1.5.6 混合依赖附加集合

另建：

```text
MixedRobotState
   |
   +-- RobotState
   |
   +-- builtin_interfaces/msg/Time
```

用于测试自定义依赖和标准消息依赖混合情形。

其通过不能替代纯自定义闭包通过。

### 1.5.7 无原消息包环境

正式验收步骤：

1. 在录制端构建并安装 `ros2_bag_test_msgs`。
2. 发送包含确定数值、数组和两层嵌套的消息。
3. 使用 Recorder 生成 MCAP。
4. 将 MCAP 复制到独立分析环境。
5. 分析环境不安装 `ros2_bag_test_msgs`。
6. 检查 ROS package index。
7. 检查 AMENT_PREFIX_PATH 和插件查找路径。
8. 使用指定 PlotJuggler 插件打开 MCAP。
9. 验证全部字段和数值。
10. 形成 2×2 测试报告。

必须提供：

```text
tools/verify-clean-analysis-env.sh
```

该脚本至少验证：

```bash
! ros2 pkg prefix ros2_bag_test_msgs
```

并检查测试包不存在于生效的 ament package index 和搜索路径。

仅检查环境变量字符串不够；需要检查真实可解析的包和插件。

### 1.5.8 AC-55 失败决策树

先依次检查：

1. 测试消息是否正确生成。
2. WriterAdapter 是否提取完整定义。
3. MCAP Schema 是否含全部依赖。
4. 指定 PlotJuggler 版本是否识别编码。
5. Parser 是否支持嵌套和数组。
6. 是否存在受支持的替代编码或插件版本。

若问题可以通过 WriterAdapter 修复，则必须修复。

若目标插件不具备能力，则评审更换受支持插件组合。

仍不可行时：

- 由 Tech Lead 与项目负责人共同评审。
- 形成 `docs/ac55_scope_change.md`。
- 明确原合同、失败证据、替代范围与用户影响。
- 更新 AC-55。
- 发布新的文档版本重新冻结。

不得自动将自包含验收降级为“安装消息包后可以解析”。

### 1.5.9 同发行版回放

必须验证：

- Humble 录制 → Humble 回放。
- Jazzy 录制 → Jazzy 回放。

不承诺跨发行版 ros2 bag play。

## 1.6 术语

| 术语 | 定义 |
|---|---|
| Session | Recorder 进程的隔离及资源命名空间 |
| Recorder | C++ 数据录制组件 |
| RecordingCoordinator | 录制状态机与控制操作协调者 |
| TopicManager | Topic 发现与订阅管理 |
| TopicInfo | 任务级不可变 Topic 描述 |
| RecordedMessage | 消息包装对象 |
| SerializedPayload | 具有合法所有权的序列化载荷 |
| RecordedMessagePool | 固定容量消息包装对象池 |
| MessageHandle | RAII 包装对象句柄 |
| QueueBudget | 消息数和载荷字节预算 |
| BudgetLease | 成功获取 QueueBudget 后的 RAII 凭证 |
| ResourceMonitor | 统一进程、系统及磁盘资源采样组件 |
| ResourceSnapshot | 已完成采样的只读资源快照 |
| WriterAdapter | Writer API 和 Schema 适配 |
| Segment | 独立 rosbag2/MCAP 分片 |
| recording_generation | Recorder 内录制任务代际 |
| topic_id | 任务内 TopicInfo 身份 |
| topic_epoch | 订阅实例身份 |
| PENDING operation | 已接纳但尚未达到终态的控制操作 |
| paused_reason | 暂停原因，null/USER/SYSTEM_DISK/SYSTEM_UNKNOWN |
| Topic event | topic_events.jsonl 中的生命周期事件 |
| Durable commit | 达到规定文件/目录持久化步骤的提交 |
| Queue ingress | 允许消息进入当前录制任务的准入边界 |

---

# 2. 系统架构与核心数据路径

## 2.1 总体架构

```text
                         ./ros2_bag
                              |
                     +--------+--------+
                     |                 |
                    CLI               Web
                     |                 |
                     +--------+--------+
                              |
                        bag_ipc.py
                              |
                         UDS / JSON
                              |
      +------------------------------------------------+
      |              ros2_bag_recorder                 |
      |                                                |
      |             RecordingCoordinator               |
      |                       |                        |
      |                  TopicManager                  |
      |                       |                        |
      |               GenericSubscription              |
      |                       |                        |
      |                SerializedPayload               |
      |                       |                        |
      |                  QueueBudget                   |
      |                       |                        |
      |              RecordedMessagePool               |
      |                       |                        |
      |                 QueuedMessage                  |
      |                       |                        |
      |            BlockingConcurrentQueue             |
      |                       |                        |
      |                  Writer Thread                 |
      |                       |                        |
      |                  WriterAdapter                 |
      |                       |                        |
      |                   rosbag2_cpp                   |
      |                       |                        |
      |                      MCAP                      |
      |                                                |
      | ResourceMonitor -> Snapshot -> Status Publisher |
      |         |                                      |
      |         +------> RecordingCoordinator          |
      +------------------------------------------------+
```

### 2.1.1 对象关系

```text
Session
   |
   v
ros2_bag_recorder 进程
   |
   +-- RecordingCoordinator
   +-- TopicManager / ROS Executor
   +-- RecordedMessagePool
   +-- Recording Task
   |      +-- QueueBudget
   |      +-- MessageQueue
   |      +-- WriterAdapter / Writer
   |
   +-- ResourceMonitor
   +-- Status Publisher
   +-- Cleanup Worker
   +-- ControlServer
```

同一 Recorder 进程内有一个 RecordedMessagePool。

Pool 跨任务复用。

QueueBudget、MessageQueue 和 ProducerToken 属于当前 Recording Task。

一个 Recorder 同时最多有一个当前任务。

## 2.2 并发、线程与调度

### 2.2.1 执行上下文

| 线程/执行上下文 | 职责 |
|---|---|
| ROS Executor | GenericSubscription 回调 |
| RecordingCoordinator | 串行化状态变化及控制操作 |
| Writer Thread | 唯一 Writer 所有者、分片及消息写入 |
| ResourceMonitor Worker | 独立轻量采样线程 |
| Status Publisher | 聚合并写入状态文件 |
| Cleanup Worker | 顺序执行已批准删除 |
| ControlServer | UDS 请求接收及应答 |

不创建独立 DiskMonitor。

### 2.2.2 状态修改者

- Coordinator 修改 recording_state。
- TopicManager 修改具体订阅生命周期。
- Writer 修改 Writer 内部状态。
- ResourceMonitor 发布 ResourceSnapshot。
- Status Publisher 写 status.json。
- Cleanup Worker 删除已批准分片。
- Pool 修改自己的 Free-List。
- QueueBudget 修改配额。

### 2.2.3 锁和阻塞规则

1. 不在 QueueBudget 锁内调用消息队列。
2. 不在 Pool 全局锁内执行文件或网络 I/O。
3. 不在 Pool 全局锁内执行大型载荷析构。
4. 不在 Coordinator 状态锁内等待 Writer。
5. 不在 Writer 控制队列锁内写 MCAP。
6. 不在 TopicManager 锁内等待 Executor 退出。
7. 不在状态快照锁内生成 JSON。
8. 不在资源快照锁内调用 statvfs 或读取 /proc。
9. 不在 ResourceMonitor 线程直接调用同步 pause/resume。
10. 不因 Cleanup 文件删除阻塞 Coordinator。

### 2.2.4 Coordinator 调度

Coordinator 必须具备独立于 ResourceMonitor 的定时检查能力。

推荐使用其控制事件循环中的 `std::chrono::steady_clock` 截止时间，而不是增加一个通用定时框架。

定时检查：

- Timed 到期。
- 磁盘样本年龄。
- 系统保护状态。
- 已接纳控制操作超时诊断。

如果 Coordinator 正等待 Writer 响应，不能阻塞整个事件循环。

Writer 请求应通过异步命令/完成事件处理。

ResourceMonitor 永久卡住时，Coordinator 仍能判断磁盘样本过期。

### 2.2.5 Status Publisher 一致性

status.json 是**近似一致快照**。

低频状态与高频原子计数不保证来自完全相同的 CPU 时刻。

要求：

- recording_state 等生命周期状态来自一致的低频快照。
- 高频计数采用原子或受控快照。
- ResourceSnapshot 为最近一次完成采样。
- updated_at_ns 标识本次成功生成的状态时间。
- 不允许客户端假设所有字段严格线性化在同一时刻。

正式任务终态需通过 Coordinator 收尾结果和 manifest 确认，而非仅比较一次 status.json 中几个计数。

### 2.2.6 Executor

默认：

```yaml
executor:
  thread_count: 4
```

采用 MultiThreadedExecutor。

线程数启动时确定。

运行中修改必须重启 Recorder。

Callback Group 数量不等于 Executor Thread 数量。

Phase 3 测试：

- 2 线程。
- 4 线程。
- 6 线程。
- 高频/中频/低频分组。

不根据 Topic 数量直接创建等数量线程。

### 2.2.7 Cleanup Worker

采用：

- 一个工作线程。
- 单线程顺序处理。
- 有界队列，默认容量 16。
- 只执行 Coordinator 授权的清理。

```yaml
cleanup:
  queue_capacity: 16
  warning_timeout_sec: 30
```

30 秒是**诊断超时**，不是系统调用可被安全中断的保证。

删除操作可能在文件系统内阻塞。

因此：

1. Coordinator 不同步等待删除。
2. Cleanup Worker 不持有 Coordinator 锁。
3. 超时后标记 CLEANUP_TIMEOUT。
4. 保留 DELETING 记录。
5. 不在旧删除调用尚可能运行时启动同一路径的并发重试。
6. 下次启动通过 DELETING 恢复。
7. 不能宣称单线程在被阻塞时仍可继续执行下一个删除。

如未来要保证单个删除超时后继续其他删除，必须另行评审隔离执行方式；不在 V1.0 默认引入子进程删除框架。

### 2.2.8 Cleanup 恢复

manifest 记录：

```text
cleanup_pending
```

每项至少：

```text
segment_id
segment_path
deleting_started_at_ns
last_cleanup_error
```

恢复时：

1. 检查 DELETING。
2. 检查目标目录。
3. 目录不存在则按已授权删除完成处理。
4. 目录存在则重新安排删除。
5. 不删除 INCOMPLETE。
6. 不把删除超时本身解释为已经完成。

## 2.3 TopicInfo、QoS 与频率统计

### 2.3.1 Topic 发现

1. 规范化 Topic 名称。
2. 查询 ROS graph。
3. 获取类型。
4. 加载必要类型支持。
5. 查询所有发现的发布者 QoS。
6. 检查并确定订阅 QoS。
7. 创建或复用 TopicInfo。
8. 创建 GenericSubscription。
9. 纳入 Executor。
10. 初始化订阅状态和统计。

### 2.3.2 TopicQos

```cpp
enum class Reliability {
    BestEffort,
    Reliable
};

enum class Durability {
    Volatile,
    TransientLocal
};

enum class History {
    KeepLast,
    KeepAll
};

struct TopicQos {
    Reliability reliability = Reliability::BestEffort;
    Durability durability = Durability::Volatile;
    History history = History::KeepLast;
    std::size_t depth = 10;
};
```

默认：

```yaml
reliability: best_effort
durability: volatile
history: keep_last
depth: 10
```

配置优先级：

```text
显式 Topic QoS
       >
兼容发布者的自动选择
       >
默认 QoS
```

### 2.3.3 QoS 支持边界

V1.0 对外可配置：

- reliability。
- durability。
- history。
- depth。

暂不对外配置：

- deadline。
- lifespan。
- liveliness。
- liveliness lease duration。

如果用户配置中出现不支持的 QoS 字段，返回 INVALID_ARGUMENT，不能静默忽略。

但“不对外配置”不表示这些 QoS 对 ROS 2 匹配没有影响。

TopicManager 必须：

1. 使用明确的默认值创建订阅。
2. 读取实际发布者 QoS。
3. 使用 ROS 2/RMW 的兼容性判断或实际匹配反馈。
4. 诊断 deadline、liveliness、lease duration 等相关不兼容。
5. 对 lifespan 等影响接收数据有效性的策略给出诊断。
6. 不声称只检查 reliability/durability 就验证了全部 QoS。
7. 若选定的订阅 QoS 无法匹配目标发布者，返回 QOS_INCOMPATIBLE 或保持有明确原因的不可匹配诊断。

QoS 自动适配不能通过猜测把未支持字段设置成任意值。

### 2.3.4 RepeatPolicy

```cpp
struct RepeatPolicy {
    bool enabled = false;
    std::size_t max_messages = 64;
    std::size_t max_bytes = 4194304;
};
```

对应：

```yaml
repeat_on_segment:
  enabled: true
  max_messages: 64
  max_bytes: 4194304
```

RepeatPolicy 独立于 QoS。

不把 transient_local 自动解释为必须保存全部历史消息。

### 2.3.5 TopicInfo

```cpp
struct TopicInfo {
    TopicId id;

    std::string name;
    std::string type;
    std::string serialization_format;

    TopicQos qos;
    std::optional<std::string> type_hash;
    std::optional<double> expected_hz;

    RepeatPolicy repeat_policy;
};
```

TopicInfo 任务级不可变。

不包含：

- GenericSubscription。
- Writer 注册状态。
- mutable 频率窗口。
- 生命周期锁。
- 当前 ACTIVE/PAUSED 状态。

### 2.3.6 type_hash

规则：

| 原有 TopicInfo | 新请求 | 结果 |
|---|---|---|
| 已知同标准哈希，相等 | 属性一致 | 允许 |
| 已知同标准哈希，不等 | 类型字符串相同 | INVALID_ARGUMENT |
| 一方哈希未知 | 基础属性一致 | 允许并记录 TYPE_DIAGNOSTIC |
| 双方未知 | 基础属性一致 | 允许 |
| 类型字符串不同 | 任意 | INVALID_ARGUMENT |

不能把缺失哈希伪造成固定默认哈希。

### 2.3.7 身份

- recording_generation：单 Recorder 进程内任务代际。
- topic_id：当前任务内 TopicInfo 身份。
- topic_epoch：订阅实例身份。

同名同定义重新添加可复用 topic_id。

重新创建订阅必须产生新 topic_epoch。

### 2.3.8 actual_hz 时钟来源

使用**回调到达时的本机单调时钟**。

例如：

```cpp
std::chrono::steady_clock::now()
```

不使用：

- 发布者消息头时间戳。
- RMW Source Timestamp。
- recv_timestamp_ns 的墙钟值。

因为 actual_hz 描述 Recorder 实际接收速率。

### 2.3.9 actual_hz 完整窗口

设：

- W = frequency_window_sec。
- t = 当前单调时钟时刻。
- N(t,W) = (t-W,t] 内的接收消息数。

\[
actual\_hz(t)=N(t,W)/W
\]

默认：

```yaml
status:
  frequency_window_sec: 1
```

采用滑动窗口语义，而非固定自然秒桶。

实现可以使用固定时间桶环形缓冲区近似。

推荐桶宽 50 ms 或 100 ms。

高频回调只更新桶计数，不逐消息扫描整个窗口。

### 2.3.10 actual_hz 预热

Topic 开始接收时记录：

```text
first_receive_steady_ns
```

完整窗口之前：

\[
actual\_hz=\frac{N(t,elapsed)}{elapsed}
\]

其中 elapsed 是开始观察至当前时刻的单调时间。

限制：

1. 尚未收到消息时 actual_hz=null。
2. elapsed 为 0 或低于最小有效测量时间时 actual_hz=null。
3. 预热初值不用于 gap。
4. elapsed 达到 W 时切换正式窗口。
5. 预热期间 gap=null。
6. Topic 重新创建订阅后重新开始窗口。
7. PAUSED 期间不生成代表发布端故障的 gap。

### 2.3.11 桶化误差标准

与逐条记录时间戳计算的精确滑动窗口进行 A/B。

合成输入包括：

- 恒定 10 Hz。
- 恒定 100 Hz。
- 恒定 1000 Hz。
- 恒定 2000 Hz。
- 突发和停止。
- 边界附近均匀扰动。

1000 Hz 稳态下：

- 稳态平均相对偏差小于 2%。
- 窗口边界 P95 相对偏差小于 5%。

必须固定参考计算方法、采样间隔及测试序列。

对瞬时零频、极小参考值，不使用分母接近零的相对误差作为唯一判定依据；同时记录绝对误差。

### 2.3.12 gap 三态

JSON 必须显式输出：

```json
"gap": null
```

或者：

```json
"gap": false
```

或者：

```json
"gap": true
```

不允许省略。

含义：

- null：不可评估或未配置 expected_hz。
- false：已评估且当前不满足缺口条件。
- true：已达到缺口条件。

客户端不得将 null 自动解释为 false。

### 2.3.13 gap 判定

只有 expected_hz 非 null 且大于 0 时才参与判断。

低频阈值：

\[
threshold=expected\_hz\times0.5
\]

每个完整评估窗口：

- actual_hz < threshold：低频计数 +1。
- actual_hz ≥ threshold：低频计数清零，gap=false。
- 连续 2 个完整窗口低于阈值：gap=true。

评估节奏默认 1 秒一次。

对重叠滑动窗口，不在同一个评估周期内重复增加连续低频次数。

Topic 未开始、PAUSED、REMOVING 或恢复预热期间 gap=null。

### 2.3.14 Topic 容量

```yaml
topics:
  discovery_timeout_ms: 2000
  allow_empty_topics: false
  max_active_topics: 64
  max_distinct_topics: 256
  recent_removed_instances: 32
```

容量超限返回 RESOURCE_EXHAUSTED。

details 包含具体容量及当前使用数量。

## 2.4 SerializedPayload 所有权和 Phase 0 决策

### 2.4.1 包装对象与载荷内存

必须区分：

**RecordedMessage：** 由 Recorder 控制的固定大小包装对象。

**SerializedPayload：** 实际 CDR 字节载荷。

两者：

- 分配来源不同。
- 内存大小不同。
- 生命周期约束不同。
- 可池化程度不同。

不得为了实现包装对象池而自动新增完整载荷深拷贝。

### 2.4.2 首选模型 A

```cpp
struct RecordedMessage {
    std::uint64_t recording_generation;
    TopicId topic_id;
    std::uint64_t topic_epoch;

    std::shared_ptr<const TopicInfo> topic_info;

    std::shared_ptr<rclcpp::SerializedMessage>
        serialized_message;

    std::size_t payload_bytes = 0;

    std::int64_t recv_timestamp_ns = 0;
    std::int64_t send_timestamp_ns = 0;

    TimestampSource timestamp_source;
    bool is_replayed = false;
};
```

该结构是 Phase 0 首选候选。

必须验证 Humble/Jazzy 的实际回调签名及 WriterAdapter 使用接口。

### 2.4.3 拥有型消息直接传递

GenericSubscription 回调收到拥有型 SerializedMessage 后：

1. 取得 shared_ptr 的合法引用。
2. 封装到 RecordedMessage。
3. 转移进队列。
4. Writer 延迟消费。
5. 所有引用释放后由合法所有者释放实际缓冲区。

不借用裸 buffer 指针。

这条路径叫：

```text
Owned SerializedMessage Forwarding
```

只表示不引入 Recorder 新增完整载荷拷贝。

不表示整个 DDS/RMW 接收过程零拷贝。

### 2.4.4 模型 B

独占拥有型序列化对象。

只有满足：

- 所有权唯一。
- 接口合法。
- allocator 正确。
- WriterAdapter 兼容。
- Humble/Jazzy 一致。

才允许采用。

不能仅凭 release 接口存在就推定 RMW buffer 零拷贝。

### 2.4.5 模型 C

Recorder 自有：

```cpp
std::vector<std::uint8_t>
```

在 A/B 无法满足要求时采用。

需要完整定义：

- 深拷贝时机。
- buffer 生命周期。
- capacity 保留策略。
- 单节点缓存上限。
- 全局缓存预算。
- Writer 释放路径。

### 2.4.6 模型 D

独立分档缓冲区或 slab。

仅在 C 的基准测试证实分配成本为主要瓶颈时采用。

不能在回调退出时重置仍被 Writer 使用的 slab 或 PMR。

必须限制全部缓冲区占用。

### 2.4.7 决策顺序

```text
验证 A
  |
  +-- 成功 → A
  |
  v
验证 B
  |
  +-- 成功 → B
  |
  v
验证 C
  |
  +-- 正确且性能满足 → C
  |
  v
评估 D
```

### 2.4.8 Pool 缓冲区配置

A/B：

```yaml
pool:
  max_cached_payload_bytes: 0
  max_per_node_buffer_bytes: 0
```

C/D：

```yaml
pool:
  max_cached_payload_bytes: 8388608
  max_per_node_buffer_bytes: 16384
```

C/D 的数值只是候选初值。

Phase 0 必须依据消息尺寸、复用收益和 RSS 预算固定。

如果采用单独 slab，必须明确 slab 总容量及其与缓存预算的关系。

### 2.4.9 决策记录

必须提交：

```text
docs/recorded_message_data_model.md
```

包含：

1. 最终数据模型。
2. 精确 C++ 类型。
3. 分配与释放所有者。
4. 回调生命周期。
5. Humble/Jazzy 验证。
6. WriterAdapter 适配。
7. 实际 memcpy 情况。
8. 消息尺寸分布。
9. Pool 管理边界。
10. 配置选择。
11. 失败与回退。
12. 测试日志与源码路径。
13. 不采用其他模型的原因。

末尾必须有：

```text
## 评审记录

评审日期：
评审人：
评审结论：
选定数据模型：
测试日志：
源代码 SHA：
仍需处理问题：
```

Tech Lead 或正式指定技术负责人审查。

通过后成为 Phase 1 唯一数据表示基线。

需要变更时必须重新评审。

## 2.5 RecordedMessagePool

### 2.5.1 正式地位

RecordedMessagePool 是 V1.0 正式组件。

不得取消对象池并作为正式发布方案。

### 2.5.2 主方案

```text
Executor A        Executor B        Writer
    |                 |               |
    v                 v               v
 TLS Cache         TLS Cache       TLS Cache
    |                 |               |
    +-----------------+---------------+
                      |
                Batch Transfer
                      |
                      v
               Global Free-List
                 single mutex
                      |
                      v
               Fixed Node Storage
```

参数：

```yaml
pool:
  enabled: true
  mode: tls
  capacity_factor: 1.25
  local_capacity: 64
  batch_size: 64
```

### 2.5.3 Phase 0 Plan B

如果 TLS/PoolState 生命周期无法在 Phase 0 正确验证：

```yaml
pool:
  enabled: true
  mode: global_mutex
```

使用：

- 固定 Node 存储。
- 一个全局 mutex。
- 一个全局 Free-List。
- MessageHandle。
- 有界回退。
- 同样的 QueueBudget。

不使用 TLS 空闲缓存。

优先牺牲部分性能，不牺牲生命周期正确性。

正式 V1.0 默认模式在 Phase 0 评审后冻结。

不要求正式发布同时维护两个可切换对象池实现；可以仅保留实际选定的生产模式，另一模式作为技术探针。

### 2.5.4 容量计算

\[
capacity=\left\lceil max\_messages\times capacity\_factor\right\rceil
\]

限制：

```text
1.0 <= capacity_factor <= 2.0
max_messages > 0
capacity <= size_t 最大可表示范围
```

默认：

```text
65536 × 1.25 = 81920
```

不是简单截断。

任何溢出或不合法数值返回 INVALID_ARGUMENT。

### 2.5.5 Node 存储

```cpp
struct alignas(64) Node {
    RecordedMessage message;
    Node* next_free = nullptr;
};

std::unique_ptr<Node[]> storage_;
```

初始化一次性分配。

Node 不扩容。

实际固定存储：

```cpp
capacity * sizeof(Node)
```

不能写死 144 字节作为实现依据。

### 2.5.6 MessageHandle

```cpp
class RecordedMessagePool;

struct MessageDeleter {
    RecordedMessagePool* pool = nullptr;
    void* node_cookie = nullptr;

    void operator()(RecordedMessage* message) const noexcept;
};

using MessageHandle =
    std::unique_ptr<RecordedMessage, MessageDeleter>;
```

#### node_cookie

`node_cookie` 是 acquire 时正向传递的 Node 稳定标识。

默认为 Node 指针。

release：

1. 检查 pool。
2. 检查 node_cookie。
3. 验证 Node 属于 Pool。
4. 验证 `&node->message == message`。
5. 验证 Node 处于借出状态。
6. 执行 reset。
7. 归还 Free-List。

不从 RecordedMessage* 反向推测 Node*。

### 2.5.7 句柄组合有效性

| pool | node_cookie | 含义 |
|---|---|---|
| null | null | 合法堆回退对象 |
| 非 null | 非 null | 合法池候选，需要验证 |
| 非 null | null | 非法 |
| null | 非 null | 非法 |

非法组合：

- `internal_error_count++`。
- 记录内部不变量错误。
- 不猜测来源后执行 delete。
- 不将错误对象送回 Free-List。
- 通知 Coordinator 进入安全错误收尾。
- 内部资源安全优先于继续录制。

严重内部状态损坏时不能继续宣称 Pool 可正常复用。

### 2.5.8 acquire

固定策略：

1. TLS 优先。
2. TLS 空时批量 refill。
3. 全局池为空时有界堆回退。

回退使用：

```cpp
new (std::nothrow) RecordedMessage()
```

并要求默认构造 noexcept，或由实现显式捕获构造异常。

分配成功增加 fallback_count。

失败增加 allocation_failure_count。

不得在 `noexcept` 函数中直接执行可能抛出的普通 new。

### 2.5.9 release

必须先：

```cpp
message.serialized_message.reset();
message.topic_info.reset();
```

再操作 Free-List。

昂贵载荷析构在 Pool 全局锁外执行。

之后重置：

- generation。
- topic_id。
- topic_epoch。
- 时间戳。
- payload_bytes。
- timestamp_source。
- is_replayed。

模型 C/D 下执行缓冲区保留或淘汰。

release 不执行文件或网络 I/O。

### 2.5.10 TLS 生命周期

主方案必须通过：

- rclcpp MultiThreadedExecutor 实际线程退出探针。
- PoolState 共享所有权或等价安全机制。
- 线程退出归还。
- 跨线程 release。
- Recorder stop/shutdown。
- ASan/TSan 独立测试。

不能依赖 `owner_->is_alive()` 检查后再访问裸 Pool 指针的竞争路径。

### 2.5.11 节点守恒

仅对池节点：

```text
capacity =
    in_use
  + global_free
  + tls_free
```

堆回退不参与等式。

单独统计：

```text
fallback_in_use
fallback_count
allocation_failure_count
```

### 2.5.12 TLS 统计

主方案维护：

```cpp
std::atomic<std::size_t> tls_free_total;
```

TLS acquire/release 和批量转移正确更新。

```text
available = global_free + tls_free_total
```

运行中不同 atomic 读取可能略有时序差异。

精确守恒在全部 Worker 停止后执行。

### 2.5.13 PoolState 退出

正常关闭：

1. 停止新消息准入。
2. 停止订阅。
3. 等待在途 Producer。
4. 停止 Executor。
5. 排空 Writer。
6. 释放队列和任务级 MessageHandle。
7. 完成 TLS 回收。
8. 验证节点守恒。
9. 销毁 PoolState。
10. 销毁 Node storage。

不允许线程继续使用已经销毁的 PoolState。

### 2.5.14 回退告警

当 fallback_count 在任意滚动 60 秒内增加超过 100 次：

```text
POOL_CAPACITY_WARNING
```

该告警：

- 是性能及容量诊断。
- 不自动认定数据损坏。
- 不直接导致 ERROR。
- 不允许无界回退增长。

### 2.5.15 pool.enabled

生产默认：

```yaml
pool:
  enabled: true
```

`enabled=false` 仅在明确的测试/基准构建中支持。

此时包装对象使用独立堆分配，仍保留：

- MessageHandle。
- QueueBudget。
- BudgetLease。
- 任务隔离。

正式生产运行不依赖该降级模式。

测试构建 status 中：

```json
"pool": {
  "enabled": false,
  "capacity": null,
  "in_use": null,
  "available": null,
  "fallback_count": null
}
```

不伪造一个容量为 0 的已启用 Pool。

### 2.5.16 缓冲区预算

模型 A/B：

```text
cached_payload_bytes = 0
```

模型 C/D：

- 单 Node capacity 有上限。
- 全局空闲 buffer capacity 有上限。
- release 并发预留是原子的。
- acquire 后正确扣减。
- 不依赖 QueueBudget 限制空闲载荷。

## 2.6 QueueBudget、MessageHandle 与队列

### 2.6.1 QueueBudget

默认：

```yaml
queue:
  max_bytes: 67108864
  max_messages: 65536
  batch_size: 256
  wait_timeout_ms: 20
  overflow_policy: drop_newest
```

严格满足：

```text
used_bytes <= max_bytes
used_messages <= max_messages
```

bytes/messages 必须在同一次线性化操作中更新。

默认使用一个短临界区。

不强制双计数器打包 CAS。

### 2.6.2 BudgetLease

只有成功获取 QueueBudget 才创建有效 Lease。

Lease 负责：

- bytes 配额。
- messages 配额。
- 恰好一次 release。

没有成功 acquire 的消息不能释放不存在的预算。

### 2.6.3 QueuedMessage

```cpp
struct QueuedMessage {
    MessageHandle message;
    BudgetLease budget;
};
```

队列：

```cpp
using MessageQueue =
    moodycamel::BlockingConcurrentQueue<QueuedMessage>;
```

Phase 0 在固定 third_party SHA 上验证移动构造、移动赋值、try_enqueue、try_dequeue 和批量接口。

### 2.6.4 队列生命周期

QueueBudget 和 MessageQueue 为任务级。

```text
start → 创建当前任务队列
stop → 停止生产者并排空
     → 全部 Token 失效
     → 销毁当前任务队列
next start → 创建新任务队列
```

不得让旧队列 Token 访问新队列。

### 2.6.5 ProducerToken

在每个实际执行入队的线程首次需要该队列时创建。

与当前队列绑定。

可使用 TLS 管理，但不与进程级 Pool TLS 共享生命周期。

规则：

- 不跨线程并发共享。
- 不每条消息重复创建。
- 当前任务 stop 后失效。
- 队列销毁之前结束所有使用者。
- 新 start 使用新 Token。
- Phase 0 验证队列版本的 Token 生命周期。

### 2.6.6 入队流程

```text
GenericSubscription
       |
       v
接收拥有型 SerializedMessage
       |
       v
检查 Topic ingress / generation / epoch
       |
       v
QueueBudget.try_acquire()
       |
       +-- 失败 -> dropped_capacity
       |
       v
RecordedMessagePool.acquire()
       |
       +-- 失败 -> 归还 BudgetLease
       |            dropped_allocation_failure
       |
       v
创建 QueuedMessage
       |
       v
try_enqueue
       |
       +-- 失败 -> RAII 归还预算与对象
       |
       v
Writer Thread
       |
       v
WriterAdapter.write()
       |
       v
释放 BudgetLease 和 MessageHandle
```

### 2.6.7 队列失败

如果 try_enqueue 失败：

- message 未入队。
- 有效 BudgetLease 归还。
- MessageHandle 归还。
- 增加 dropped_enqueue_failure。
- 不向 Writer 报告已接纳消息。

### 2.6.8 Generation 隔离

正常 Stop→Start 必须：

1. 关闭旧任务准入。
2. 等待旧 Producer。
3. 排空旧任务队列。
4. 关闭 Writer。
5. 确认旧任务不再持有消息。
6. 再启动新任务。

因此正常路径：

```text
dropped_stale_generation = 0
```

如果发现旧 generation 消息进入新任务 Writer：

- 视为任务隔离异常。
- 记录 INTERNAL/GENERATION_VIOLATION。
- 不将其作为日常合法丢帧。
- 阻止写入错误任务。
- 触发安全错误处理。

该计数仅作为防御性诊断。

### 2.6.9 统计

```text
received
enqueued
written

dropped_capacity
dropped_enqueue_failure
dropped_allocation_failure
dropped_paused
dropped_stale_generation

write_errors
```

`written` 表示 Writer API 已接受写入。

最终 MCAP 可恢复消息数另行验证。

### 2.6.10 BudgetLease 测试计数

测试构建中维护：

```text
budget_acquire_count
budget_release_count
budget_double_release_count
budget_invalid_release_count
```

正常收尾：

```text
budget_acquire_count == budget_release_count
budget_double_release_count == 0
budget_invalid_release_count == 0
used_bytes == 0
used_messages == 0
```

仅 used_bytes=0 不足以证明没有重复释放。

## 2.7 动态 Topic 生命周期

### 2.7.1 状态

```text
ABSENT → ADDING → ACTIVE → REMOVING → REMOVED
              |
              +-- ARMED
              |
              +-- FAILED
```

ARMED 表示 PAUSED 下完成订阅准备但尚未开放录制准入。

### 2.7.2 三个关键时间点

1. 接受点：操作被接纳，状态 PENDING。
2. 生效点：Ingress 或订阅实例发生变化。
3. 终态提交点：事件按规定持久化，操作进入 COMPLETED/FAILED。

不能把 PREPARED 当作操作完成。

### 2.7.3 add_topic

RECORDING 下：

1. 接受请求。
2. 验证 Topic 名称、类型和 QoS。
3. 验证容量。
4. 分配 topic_id/topic_epoch。
5. 写入并持久化 TOPIC_ADD_PREPARED。
6. Writer 注册 TopicMetadata。
7. TopicManager 创建订阅。
8. Executor 纳入订阅。
9. 开放 ingress。
10. 持久化 TOPIC_ADDED。
11. 操作 COMPLETED。

PAUSED 下创建 ARMED，不开放 ingress。

### 2.7.4 remove_topic

1. 接受请求。
2. 持久化 TOPIC_REMOVE_PREPARED。
3. 关闭对应 epoch ingress。
4. 等待在途 Producer。
5. 解除 GenericSubscription。
6. 持久化 TOPIC_REMOVED。
7. 操作 COMPLETED。
8. 状态进入 REMOVED。

旧 epoch 已接纳消息允许继续写入。

### 2.7.5 同名重添加

允许同名同定义复用 topic_id。

每次重添加新 topic_epoch。

Writer 已注册 Channel 可以按实际 WriterAdapter 能力复用，但不得混淆订阅 epoch 统计。

### 2.7.6 操作失败

失败发生在副作用前：

- 恢复原状态。
- FAILED。
- 记录原因。

失败发生在部分副作用后：

- 判断 ingress 是否开放。
- 判断是否存在已接纳消息。
- 优先保证所有权和数据安全。
- 必要时进入 ERROR 收尾。

不能主动清空合法数据来恢复表面一致性。

### 2.7.7 元数据失败

终态 topic_events 写入失败：

1. 记录 TOPIC_EVENT_PERSIST_FAILED。
2. 关闭新消息 ingress。
3. 关闭新 Topic 操作。
4. 等待 Producer。
5. 尽可能排空。
6. 尽可能关闭 Writer。
7. 进入 ERROR。
8. 尽力保存诊断。

不能无限重试损坏或不可写的事件文件。

### 2.7.8 空 Topic 集合

任务可以在所有 Topic 移除后继续保持 RECORDING。

Timed 继续计时。

Rolling 保留窗口继续推进。

不持续生成空分片。

可再次添加 Topic。

## 2.8 WriterAdapter 与 Writer

### 2.8.1 WriterAdapter

负责：

- Humble/Jazzy Writer API。
- SerializedBagMessage。
- TopicMetadata。
- MessageDefinition。
- Schema/Channel。
- Writer.open/close。
- 时间戳适配。
- 载荷使用期间的所有权。

不负责：

- QueueBudget。
- Pool。
- Coordinator 状态机。
- Session。
- Web。

### 2.8.2 Writer 独占

rosbag2 Writer 只能由 Writer Thread 调用。

不得在 TopicManager 回调里直接调用 Writer。

### 2.8.3 控制队列

```yaml
writer:
  control_queue_capacity: 64
  max_control_batch: 8
```

命令：

```text
OPEN_SEGMENT
REGISTER_TOPIC
FINALIZE_SEGMENT
CLOSE_WRITER
```

stop/shutdown 保留必要的安全关闭提交路径。

### 2.8.4 公平调度

每轮最多：

- 8 条控制命令。
- 256 条消息。

之后检查下一轮控制与消息状态。

确保：

- 控制不能无限占用 Writer。
- 高频消息不能无限推迟控制。
- 关闭命令有界响应。
- 当前消息写入不可被其他线程强制打断。

### 2.8.5 Stop

1. 关闭 ingress。
2. 等待在途 Producer。
3. 完成已接纳 Topic 操作。
4. 排空消息队列。
5. Writer 处理当前批次。
6. 关闭 Writer。
7. QueueBudget 归零。
8. 任务级 MessageHandle 归还。
9. 更新 manifest。
10. 提交任务终态。

### 2.8.6 Resume Prepare/Commit

PAUSED → RESUMING：

1. 冻结需要恢复的 Topic 集合。
2. 关闭 ingress。
3. 准备新分片。
4. 注册全部 TopicMetadata。
5. 验证 Writer。
6. 所有 Prepare 成功。
7. 一次性开放 ingress。
8. RECORDING。

Prepare 失败不得部分开放 ingress 后报告成功。

---

# 3. 录制模式、MCAP、持久化与磁盘保护

## 3.1 录制目录

```text
recordings/
└── run1/
    ├── manifest.json
    ├── topic_events.jsonl
    ├── segment_000001/
    │   ├── metadata.yaml
    │   └── segment_000001_0.mcap
    └── segment_000002/
        ├── metadata.yaml
        └── segment_000002_0.mcap
```

每个 Segment 是独立 rosbag2 bag。

任务根目录不是单个可直接播放的 bag。

分片序号单调递增。

不覆盖已有 recording_id。

## 3.2 Normal、Timed、Rolling

### 3.2.1 Normal

手动开始和停止。

默认单个分片。

暂停后恢复可以新建分片。

**无消息时的分片创建语义：**

- start 成功后创建任务根目录和 manifest。
- 如果没有任何已接纳数据且无必要的 Topic 注册写入，不必立即产生实际 MCAP 分片。
- 第一条需要持久化的消息、必须完成的 Writer 注册或其他明确的分片操作触发实际分片创建。
- Stop 时没有有效消息，可完成零消息任务。
- 不得伪造存在有效数据的分片。
- 若 Writer 已创建但零消息，必须按实际 API 正确关闭并记录为空分片或完整分片，不删除以掩盖状态。

### 3.2.2 Timed

使用单调时钟。

暂停时间计入总时长。

到期：

- RECORDING → STOPPING。
- PAUSED → STOPPING。

统计：

```text
elapsed_wall_sec
elapsed_active_sec
paused_total_sec
```

### 3.2.3 Rolling

默认：

```yaml
recording:
  segment_duration_sec: 60
  retention_duration_sec: 600
```

按单调经过时间推进窗口。

PAUSED 和空 Topic 集合也推进保留时间。

不连续生成空分片。

只清理 COMPLETE。

## 3.3 MCAP

统一：

```text
storage_id = mcap
serialization_format = cdr
```

默认：

```yaml
compression: "Lz4"
compressionLevel: "Fastest"
chunkSize: 786432
noChunking: false
noMessageIndex: false
noSummary: false
```

默认：

```yaml
storage:
  max_cache_size: 0
```

### 3.3.1 None 压缩模式

允许 start 前选择：

```yaml
compression: "None"
```

Phase 3 验证：

- CPU。
- 磁盘带宽。
- 文件大小。
- RSS。
- 关闭耗时。
- MCAP 可恢复性。
- PlotJuggler 加载结果。

不在录制中自动切换压缩算法。

### 3.3.2 非零缓存

`storage.max_cache_size` 是任务级配置。

运行中的任务不允许修改。

修改在下一次 start 生效；不强制为了该参数重启整个 Recorder。

非零值需要额外评估：

- RSS。
- 写入平滑性。
- Stop 排空。
- 错误恢复。
- 持久化窗口。

### 3.3.3 Chunk 与断电恢复

Chunk 大小不直接等于掉电丢失窗口。

还受：

- 用户态 Writer 缓冲。
- 内核 Page Cache。
- 文件系统提交。
- fsync 策略。
- 存储设备。
- MCAP Index/Summary。

影响。

必须用故障注入测量。

## 3.4 历史快照

默认：

```yaml
repeat_on_segment:
  enabled: true
  max_messages: 64
  max_bytes: 4194304

snapshot:
  max_total_bytes: 16777216
```

单 Topic 和全局预算同时约束。

Writer Thread 独占更新快照。

快照持有 shared_ptr 载荷时，按实际 payload bytes 计入预算。

不按 shared_ptr 对象大小计入。

快照重复写入不增加原始 received。

Topic 移除后不继续主动重复旧 epoch 快照。

## 3.5 分片状态

```text
WRITING → FINALIZING → COMPLETE → DELETING → REMOVED
              |
              v
          INCOMPLETE
```

仅 COMPLETE 可以自动清理。

进入 DELETING 前必须有 durable 状态记录。

INCOMPLETE 不自动删除。

DELETING 目录不存在时：

- 无论因上次清理成功还是第三方删除。
- 在已经确认该路径属于授权 DELETING 的前提下。
- 视为幂等删除完成。
- 在 manifest 记录诊断及最终结果。

## 3.6 manifest、topic_events 与错误

### 3.6.1 manifest

至少：

```text
schema_version
session_id
recording_id
mode
recording_state
timestamp_policy
topic_events_file
last_error
secondary_errors
segments
cleanup_pending
```

### 3.6.2 manifest 原子提交

步骤：

1. 写临时文件。
2. fsync 临时文件。
3. rename 替换。
4. 按需要 fsync 父目录。

rename 完成不必然意味着达到全部断电持久化要求。

### 3.6.3 Topic 事件

事件类型：

```text
TOPIC_ADD_PREPARED
TOPIC_ADDED
TOPIC_ADD_FAILED
TOPIC_ADD_UNCERTAIN

TOPIC_REMOVE_PREPARED
TOPIC_REMOVED
TOPIC_REMOVE_FAILED
TOPIC_REMOVE_UNCERTAIN

TOPIC_EPOCH_DRAINED
TYPE_DIAGNOSTIC
QOS_DIAGNOSTIC
```

示例：

```json
{"event":"TOPIC_ADD_PREPARED","topic_id":3,"topic_epoch":1,"name":"/joint_states","at_ns":1792670400000000000}
{"event":"TOPIC_ADDED","topic_id":3,"topic_epoch":1,"name":"/joint_states","at_ns":1792670400100000000}
{"event":"TOPIC_REMOVE_PREPARED","topic_id":3,"topic_epoch":1,"name":"/joint_states","at_ns":1792670500000000000}
{"event":"TOPIC_REMOVED","topic_id":3,"topic_epoch":1,"name":"/joint_states","at_ns":1792670500100000000}
```

每行一个完整 JSON 对象。

UTF-8。

错误或截断行不得伪造为有效事件。

### 3.6.4 查询

```bash
jq -c 'select(.event == "TOPIC_ADDED")' \
  recordings/run1/topic_events.jsonl
```

### 3.6.5 Durable 定义

Topic 事件：

- 已完整写入事件文件。
- 已达到该事件要求的 fsync 提交点。

manifest：

- 临时文件已 fsync。
- 已 rename。
- 父目录持久化要求已满足。

不把普通日志输出视为 durable Topic event。

### 3.6.6 主次错误

`last_error`：首次导致不可恢复收尾的主故障。

`secondary_errors`：错误收尾期间次生故障，默认最多 8 条。

普通 INVALID_ARGUMENT、INVALID_STATE、NOT_FOUND 不覆盖主故障。

未知进程终止：

```text
UNEXPECTED_TERMINATION
```

不编造 Writer 或磁盘根因。

## 3.7 ResourceMonitor 和磁盘保护

### 3.7.1 合并 DiskMonitor

正式只保留 ResourceMonitor。

负责：

- CPU。
- RSS。
- FD。
- 线程数量。
- 系统可用内存。
- CPU 核数。
- 磁盘总容量。
- 实际可用容量。
- 使用率。
- 采样错误。

不再有：

```text
disk_monitor.hpp
disk_monitor.cpp
```

### 3.7.2 执行上下文

ResourceMonitor 拥有一个独立轻量工作线程。

不与 Status Publisher 共用阻塞采样流程。

磁盘条件通知 Coordinator。

Coordinator 是唯一状态转换者。

### 3.7.3 ResourceSnapshot

```cpp
struct DiskResource {
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::uint64_t> available_bytes;
    std::optional<double> used_percent;

    bool valid = false;
};

struct ResourceSnapshot {
    ProcessResource process;
    SystemResource system;
    DiskResource disk;
};
```

失败时指标为 null，不伪造成 0。

### 3.7.4 CPU 百分比

\[
cpu\_percent=
\frac{\Delta process\_cpu\_seconds}
{\Delta steady\_seconds}\times100
\]

包含所有线程。

可以大于 100%。

首次采样前 CPU=null。

解析 `/proc/self/stat` 时正确处理 comm 字段和 CLK_TCK。

### 3.7.5 资源采样配置

```yaml
resource:
  enabled: true
  sample_interval_sec: 2

disk:
  check_interval_sec: 2
  max_sample_age_sec: 10
  unknown_after_failures: 3
  warning_percent: 80
  pause_percent: 90
  resume_percent: 85
  min_free_gb: 2
  auto_resume: false
  resume_stable_sec: 10
```

规则：

1. ResourceMonitor 单线程。
2. disk.check_interval_sec ≥ resource.sample_interval_sec。
3. disk.check_interval_sec 是 resource.sample_interval_sec 整数倍。
4. 默认每次普通资源采样同时采样磁盘。
5. resource.enabled=false 只关闭普通遥测。
6. 磁盘安全检查仍运行。
7. max_sample_age_sec > disk.check_interval_sec。

### 3.7.6 statvfs

实际可用空间：

```text
f_bavail × f_frsize
```

不是 f_bfree。

采样失败：

- valid=false。
- 保留失败原因。
- 不将剩余空间填为 0。
- 更新时间与最近有效样本时间分开记录。

### 3.7.7 磁盘状态

```text
NORMAL
WARNING
CRITICAL
UNKNOWN
```

CRITICAL：

- used_percent ≥ pause_percent；或
- available_bytes 低于 min_free_gb。

WARNING：

- 未达到 CRITICAL。
- used_percent ≥ warning_percent。

恢复：

- used_percent ≤ resume_percent。
- available_bytes ≥ min_free_gb。
- 稳定持续 resume_stable_sec。

### 3.7.8 UNKNOWN

触发：

- 连续采样失败达到 unknown_after_failures。
- 或最近成功样本年龄超过 max_sample_age_sec。

默认：

```text
3 次失败
或样本过期 10 秒
```

UNKNOWN 与 DISK_FULL 分别诊断。

### 3.7.9 初次采样

Recorder 启动 ResourceMonitor 后，先执行初次磁盘采样。

推荐 READY 前完成初次采样；若失败或超过首次采样允许时间，Recorder 仍可提供控制/状态服务，但 start 必须受到 UNKNOWN 保护限制。

不得在 Coordinator 持有状态锁时同步等待可能永久阻塞的 statvfs。

初次采样成功且磁盘安全才允许开始录制准入。

### 3.7.10 UNKNOWN 保护时序

```text
ResourceMonitor 无有效样本
           |
           v
Coordinator 检测失败次数/样本年龄
           |
           v
DISK_STATE_UNKNOWN
           |
           v
关闭新 ingress
           |
           v
等待在途 Producer
           |
           v
排空已接纳消息
           |
           v
关闭 Writer
           |
           v
PAUSED
paused_reason=SYSTEM_UNKNOWN
```

如果 Writer 无法安全关闭，进入 ERROR。

### 3.7.11 Coordinator 不被阻塞

`statvfs` 可能长期阻塞。

Coordinator 必须根据单调时钟和最近成功样本独立判断过期。

不能依赖 ResourceMonitor 返回。

Coordinator 不能阻塞等待 Writer.close。

使用异步 Writer 完成事件，保证安全事件最终可以被处理。

### 3.7.12 paused_reason

正式枚举：

```text
null
USER
SYSTEM_DISK
SYSTEM_UNKNOWN
```

规则：

- USER：用户主动暂停。
- SYSTEM_DISK：磁盘 CRITICAL 保护暂停。
- SYSTEM_UNKNOWN：磁盘状态不可知保护暂停。
- null：非 PAUSED 状态。

USER 暂停期间磁盘进入 UNKNOWN，不覆盖 USER 暂停原因。

但仍记录 disk.protection_state=UNKNOWN。

磁盘恢复后仍保持 USER PAUSED。

### 3.7.13 自动恢复

仅 SYSTEM_DISK 和 SYSTEM_UNKNOWN 可以在 auto_resume=true 时自动恢复。

必须：

- 重新获得有效且安全磁盘样本。
- 满足 resume_percent。
- 满足 min_free_gb。
- 满足 resume_stable_sec。
- 没有其他阻止恢复的错误。

USER 不自动恢复。

Timed 到期不恢复。

ERROR 不自动恢复。

用户显式 resume 也不能绕过 UNKNOWN 或 CRITICAL。

### 3.7.14 状态文件故障

Status Publisher 写入失败不停止 ResourceMonitor。

磁盘保护独立运行。

状态文件保留上一次完整成功版本。

Web 根据 updated_at_ns 和本地接收时间识别过期。

ResourceMonitor 不直接修改 status.json。

## 3.8 崩溃恢复

步骤：

1. 确认旧 Session 所有者已不活跃。
2. 扫描任务目录。
3. 读取 manifest。
4. 读取 topic_events。
5. 分析 durable 事件。
6. 分析无终态 PREPARED。
7. 检查 MCAP Channel/Schema/消息。
8. 处理 DELETING。
9. 保护 INCOMPLETE。
10. 保存恢复诊断。
11. 新 Recorder 当前状态 IDLE。

Channel 存在不证明 add 已向客户端成功完成。

remove 后出现旧消息不证明 remove 失败。

不得修改 MCAP 伪造操作终态。

---

# 4. 生命周期、UDS、状态与用户界面

## 4.1 Recorder 与 Recording 状态

### 4.1.1 Recorder

```text
STARTING → READY → SHUTTING_DOWN → EXITED
```

### 4.1.2 Recording

```text
IDLE
STARTING
RECORDING
PAUSING
PAUSED
RESUMING
STOPPING
FINALIZING
COMPLETED
ERROR
```

### 4.1.3 控制矩阵

| 命令 | IDLE | RECORDING | PAUSED | COMPLETED | ERROR |
|---|---|---|---|---|---|
| start | 允许 | 拒绝 | 拒绝 | 允许 | 安全收尾后允许 |
| stop | 拒绝 | 允许 | 允许 | 拒绝 | 拒绝 |
| pause | 拒绝 | 允许 | 拒绝 | 拒绝 | 拒绝 |
| resume | 拒绝 | 拒绝 | 受保护条件限制 | 拒绝 | 拒绝 |
| add_topic | 拒绝 | 允许 | 允许 | 拒绝 | 拒绝 |
| remove_topic | 拒绝 | 允许 | 允许 | 拒绝 | 拒绝 |
| status | 允许 | 允许 | 允许 | 允许 | 允许 |
| shutdown | 允许 | 允许 | 允许 | 允许 | 允许 |

### 4.1.4 ERROR → start

只有旧任务完成安全隔离后才允许 start：

- Writer 已关闭。
- 旧订阅不再活动。
- Producer 在途计数归零。
- 旧 MessageQueue 已排空或完成受控错误收尾。
- QueueBudget 已归零。
- 旧任务 MessageHandle 全部释放。
- Pool 状态有效。
- 磁盘状态有效且安全。
- 未遗留会访问旧任务资源的异步操作。

否则 INVALID_STATE。

如果 Pool 内部不变量破坏，不允许在同一 Recorder 进程内启动下一任务。

### 4.1.5 重启

新 Recorder：

```text
recording_state = IDLE
recording_id = null
recording_generation = 0
active_operations = []
last_operation = null
```

生成新 recorder_instance_id。

历史信息保留在旧 manifest。

## 4.2 UDS 控制协议

### 4.2.1 基础

- Unix Domain Socket。
- JSON Lines。
- UTF-8。
- request_id。
- 有副作用命令支持 PENDING。
- 查询命令同步。

命令：

```text
start
stop
pause
resume
add_topic
remove_topic
status
operation_status
shutdown
```

### 4.2.2 PENDING

```json
{
  "request_id": "req-001",
  "state": "PENDING",
  "code": "OK",
  "data": {
    "phase": "QUEUED"
  }
}
```

phase 仅：

```text
QUEUED
EXECUTING
```

TOPIC_ADD_PREPARED 等细节保存在 topic_events，不放入 PENDING phase。

### 4.2.3 COMPLETED

```json
{
  "request_id": "req-001",
  "state": "COMPLETED",
  "code": "OK",
  "data": {
    "topic_id": 3,
    "topic_epoch": 1
  }
}
```

COMPLETED 仅代表成功。

### 4.2.4 FAILED

```json
{
  "request_id": "req-001",
  "state": "FAILED",
  "code": "WRITER_ERROR",
  "error": {
    "msg": "writer failed",
    "details": {}
  }
}
```

执行失败不能伪装为 COMPLETED。

### 4.2.5 命令异步性

有副作用命令：

```text
start
stop
pause
resume
add_topic
remove_topic
shutdown
```

均可 PENDING。

同步：

```text
status
operation_status
```

### 4.2.6 错误码

```text
OK
INVALID_ARGUMENT
INVALID_STATE
NOT_FOUND
SESSION_CONFLICT
TOPIC_NOT_FOUND
TYPE_NOT_FOUND
QOS_INCOMPATIBLE
DISK_FULL
DISK_STATE_UNKNOWN
STORAGE_UNAVAILABLE
WRITER_ERROR
TOPIC_EVENT_PERSIST_FAILED
TIMEOUT
RESOURCE_EXHAUSTED
INTERNAL
```

诊断扩展：

```text
UNEXPECTED_TERMINATION
POOL_CAPACITY_WARNING
CLEANUP_TIMEOUT
STATUS_WRITE_STALE
GENERATION_VIOLATION
```

警告/诊断不必全部成为控制失败码。

### 4.2.7 幂等容量

默认：

```yaml
ipc:
  idempotency_cache_size: 256
  max_pending_operations: 32
```

独立管理：

- PENDING 最多 32。
- 已完成终态缓存最多 256。

PENDING 达到上限，新有副作用请求返回 RESOURCE_EXHAUSTED。

不能因为终态缓存淘汰导致仍在执行的操作丢失身份。

### 4.2.8 request_id

相同 ID、相同请求只执行一次。

相同 ID、不同请求返回 INVALID_ARGUMENT。

客户端超时后可以 operation_status 查询。

不承诺跨 Recorder 重启 exactly-once。

### 4.2.9 请求上限

```yaml
ipc:
  max_request_bytes: 65536
  max_response_bytes: 262144
```

不能截断 JSON。

### 4.2.10 超时

```yaml
ipc:
  timeout_status_sec: 2
  timeout_start_sec: 10
  timeout_pause_sec: 10
  timeout_resume_sec: 10
  timeout_topic_operation_sec: 10
  timeout_stop_sec: 30
  timeout_shutdown_sec: 30
```

超时不代表操作被取消。

## 4.3 Session 和 status.json

### 4.3.1 Session

默认：

```text
session_id = default
```

目录：

```text
$XDG_RUNTIME_DIR/ros2_bag/<session>/
```

文件：

```text
recorder.sock
status.json
recorder.pid
web.pid
session.lock
```

Session 目录权限 0700。

Socket 0600。

同一 Session 最多一个活跃 Recorder。

### 4.3.2 recorder_instance_id

读取：

```text
/dev/urandom
```

获得 16 字节。

编码为：

```text
recorder-<32位十六进制>
```

不依赖 std::random_device 的实现差异。

结合：

- PID。
- Linux 进程启动身份。
- Session 锁。
- UDS 握手。

避免 PID 重用误判。

### 4.3.3 status.json 字段

至少：

```text
schema_version
session_id
recorder_state
recorder_instance_id

recording_state
recording_id
recording_generation
paused_reason
mode
bag_path

updated_at_ns
uptime_ns
elapsed_wall_sec
elapsed_active_sec
paused_total_sec

queue_bytes
queue_messages

active_operations
last_operation
last_error
secondary_errors

topics
topics_removed
topics_removed_total

process
system
disk
```

### 4.3.4 Topic 状态

```json
{
  "topic_id": 3,
  "topic_epoch": 1,
  "name": "/joint_states",
  "type": "sensor_msgs/msg/JointState",
  "state": "ACTIVE",
  "expected_hz": 1000.0,
  "actual_hz": 997.0,
  "gap": false,
  "matched_publishers": 1,
  "received": 100000,
  "enqueued": 100000,
  "written": 100000,
  "dropped_capacity": 0,
  "dropped_enqueue_failure": 0,
  "dropped_allocation_failure": 0,
  "dropped_paused": 0,
  "dropped_stale_generation": 0,
  "write_errors": 0
}
```

gap 始终显式存在。

### 4.3.5 topics_removed

```json
"topics_removed": [
  {
    "topic_id": 3,
    "topic_epoch": 1,
    "name": "/camera/image_raw",
    "type": "sensor_msgs/msg/Image",
    "removed_at_ns": 1792670440000000000,
    "final_state": "REMOVED"
  }
]
```

final_state：

```text
REMOVED
REMOVE_FAILED
```

REMOVED：TOPIC_REMOVED 终态已经确认。

REMOVE_FAILED：移除操作已经失败终止，并且该订阅实例确实退出活动集合。

如果移除失败后 Topic 仍 ACTIVE，则不能进入 topics_removed。

最多保留 recent_removed_instances，默认 32。

完整历史在 topic_events.jsonl。

### 4.3.6 process

```json
{
  "rss_bytes": 104857600,
  "cpu_percent": 18.5,
  "thread_count": 9,
  "fd_count": 24,
  "pool": {
    "enabled": true,
    "capacity": 81920,
    "in_use": 1200,
    "available": 80720,
    "fallback_in_use": 0,
    "fallback_count": 0,
    "allocation_failure_count": 0,
    "internal_error_count": 0,
    "tls_hit_rate": 0.97,
    "cached_payload_bytes": 0,
    "buffer_evict_count": 0,
    "global_lock_ops": 320
  }
}
```

### 4.3.7 system

```json
{
  "memory_available_bytes": 8589934592,
  "cpu_count": 8
}
```

### 4.3.8 disk

```json
{
  "valid": true,
  "total_bytes": 107374182400,
  "available_bytes": 42949672960,
  "used_percent": 60.0,
  "protection_state": "NORMAL",
  "sampled_at_ns": 1792670400000000000,
  "last_attempt_at_ns": 1792670400000000000
}
```

`samped_at_ns` 的正式字段拼写是 `sampled_at_ns`。

含义：

- sampled_at_ns：最近一次成功有效采样时间。
- last_attempt_at_ns：最近一次采样尝试完成的时间；如果调用仍阻塞，则不伪造完成时间。
- valid=false：当前有效性不可确认。
- protection_state=UNKNOWN：保护条件未知。

顶层不保留 disk_free_gb/disk_used_percent。

Web/CLI 从 disk.available_bytes 派生显示单位。

### 4.3.9 updated_at_ns

状态写入成功：

- 新状态文件带本次快照生成时间。
- 原子替换成功后对外生效。

状态写入失败：

- 保留上次完整文件。
- 不修改旧文件 updated_at_ns。
- 记录 STATUS_WRITE_FAILED。
- 失败持续超过 3 个状态更新周期，产生 STATUS_WRITE_STALE 诊断。
- 不停止 ResourceMonitor 和磁盘保护。

旧文件无法更新时，内部诊断无法保证出现在该旧文件中，应通过日志和可用的 UDS 状态等路径尽力报告。

### 4.3.10 状态新鲜度

客户端根据：

- updated_at_ns。
- 当前时间。
- 已知的状态更新周期。
- recorder_instance_id。

判断是否陈旧。

如果状态不可用，不能把所有计数显示为 0。

### 4.3.11 紧凑 JSON

status.json 使用单行紧凑 JSON。

采用：

```text
临时文件 → fsync/必要落盘 → rename
```

Web SSE 直接使用完整 JSON 内容作为 data，而不改变字段和数值。

### 4.3.12 容量

```yaml
status:
  update_hz: 1
  frequency_window_sec: 1
  max_file_bytes: 131072
  max_sse_payload_bytes: 262144
```

128 KiB 状态文件。

256 KiB SSE 单帧上限含协议封装余量。

不静默截断必需字段。

## 4.4 CLI

### 4.4.1 命令

```bash
./ros2_bag recorder

./ros2_bag cli start --topics /joint_states,/odom
./ros2_bag cli start --mode timed --duration 1800
./ros2_bag cli start --mode rolling --retention 600

./ros2_bag cli add-topic /camera/image_raw \
  --type sensor_msgs/msg/Image \
  --expected-hz 30

./ros2_bag cli remove-topic /camera/image_raw

./ros2_bag cli topics
./ros2_bag cli pause
./ros2_bag cli resume
./ros2_bag cli status
./ros2_bag cli stop
```

支持 --session，默认 default。

### 4.4.2 直接 Recorder

`./ros2_bag recorder`：

- 前台启动。
- 不依赖 CLI 生命周期。

### 4.4.3 CLI 管理

`./ros2_bag cli start`：

- 可以管理子进程启动。
- 等待 READY。
- 转发日志。
- Ctrl+C 请求正常停止。

不自动 SIGKILL。

### 4.4.4 退出码

```text
0 成功
1 参数错误
2 连接/Session 错误
3 控制失败
4 超时
5 内部错误
```

更详细错误由 UDS code 给出。

## 4.5 Web/SSE

### 4.5.1 服务

Python ThreadingHTTPServer。

默认：

```text
127.0.0.1:8000
```

### 4.5.2 功能

- Start/Stop。
- Pause/Resume。
- Normal/Timed/Rolling。
- Topic 管理。
- PENDING。
- QueueBudget。
- Pool。
- CPU/RSS。
- 磁盘及 UNKNOWN。
- 错误。

### 4.5.3 状态不可用

当 status.json：

- 不存在。
- 无法读取。
- JSON 无效。
- 过期。
- instance_id 与当前实例不一致。

Web 显示状态不可用。

如曾成功读取，显示：

```text
last_known_updated_at_ns
```

以及错误原因。

不显示伪造的零值。

### 4.5.4 HTTP 状态

| 业务结果 | HTTP |
|---|---|
| 成功同步查询 | 200 |
| 成功完成控制 | 200 |
| 已接受但 PENDING | 202 |
| INVALID_ARGUMENT | 400 |
| NOT_FOUND/TOPIC_NOT_FOUND | 404 |
| INVALID_STATE/SESSION_CONFLICT | 409 |
| QOS_INCOMPATIBLE | 409 |
| RESOURCE_EXHAUSTED | 429 |
| DISK_FULL | 503 |
| DISK_STATE_UNKNOWN | 503 |
| STORAGE_UNAVAILABLE | 503 |
| WRITER_ERROR/INTERNAL | 500 |
| TIMEOUT | 504 |

COMPLETED 不允许搭配 WRITER_ERROR 表示操作失败。

如果查询 operation_status 成功获得一条历史 FAILED 记录，HTTP 200 可以表示查询成功，但其 data 中的原操作必须仍是 FAILED；不能混淆查询结果与原控制操作结果。

### 4.5.5 SSE

```text
event: status
data: {"schema_version":1,...}

```

完整状态单行 JSON。

每条 SSE data 载荷满足 256 KiB 上限。

连接有界，断开安全回收。

### 4.5.6 安全

- 默认回环地址。
- Token。
- Cookie HttpOnly。
- SameSite。
- CSRF。
- 远程访问需要 TLS。
- 不在日志输出 Token。
- 不通过 URL 查询参数携带长期凭据。

## 4.6 日志和进程

### 4.6.1 日志目的地

默认写 stdout/stderr。

Recorder 不默认自行管理日志文件轮转。

轮转交由：

- systemd/journald。
- 容器日志驱动。
- CLI 启动时的外部重定向。
- 用户部署环境。

### 4.6.2 级别

```text
DEBUG
INFO
WARN
ERROR
```

### 4.6.3 格式

结构化单行文本，至少：

```text
time level component message
```

根据事件补充：

```text
session_id
recording_id
request_id
topic
error_code
```

### 4.6.4 高频限制

不逐消息打印 INFO/DEBUG。

容量告警、Writer 故障、Topic 变更等按照聚合或限频策略输出。

### 4.6.5 shutdown

正常：

1. 关闭新请求。
2. 关闭消息准入。
3. 收尾 Writer。
4. 停止线程。
5. 释放 Pool。
6. 关闭 UDS。
7. 清理 Session 资源。

超时：

- 不伪造退出。
- 不自动 SIGKILL。
- 提供诊断。

### 4.6.6 信号

信号处理器只设置停止意图或通知受控执行上下文。

不在信号处理器直接调用 Writer.close、JSON 或复杂析构。

---

# 5. 工程结构、配置、Docker 与发布

## 5.1 源码结构

```text
ros2_bag/
├── CMakeLists.txt
├── package.xml
├── README.md
├── .gitmodules
│
├── ros2_bag
├── bag_cli
├── bag_web
│
├── include/
│   └── ros2_bag/
│       ├── recorder.hpp
│       ├── recorded_message.hpp
│       ├── recorded_message_pool.hpp
│       ├── recording_queue.hpp
│       ├── queue_budget.hpp
│       ├── recording_coordinator.hpp
│       ├── topic_info.hpp
│       ├── topic_manager.hpp
│       ├── resource_monitor.hpp
│       ├── control_server.hpp
│       ├── status.hpp
│       └── writer_adapter.hpp
│
├── src/
│   ├── main.cpp
│   ├── recorder.cpp
│   ├── recorded_message_pool.cpp
│   ├── recording_queue.cpp
│   ├── queue_budget.cpp
│   ├── recording_coordinator.cpp
│   ├── topic_manager.cpp
│   ├── resource_monitor.cpp
│   ├── control_server.cpp
│   ├── status.cpp
│   └── writer_adapter.cpp
│
├── scripts/
│   ├── launch_recorder.sh
│   ├── bag_cli.py
│   ├── bag_ipc.py
│   ├── web_server.py
│   └── status_parser.py
│
├── config/
│   ├── recorder.yaml
│   ├── topics.yaml
│   ├── qos.yaml
│   ├── mcap.yaml
│   └── fastdds/
│       └── profile.xml
│
├── web/
│   ├── index.html
│   ├── app.js
│   └── style.css
│
├── third_party/
│   ├── concurrentqueue/
│   ├── sockpp/
│   ├── json/
│   └── yaml-cpp/
│
├── docker/
│   ├── Dockerfile
│   ├── Dockerfile.runtime
│   └── README.md
│
├── .github/
│   └── workflows/
│       ├── pr-check.yml
│       ├── platform-build.yml
│       ├── integration.yml
│       ├── performance.yml
│       └── release.yml
│
├── cmake/
│   └── platform.cmake
│
├── tools/
│   ├── verify-submodules.sh
│   ├── verify-clean-analysis-env.sh
│   ├── generate-third-party-lock.sh
│   ├── verify-runtime-deps.sh
│   └── package.sh
│
├── tests/
│   ├── msgs/
│   │   └── ros2_bag_test_msgs/
│   │       ├── CMakeLists.txt
│   │       ├── package.xml
│   │       └── msg/
│   │           ├── RobotState.msg
│   │           ├── JointState.msg
│   │           ├── MotorState.msg
│   │           ├── TestTimestamp.msg
│   │           └── MixedRobotState.msg
│   ├── probes/
│   │   ├── serialized_message_ownership.cpp
│   │   ├── queue_move_handle.cpp
│   │   ├── writer_adapter_probe.cpp
│   │   └── pool_tls_lifetime.cpp
│   ├── unit/
│   ├── integration/
│   └── performance/
│
└── docs/
    ├── deployment.md
    ├── recovery.md
    ├── implementation_notes.md
    ├── recorded_message_data_model.md
    ├── phase0_extension_decision.md
    └── ac55_scope_change.md
```

最后两个变更记录文件仅在对应变更实际发生时创建；不存在范围变更时不生成虚构批准记录。

不保留：

```text
disk_monitor.hpp
disk_monitor.cpp
RecordingSession
```

## 5.2 依赖

### 5.2.1 四个独立 Submodule

```text
third_party/
├── concurrentqueue/
├── sockpp/
├── json/
└── yaml-cpp/
```

四个独立 gitlink。

### 5.2.2 依赖清单

| 依赖 | 用途 |
|---|---|
| concurrentqueue | 高性能消息队列 |
| sockpp | UDS |
| nlohmann/json | JSON |
| yaml-cpp | YAML |

### 5.2.3 初始化

```bash
git submodule update --init --recursive
```

### 5.2.4 校验

verify-submodules.sh：

1. 检查四个 gitlink。
2. 检查 .gitmodules。
3. 检查 checkout SHA。
4. 检查未初始化状态。
5. 检查工作树是否修改。
6. 禁止浮动分支替代固定 SHA。

### 5.2.5 链接

sockpp、yaml-cpp 优先静态链接。

ROS 2、Fast DDS 和 MCAP 所需动态库仍来自目标运行环境。

不能把第三方静态链接当作完全独立运行。

## 5.3 统一默认配置

```yaml
recording:
  mode: normal
  segment_duration_sec: 60
  retention_duration_sec: 600
  output_root: ./recordings

executor:
  thread_count: 4

topics:
  discovery_timeout_ms: 2000
  allow_empty_topics: false
  max_active_topics: 64
  max_distinct_topics: 256
  recent_removed_instances: 32

queue:
  max_bytes: 67108864
  max_messages: 65536
  batch_size: 256
  wait_timeout_ms: 20
  overflow_policy: drop_newest

pool:
  enabled: true
  mode: tls
  capacity_factor: 1.25
  local_capacity: 64
  batch_size: 64
  max_cached_payload_bytes: 0
  max_per_node_buffer_bytes: 0

writer:
  control_queue_capacity: 64
  max_control_batch: 8

cleanup:
  queue_capacity: 16
  warning_timeout_sec: 30

storage:
  storage_id: mcap
  max_cache_size: 0
  config_file: config/mcap.yaml

snapshot:
  max_total_bytes: 16777216

resource:
  enabled: true
  sample_interval_sec: 2

disk:
  check_interval_sec: 2
  max_sample_age_sec: 10
  unknown_after_failures: 3
  warning_percent: 80
  pause_percent: 90
  resume_percent: 85
  min_free_gb: 2
  auto_resume: false
  resume_stable_sec: 10

ipc:
  max_request_bytes: 65536
  max_response_bytes: 262144
  timeout_status_sec: 2
  timeout_start_sec: 10
  timeout_pause_sec: 10
  timeout_resume_sec: 10
  timeout_topic_operation_sec: 10
  timeout_stop_sec: 30
  timeout_shutdown_sec: 30
  idempotency_cache_size: 256
  max_pending_operations: 32

status:
  update_hz: 1
  frequency_window_sec: 1
  max_file_bytes: 131072
  max_sse_payload_bytes: 262144
```

### 5.3.1 配置优先级

```text
命令行
  >
用户配置
  >
项目默认配置
  >
代码默认值
```

### 5.3.2 配置校验

必须检查：

1. max_messages > 0。
2. max_bytes > 0。
3. capacity_factor ∈ [1.0,2.0]。
4. ceil 计算不溢出 size_t。
5. batch_size > 0。
6. pool.local_capacity > 0（TLS 模式）。
7. pool.batch_size > 0（TLS 模式）。
8. writer.control_queue_capacity > 0。
9. executor.thread_count > 0。
10. disk.check_interval_sec > 0。
11. resource.sample_interval_sec > 0。
12. disk.check_interval_sec 为资源周期整数倍。
13. max_sample_age_sec > disk.check_interval_sec。
14. unknown_after_failures > 0。
15. warning_percent < pause_percent。
16. resume_percent < pause_percent。
17. min_free_gb ≥ 0。
18. max_file_bytes > 0。
19. max_sse_payload_bytes ≥ max_file_bytes，且足够容纳协议封装。
20. max_response_bytes ≥ max_file_bytes，且足够容纳响应封装。
21. C/D 模型对应正确的非零载荷预算。
22. A/B 模型缓存载荷值为 0。
23. 不支持的 QoS 字段返回 INVALID_ARGUMENT。
24. 不允许非法路径遍历或危险输出目录。
25. Pool 正式禁用仅允许测试构建。

不合法配置拒绝启动或拒绝 start。

不静默改值。

### 5.3.3 配置生效时间

Recorder 启动时固定：

- executor.thread_count。
- Pool 实现模式。
- Pool 全局容量设置。
- Session。

Recording start 时固定：

- 录制模式。
- QueueBudget。
- Writer/MCAP 配置。
- 分片策略。
- SnapshotBudget。
- 任务级磁盘保护参数。

动态 Topic 参数通过专用操作修改。

## 5.4 平台与启动

### 5.4.1 目标平台

| ROS 2 | Ubuntu | 架构 |
|---|---|---|
| Humble | 22.04 | AMD64 |
| Humble | 22.04 | ARM64 |
| Jazzy | 24.04 | AMD64 |
| Jazzy | 24.04 | ARM64 |

### 5.4.2 启动器

入口：

```text
./ros2_bag
```

实际：

```text
scripts/launch_recorder.sh
```

按 ROS_DISTRO、CPU 架构选择对应 Recorder。

### 5.4.3 ROS 环境初始化

如果已正确 source ROS 2：

- 保留已有 AMENT_PREFIX_PATH。
- 不覆盖用户 overlay。

如果尚未 source 基础 ROS 环境：

- 在可明确判定目标 ROS_DISTRO 时 source `/opt/ros/$ROS_DISTRO/setup.bash`。
- 不凭空推断多个安装中的发行版。
- 自定义消息 overlay 必须保留或由用户明确 source。

### 5.4.4 RMW

要求：

```bash
RMW_IMPLEMENTATION=rmw_fastrtps_cpp
```

如未指定，启动器可以明确设置该目标 RMW。

如果显式配置为其他 RMW，拒绝启动并返回诊断。

不能悄悄运行在不支持的 DDS 实现上。

### 5.4.5 动态依赖

构建 CI：

- readelf -d。
- ldd。
- ROS 插件检查。
- 运行测试。

启动器：

- 检查选择的二进制存在且可执行。
- 检查 ROS 2 环境。
- 检查 RMW。
- 检查关键运行依赖可加载。
- 检查 MCAP 插件可用。

不强制使用 24 小时纯时间缓存。

如果使用缓存，必须绑定：

- 二进制 SHA。
- ROS_DISTRO。
- RMW。
- LD_LIBRARY_PATH。
- AMENT_PREFIX_PATH。
- 相关动态库身份。

## 5.5 正式发布包

### 5.5.1 文件名

```text
ros2_bag.tar.xz
```

### 5.5.2 目录

```text
ros2_bag.tar.xz
└── ros2_bag/
    ├── ros2_bag
    ├── bag_cli
    ├── bag_web
    ├── scripts/
    ├── config/
    ├── web/
    ├── docs/
    │   ├── deployment.md
    │   └── recovery.md
    ├── README.md
    ├── LICENSES/
    ├── third_party.lock
    ├── build-manifest.json
    ├── humble/
    │   ├── amd64/
    │   │   └── ros2_bag_recorder
    │   └── arm64/
    │       └── ros2_bag_recorder
    └── jazzy/
        ├── amd64/
        │   └── ros2_bag_recorder
        └── arm64/
            └── ros2_bag_recorder
```

不加 bin 层级。

### 5.5.3 顶层脚本

```text
ros2_bag
bag_cli
bag_web
```

均为 Bash 可执行入口：

```bash
#!/usr/bin/env bash
```

权限：

```text
0755
```

### 5.5.4 快速开始

README 必须包含：

```bash
tar -xJf ros2_bag.tar.xz
cd ros2_bag

./ros2_bag recorder
```

另一个终端：

```bash
./ros2_bag cli start --topics /joint_states,/odom
```

如果 Recorder 已通过直接命令运行，CLI start 必须连接该 Recorder，而不是尝试重复创建同 Session 进程；如 CLI 的 start 子进程管理模式与连接现有 Recorder 模式有区分，需在帮助中说明。

停止：

```bash
./ros2_bag cli stop
```

### 5.5.5 发布文档

必须有：

- README.md。
- docs/deployment.md。
- docs/recovery.md。

项目维护者负责在 Phase 2 撰写初稿，Phase 3 完成实际部署与恢复测试后定稿。

不要求只有 Tech Lead 一人编写。

## 5.6 Docker 与 CI

### 5.6.1 定位

Docker/CI 是 Phase 0 必需基础设施。

Docker 镜像不作为用户正式交付物。

目标：

- 四平台编译。
- 依赖隔离。
- 测试。
- 构建产物追溯。
- 干净环境验收。

### 5.6.2 镜像

基础：

```text
ros:humble-ros-base
ros:jazzy-ros-base
```

目标：

```text
linux/amd64
linux/arm64
```

采用统一参数化 Dockerfile。

Release 固定 Digest 及依赖版本记录。

### 5.6.3 Runner

优先：

| 平台 | Runner |
|---|---|
| Humble AMD64 | ubuntu-22.04 |
| Humble ARM64 | ubuntu-22.04-arm |
| Jazzy AMD64 | ubuntu-24.04 |
| Jazzy ARM64 | ubuntu-24.04-arm |

Phase 0 运行实际作业验证可用性。

Humble ARM64 Runner 不可用：

```text
ubuntu-24.04-arm
    +
Humble Ubuntu 22.04 ARM64 Docker 容器
```

此方案仍为原生 ARM64，不是 QEMU。

备用 self-hosted ARM64。

QEMU 不用于正式性能验收。

### 5.6.4 L1–L4

| 级别 | 触发 | 内容 |
|---|---|---|
| L1 | PR/Push | Humble AMD64 快速编译、单测 |
| L2 | PR/Nightly | 四平台编译、单测 |
| L3 | Nightly/RC | 录制、CLI/Web、DDS、恢复 |
| L4 | 发布前 | 真实硬件性能、PlotJuggler、全量 AC |

### 5.6.5 测试消息包

`tests/msgs/ros2_bag_test_msgs/` 为独立 ROS 2 包。

具有自己的：

```text
CMakeLists.txt
package.xml
msg/
```

主 CMakeLists.txt 不依赖该测试包。

主 package.xml 不添加运行时依赖。

CI L2/L3：

1. 构建测试包。
2. source 测试 install。
3. 构建/测试主包。
4. 运行消息录制测试。
5. 在干净分析环境执行 AC-55。

正式发布不复制测试包。

### 5.6.6 Fast DDS 网络

容器内 Publisher/Recorder 测试。

宿主机 Publisher/容器 Recorder 测试。

可信环境：

```bash
docker run --rm \
  --network host \
  --ipc host \
  -e RMW_IMPLEMENTATION=rmw_fastrtps_cpp \
  -e ROS_DOMAIN_ID=42 \
  <test-image>
```

必须验证：

- Topic 发现。
- 匹配。
- 实际数据接收。
- MCAP 数量。
- QoS。

### 5.6.7 可信 CI 边界

可信指：

- 主仓库受信任分支；或
- 外部贡献代码经负责人审查批准。
- 有关 workflow/docker 修改已完成明确审查。
- 不存在未授权高权限执行路径。

外部 PR 修改 `.github/workflows/` 或 `docker/` 时，不自动运行共享宿主机网络/IPC 测试。

需要人工批准。

不可信 PR 不获得发布令牌和真实机器人访问权限。

### 5.6.8 缓存

| 对象 | 依据 |
|---|---|
| Docker Base | Digest |
| 系统依赖 | Dockerfile/版本 |
| third_party | Git SHA |
| ccache | GCC、架构、头文件 |
| 中间产物 | ROS_DISTRO、架构和源码 |

缓存失效不影响正确构建。

### 5.6.9 third_party.lock

```json
{
  "schema_version": 1,
  "source_commit": "<main-sha>",
  "dependencies": {
    "concurrentqueue": "<sha>",
    "sockpp": "<sha>",
    "json": "<sha>",
    "yaml-cpp": "<sha>"
  }
}
```

从 gitlink 和 checkout 生成。

### 5.6.10 build-manifest.json

记录：

```text
source_commit
build_timestamp
ros_distro
architecture
ubuntu_version
base_image_digest
compiler_version
glibc_version
ros_package_versions
third_party_shas
build_flags
binary_sha256
```

third_party.lock 和 build-manifest.json 的 source_commit 必须一致。

third_party.lock 在四平台内容相同。

build-manifest 按平台记录，并汇总进发布包。

### 5.6.11 Release

```text
RC Commit
   |
   v
 L1/L2
   |
   v
  L3
   |
   v
  L4
   |
   v
锁定 Release SHA
   |
   v
四平台正式构建
   |
   v
依赖/二进制检查
   |
   v
third_party.lock / build-manifest
   |
   v
ros2_bag.tar.xz
   |
   v
干净环境验证
   |
   v
Release
```

默认 GitHub Release。

tools/package.sh 独立生成：

- ros2_bag.tar.xz。
- SHA256SUMS。

也可上传 GitLab 或私有制品仓库。

### 5.6.12 CI 日志与制品保留

默认：

- 失败日志保留 30 天。
- 构建制品保留最近 10 次成功/失败构建中需要追溯的关键记录。
- 受 GitHub Actions 实际保留策略、配额和组织权限限制。
- 关键 Release 构建清单和验收证据随正式发布保留。

不能因清理缓存删除正式 Release 的唯一验收证据。

### 5.6.13 Nightly 性能

```text
10 Topics × 1000 Hz
持续 5 分钟
```

记录：

- CPU。
- RSS。
- P99。
- QueueBudget。
- Pool fallback。
- MCAP 数量。

与固定基线相比超过 15% 触发调查。

托管 Runner 波动不直接作为产品性能失败。

---

# 6. 实施阶段与验收

## 6.1 Phase 0–3

### 6.1.1 Phase 0

目标时间：4 周。

必须建立：

1. CMake/package.xml。
2. 四个独立 third_party。
3. Docker。
4. CI L1。
5. CI L2。
6. Fast DDS 网络实验。
7. Humble/Jazzy 所有权实验。
8. WriterAdapter。
9. MessageHandle 队列探针。
10. Pool TLS/PoolState 探针。
11. AC-55 Schema/插件实验。
12. 数据模型决策记录。
13. 测试消息包。
14. 发布打包骨架。

### 6.1.2 Phase 0 Pool 探针

覆盖：

- MultiThreadedExecutor 真实线程。
- TLS 初始化/销毁。
- PoolState 生命周期。
- 跨线程 acquire/release。
- Executor 停止。
- Writer 停止。
- 节点守恒。
- ASan/TSan。

主方案失败，进入单锁方案评审。

### 6.1.3 Phase 0 失败

失败项必须：

1. 记录证据。
2. 关联受影响的 AC。
3. 分析替代方案。
4. Tech Lead/项目负责人评审。
5. 更新冻结文档。
6. 重新验证。

不能带未解决的 P0 进入依赖它的 Phase 1 工作。

### 6.1.4 Phase 0 超期

超过目标 4 周：

- 不自动视为技术失败。
- 不自动放宽要求。
- 未闭合的 P0 不能绕过。
- 暂停受影响 Phase 1 工作。
- 项目负责人和 Tech Lead 评审。

生成：

```text
docs/phase0_extension_decision.md
```

包含：

1. 未闭合前提。
2. 失败证据。
3. 受影响 AC。
4. 继续、延期、范围变更或暂停决定。
5. 新时间预算。
6. 审批人。

可并行执行不依赖这些 P0 的独立工程任务，但不能提前宣称 Phase 0 通过。

### 6.1.5 Phase 1

完整实现：

- TopicManager。
- GenericSubscription。
- SerializedPayload。
- RecordedMessagePool。
- MessageHandle。
- QueueBudget/BudgetLease。
- MessageQueue。
- RecordingCoordinator。
- WriterAdapter。
- 三种模式。
- 动态 Topic。
- 快照。
- manifest/topic_events。
- ResourceMonitor。
- 磁盘保护。
- Status Publisher。

### 6.1.6 Phase 2

- UDS。
- CLI。
- Web/SSE。
- Session。
- recorder_instance_id。
- 幂等。
- 操作查询。
- Pool/Resource/Disk 展示。
- Release 脚本完善。

### 6.1.7 Phase 3

- 四平台全量验证。
- 真实 ARM64/AMD64。
- 10×1000Hz。
- Pool A/B。
- Writer 故障。
- 磁盘 UNKNOWN。
- Cleanup 阻塞。
- Stop→Start 隔离。
- PlotJuggler 2×2。
- AC-55。
- 最终发布包。

## 6.2 AC-01～AC-68

| ID | 验收项 | 通过要求 |
|---|---|---|
| AC-01 | 四平台 | Humble/Jazzy × AMD64/ARM64 编译、启动 |
| AC-02 | 消息录制 | 标准和自定义消息正确 |
| AC-03 | QoS | 自动适配、覆盖、错误诊断 |
| AC-04 | 时间戳 | 接收/发送时间正确映射 |
| AC-05 | Normal | 开始、停止、完整关闭 |
| AC-06 | Timed | 暂停计时和到期停止 |
| AC-07 | Rolling | 保留窗口、分片和删除 |
| AC-08 | 分片 | 每分片独立可读取 |
| AC-09 | manifest | 原子更新和恢复 |
| AC-10 | 快照 | 单 Topic/全局预算正确 |
| AC-11 | 队列 | MPSC、移动所有权 |
| AC-12 | QueueBudget | 两个配额严格不超限 |
| AC-13 | Generation | 任务间不混写 |
| AC-14 | Stop | 生产者屏障和排空 |
| AC-15 | Writer 故障 | 安全关闭、主次错误 |
| AC-16 | UDS | 所有命令正确 |
| AC-17 | 幂等 | 请求重试不重复副作用 |
| AC-18 | 操作查询 | 超时后可查询终态 |
| AC-19 | Session | 冲突和隔离正确 |
| AC-20 | 状态文件 | Schema、更新、原子替换 |
| AC-21 | CLI | 信号、日志、退出码 |
| AC-22 | Web | 控制、SSE、安全 |
| AC-23 | 磁盘保护 | ResourceMonitor 报告，Coordinator 暂停 |
| AC-24 | Fast DDS | 目标 RMW 和数据链路 |
| AC-25 | 异常恢复 | DELETING/INCOMPLETE |
| AC-26 | Submodule | 四个 SHA 固定 |
| AC-27 | 链接 | 无意外动态依赖 |
| AC-28 | Python | 标准库运行 |
| AC-29 | 发布 | ros2_bag.tar.xz 可用 |
| AC-30 | 资源清理 | 线程/Pool/Socket/Writer |
| AC-31 | Web 安全 | Token/CSRF/TLS |
| AC-32 | ARM64 | 30 分钟与 UDS |
| AC-33 | Topic 动态增删 | RECORDING/PAUSED |
| AC-34 | Topic 统计 | epoch 归属 |
| AC-35 | Topic 隔离 | generation 正确 |
| AC-36 | Topic 幂等 | 重复同名请求 |
| AC-37 | Writer 注册 | Channel/Schema 正确 |
| AC-38 | 并发操作 | pause/resume/stop/切片 |
| AC-39 | 添加失败 | 无错误 ingress/泄漏 |
| AC-40 | CLI/Web | 控制一致 |
| AC-41 | 增删压力 | 至少 3000 次 |
| AC-42 | 空 Topic | 允许正确继续 |
| AC-43 | Topic 事件 | PREPARED/终态 durable |
| AC-44 | 所有者异常 | PID/Session 检查 |
| AC-45 | 自动恢复 | 系统暂停、稳定窗口 |
| AC-46 | 删除恢复 | DELETING 幂等 |
| AC-47 | 同名重添加 | 旧 epoch 消息正确 |
| AC-48 | 零消息 | 无消息 start/stop/分片 |
| AC-49 | PlotJuggler | 2×2 |
| AC-50 | ros2 bag play | 同发行版 |
| AC-51 | MCAP Schema | 编码和依赖闭包 |
| AC-52 | 命名 | 静态检查和编译 |
| AC-53 | ResourceMonitor | 各资源和失败语义 |
| AC-54 | 状态容量 | 最大合法状态完整 |
| AC-55 | 自包含 | 纯自定义依赖闭包 |
| AC-56 | 多 PENDING | 独立容量、顺序 |
| AC-57 | 重启 | 新进程 IDLE |
| AC-58 | 生效点 | 接受/准入/终态 |
| AC-59 | shutdown 超时 | 不伪造退出 |
| AC-60 | 实例身份 | 128 位随机、PID 重用 |
| AC-61 | 事件故障 | 安全 ERROR、保护数据 |
| AC-62 | Coordinator | 类/文件正确 |
| AC-63 | 默认 Session | default |
| AC-64 | 历史错误 | 已知与未知根因区分 |
| AC-65 | 快照成本 | 不逐消息交换状态 |
| AC-66 | 采样干扰 | 10 ms 注入不阻塞数据 |
| AC-67 | 联合恢复 | PREPARED/DELETING/INCOMPLETE |
| AC-68 | 发布结构 | 文件名、层级、文档和脚本 |

### 6.2.1 AC-12

10 个生产者并发申请不同字节数。

注入：

- 成功。
- 配额不足。
- 入队失败。
- Writer 失败。
- Stop。
- generation 切换。

验证：

```text
used_bytes <= max_bytes
used_messages <= max_messages
```

最终归零。

### 6.2.2 AC-23/45/53

必须验证：

- ResourceMonitor 采样磁盘。
- Coordinator 触发保护暂停。
- resource.enabled=false 不关闭磁盘保护。
- USER 暂停不自动恢复。
- UNKNOWN 连续失败和样本过期均触发保护。
- statvfs 卡住不阻塞 Coordinator。
- status.json 写失败不影响磁盘保护。
- 手动 resume 不能绕过不安全磁盘状态。

### 6.2.3 AC-41

至少 3000 次 Topic 增删。

记录：

- RSS。
- FD。
- 线程数。
- TopicInfo 引用。
- Pool in_use。
- 事件文件。
- MCAP Channel/Schema。

不能只依据“未崩溃”判定通过。

### 6.2.4 AC-52/62

```bash
test -f include/ros2_bag/recording_coordinator.hpp
test -f src/recording_coordinator.cpp

grep -q 'class RecordingCoordinator' \
  include/ros2_bag/recording_coordinator.hpp

! grep -RIn 'RecordingSession' include/ src/

test -f include/ros2_bag/resource_monitor.hpp
test -f src/resource_monitor.cpp

test ! -e include/ros2_bag/disk_monitor.hpp
test ! -e src/disk_monitor.cpp
```

还必须编译、链接通过。

### 6.2.5 AC-54

构造：

```text
64 ACTIVE
32 removed
32 PENDING
8 secondary_errors
完整 Pool/ResourceSnapshot
最大允许 Topic 名称
```

验证：

- JSON 完整。
- 状态文件不截断。
- UDS 完整。
- SSE 完整。
- updated_at_ns 有效。

### 6.2.6 AC-55

按 §1.5 的纯自定义闭包执行。

严格排除测试消息包。

核对嵌套字段和值。

### 6.2.7 AC-66

10×1000Hz 负载。

对比：

- 资源采样关闭。
- 资源采样开启。
- 注入 10 ms 采样延迟。

在参考硬件可承载负载下，无因采样导致的异常队列丢弃。

### 6.2.8 AC-67

同时注入：

- PREPARED 无终态。
- DELETING。
- INCOMPLETE。

验证恢复顺序，保护 INCOMPLETE，不伪造终态。

## 6.3 AC-69～AC-80

| ID | 验收 | 标准 |
|---|---|---|
| AC-69 | Pool 容量 | 固定、预算合法 |
| AC-70 | 快路径 | TLS 模式命中无全局锁 |
| AC-71 | 批量转移 | 锁频率/延迟统计 |
| AC-72 | 回退 | 基准负载目标 0 |
| AC-73 | 生命周期 | PoolState/TLS/守恒 |
| AC-74 | RAII | 池/堆/跨线程/异常 |
| AC-75 | A/B | CPU/RSS/P99/分配 |
| AC-76 | 缓冲区 | A/B 不缓存；C/D 有界 |
| AC-77 | 消息所有权 | 回调后延迟消费 |
| AC-78 | 数据决策 | 评审记录 |
| AC-79 | BudgetLease | acquire/release 恰好配对 |
| AC-80 | 额外 memcpy | A 模型无新增完整载荷复制 |

### 6.3.1 AC-73

测试：

- Executor 线程退出。
- Writer 线程退出。
- PoolState 销毁。
- 新任务。
- 多线程跨线程归还。
- Stop/shutdown。

正常最终：

```text
in_use = 0
fallback_in_use = 0
tls_free = 0
global_free = capacity
internal_error_count = 0
```

如果正式选择 global_mutex 模式：

- TLS 项为不适用。
- global_free=capacity。
- 不降低其他所有权验收。

### 6.3.2 AC-74

ASan 与 TSan 独立构建。

验证：

- node_cookie 非法组合。
- 重复释放。
- Pool 外堆对象。
- Writer 异常。
- TopicInfo 释放。
- BudgetLease。
- TLS 析构。

### 6.3.3 AC-75

固定同一 SerializedPayload 模型。

A：无 Pool。

B：有 Pool。

比较：

- Recorder CPU。
- 回调 CPU。
- RSS。
- 分配次数。
- Writer P99。
- QueueBudget 峰值。
- 最终消息数量。

不能把载荷深拷贝差异算成 Pool 收益。

### 6.3.4 AC-77

测试两组：

**所有权纯验证组**

- 使用 ASan。
- 低于队列容量的可持续输入。
- 注入受控 Writer 500 ms 延迟。
- 回调返回后 Writer 消费。
- 比较每条消息内容。
- 不允许 UAF。

**压力验证组**

- 1000 Hz。
- 累计 10 分钟。
- 短时注入 500 ms Writer 延迟。
- 记录 QueueBudget 和丢弃。
- Stop 后对比最终 MCAP。

报告：

```text
total_dropped_capacity
test_delay_active_drops
baseline_dropped_capacity
```

`test_delay_active_drops` 只是与测试延迟同时发生的丢弃，不宣称严格因果。

正式生产 status 不增加测试专用字段。

### 6.3.5 AC-78

记录：

- 最终数据类型。
- 所有权。
- 编译日志。
- Humble/Jazzy 验证。
- 载荷大小。
- WriterAdapter。
- Pool 范围。
- Tech Lead 审查结论。

### 6.3.6 AC-79

每次成功 BudgetLease 申请必须有对应 release。

检查：

```text
budget_acquire_count == budget_release_count
budget_double_release_count == 0
budget_invalid_release_count == 0
used_bytes == 0
used_messages == 0
```

测试所有异常和失败路径。

## 6.4 AC-81～AC-86

| ID | 验收 | 标准 |
|---|---|---|
| AC-81 | Docker | 四平台可构建，Digest 可追踪 |
| AC-82 | CI | 同源码 SHA 四平台编译单测 |
| AC-83 | Fast DDS 容器 | 发现和实际录制 |
| AC-84 | third_party | 四个 gitlink/lock 一致 |
| AC-85 | Release | 同 SHA、干净环境 |
| AC-86 | 真实硬件 | AMD64/ARM64 性能和全部 AC |

### 6.4.1 AC-85

检查：

- 文件名精确为 ros2_bag.tar.xz。
- 顶层 ros2_bag/。
- 三个 Bash 入口 0755。
- Humble AMD64。
- Humble ARM64。
- Jazzy AMD64。
- Jazzy ARM64。
- 无 bin 额外层。
- README。
- deployment/recovery。
- third_party.lock。
- build-manifest。
- SHA256SUMS。
- ELF 依赖。
- 同一 Release SHA。

运行：

```bash
readelf -d humble/amd64/ros2_bag_recorder
ldd humble/amd64/ros2_bag_recorder
```

其他平台在相应目标环境执行检查。

不允许意外指向构建机私有目录。

### 6.4.2 AC-86

正式指定：

- 一台实际部署目标 ARM64 设备。
- 一台固定 AMD64 参考机器。

记录：

- 设备型号。
- CPU。
- 内存。
- 存储。
- 内核。
- ROS 2。
- RMW。
- 测试参数。

GitHub Runner 不能替代真实目标设备的最终性能验收。

## 6.5 AC-87～AC-94

| ID | 新增验收 | 通过标准 |
|---|---|---|
| AC-87 | Pool Plan B | TLS 主方案或经批准的单锁方案保持所有权与守恒 |
| AC-88 | 频率统计 | actual_hz 精度、预热、gap 三态 |
| AC-89 | 磁盘 UNKNOWN | 连续失败、过期、初次采样和暂停恢复 |
| AC-90 | Cleanup 超时 | 不阻塞 Coordinator，不并发重删 |
| AC-91 | Generation 隔离 | 正常 Stop→Start 无旧消息进入新任务 |
| AC-92 | 状态故障 | 写入失败、过期和 Web 不可用展示 |
| AC-93 | Token 生命周期 | 队列重建、线程复用无 UAF |
| AC-94 | 变更追溯 | Phase 0/AC-55 变更与发布证据完整 |

### 6.5.1 AC-87

Phase 0 确定正式 Pool 模式。

主方案通过：

- TLS 生命周期。
- 节点守恒。
- ASan/TSan。
- 压力测试。

主方案失败：

- 单锁方案通过相同正确性测试。
- 记录设计变更。
- 正式冻结选择。
- 不取消 Pool。

### 6.5.2 AC-88

测试：

- 预热。
- 1000 Hz 稳态。
- 窗口边界。
- 突发停止。
- PAUSED。
- expected_hz=null。

通过：

- 规定频率误差。
- gap=true/false/null 正确。
- null 不省略。
- 回调到达时钟为 steady_clock。

### 6.5.3 AC-89

注入：

- 连续 3 次失败。
- 永久阻塞。
- 样本年龄超过 10 秒。
- 启动无有效样本。
- USER 暂停。
- SYSTEM_DISK。
- SYSTEM_UNKNOWN。

验证：

- Coordinator 能独立判定。
- 关闭 ingress。
- 进入 PAUSED 或受控 ERROR。
- 自动恢复条件正确。
- 手动恢复不绕过磁盘安全。

### 6.5.4 AC-90

模拟删除调用阻塞超过 30 秒。

验证：

- Coordinator 仍能响应。
- Writer 不受影响。
- DELETING 保留。
- 不启动同一路径并发删除。
- 重启后根据 durable 记录恢复。

### 6.5.5 AC-91

重复 100 次 Stop→Start：

- 旧 Producer 关闭。
- QueueBudget 归零。
- 旧 MessageHandle 归还。
- 新 Writer 不消费旧消息。

正常：

```text
dropped_stale_generation = 0
```

人为注入违规旧消息：

- 检出 GENERATION_VIOLATION。
- 不写入新任务。
- 安全错误收尾。

### 6.5.6 AC-92

注入：

- status.json 写入失败。
- JSON 损坏。
- 文件消失。
- 文件过期。
- instance_id 改变。

验证：

- 保留上次完整有效文件。
- 客户端识别过期。
- Web 显示不可用。
- 不伪造 0。
- 磁盘保护持续。

### 6.5.7 AC-93

多个任务：

```text
start → stop → start → stop
```

多个 Executor 线程复用。

验证：

- 旧 ProducerToken 不接触新队列。
- 旧队列销毁前无在途 Token 使用。
- ASan/TSan 不报告 UAF。
- 正常消息数量正确。

### 6.5.8 AC-94

检查：

- Phase 0 决策记录。
- 如有延长决策，存在 extension 文档。
- 如有 AC-55 范围变更，存在批准文档。
- 变更后文档重新冻结。
- Release SHA 对应批准规格。
- 构建、测试和发布证据完整。

## 6.6 性能与故障测试

### 6.6.1 标准吞吐

```text
10 Topics × 1000 Hz
10 分钟
```

逐 Topic 对比：

- 发布成功数量。
- Recorder received。
- enqueued。
- written。
- 最终 MCAP 可读取数量。

缺口必须区分来源。

### 6.6.2 ARM64

Humble ARM64、Jazzy ARM64：

```text
10 Topics × 1000 Hz
30 分钟
```

真实目标设备。

### 6.6.3 动态 Topic

至少 3000 次 add/remove。

RSS 每秒采样。

使用连续 5 分钟窗口对 RSS 斜率进行诊断。

若斜率超过：

```text
1 MiB/分钟
```

触发 Heap Profiling 调查。

该阈值是诊断触发条件，不是仅凭一次 RSS 回归就认定泄漏。

### 6.6.4 参数敏感性

| 参数 | 候选 |
|---|---|
| Pool local_capacity | 32 / 64 / 128 |
| Pool batch_size | 32 / 64 |
| Queue batch_size | 256 / 512 / 1024 |
| MCAP chunkSize | 768 KiB / 2 MiB / 4 MiB |
| wait_timeout_ms | 10 / 20 / 50 |
| Compression | Lz4 / None |
| Executor thread_count | 2 / 4 / 6 |
| Callback Group | 集中 / 分层 |
| storage.max_cache_size | 0 / 64 MiB / 256 MiB |

不预设最佳值。

### 6.6.5 SIGKILL 恢复测试

每组：

1. 启动录制。
2. 固定频率发布可编号消息。
3. 运行约 30 秒。
4. `kill -9` Recorder。
5. 重启恢复工具。
6. 检查 manifest。
7. 检查完整可读取 MCAP。
8. 统计最后可恢复消息序号。
9. 与死亡前实际发送、接收数量比较。
10. 重复至少 10 次。

分别测试 LZ4、None 和候选 Chunk。

不能把进程被杀后丢失的全部消息直接认定为 MCAP 格式错误。

### 6.6.6 Writer 与磁盘错误

注入：

- 写入失败。
- Close 失败。
- 磁盘满。
- statvfs 失败。
- status.json 失败。
- manifest 失败。
- topic_events 失败。

检查错误主次归属和收尾。

## 6.7 最终交付

V1.0 必须包含：

1. C++17 Recorder 源码。
2. 录制核心组件。
3. RecordingCoordinator。
4. RecordedMessagePool。
5. MessageHandle/QueueBudget/BudgetLease。
6. WriterAdapter。
7. 三种模式。
8. 动态 Topic。
9. MCAP 分片。
10. manifest/topic_events。
11. ResourceMonitor 和磁盘保护。
12. 状态文件及资源统计。
13. UDS/CLI/Web。
14. Session 管理。
15. Docker。
16. L1–L4 CI。
17. 四平台预编译二进制。
18. AC-55 测试和报告。
19. PlotJuggler 2×2 报告。
20. 真实硬件性能报告。
21. third_party.lock。
22. build-manifest.json。
23. README/deployment/recovery。
24. 数据模型决策记录。
25. 完整 AC-01～AC-94 验收报告。
26. `ros2_bag.tar.xz`。
27. SHA256SUMS。

---

# 7. 冻结、技术风险与发布管理

## 7.1 正式冻结的架构

- 单 Recorder 进程。
- RecordingCoordinator 统一录制状态。
- TopicManager 管理订阅。
- Owned SerializedMessage Forwarding 优先。
- RecordedMessagePool 正式纳入 V1.0。
- Pool 主方案 TLS + Global Free-List。
- 经 Phase 0 评审可采用固定容量单锁方案。
- QueueBudget 与 Pool 分离。
- Writer 单线程独占。
- Topic 操作具有接受、生效和持久化边界。
- MCAP 独立分片。
- manifest/topic_events。
- ResourceMonitor 统一磁盘和进程资源。
- 磁盘 UNKNOWN 保守暂停。
- CLI/Web/UDS。
- Docker/CI。
- 四平台。
- `ros2_bag.tar.xz`。
- V1.0 一次性交付。

## 7.2 Phase 0 必须关闭

1. SerializedPayload 精确类型。
2. Humble/Jazzy 所有权验证。
3. WriterAdapter Schema。
4. Move-only MessageQueue。
5. PoolState/TLS 或单锁正式选择。
6. AC-55 能力。
7. Docker 四平台。
8. Fast DDS 容器网络。
9. 测试消息包构建。
10. Release 依赖闭包。

## 7.3 变更流程

发现技术前提不可行：

1. 提供失败日志。
2. 确认影响的设计章节。
3. 确认影响的 AC。
4. 对比最小可行替代方案。
5. Tech Lead 与项目负责人共同评审。
6. 形成变更记录。
7. 更新文档版本。
8. 重新冻结。
9. 再进行依赖该契约的实施。

不得静默降低要求。

## 7.4 Phase 0 超期

四周为目标，不是允许降低安全与正确性的硬性截止日。

超期生成：

```text
docs/phase0_extension_decision.md
```

记录：

- 未闭合项。
- 原因。
- 测试证据。
- 受影响 AC。
- 延期、变更或暂停决定。
- 新目标日期。
- 负责人及批准人。

## 7.5 AC-55 范围变更

只有确证全部受支持方案不可行后才能讨论范围变化。

必须：

- Tech Lead 与项目负责人共同批准。
- `docs/ac55_scope_change.md`。
- 修改设计和验收。
- 新文档版本。
- 新冻结记录。
- README 明确用户限制。

未批准时 AC-55 原要求有效。

## 7.6 正式发布条件

必须同时满足：

1. Phase 0–3 退出条件全部通过。
2. AC-01～AC-94 适用项通过。
3. 四平台产物同 Release SHA。
4. Docker/依赖记录完整。
5. 真实 ARM64/AMD64 验收。
6. PlotJuggler 2×2。
7. AC-55 或已正式重新冻结的替代支持范围。
8. Pool/QueueBudget/Writer 生命周期正确。
9. 异常恢复通过。
10. `ros2_bag.tar.xz` 干净环境验证。
11. 交付文档与校验文件完整。

---

# 附录 A：版本变更历史

| 版本 | 主要变化 |
|---|---|
| v4.0 | 形成完整 Recorder、Topic、Writer、存储和控制架构 |
| v4.1 | 完善控制操作、命名、资源统计及异常恢复约束 |
| v4.2 | 修正主次错误、事件持久化、AC-55 风险及正式发布结构 |
| v4.2.1 | 明确消息所有权、队列双配额原子语义和 ProducerToken 要求 |
| v4.3 | RecordedMessagePool 纳入正式架构；建立 Phase 0 数据模型决策 |
| v4.3.1 | DiskMonitor 合并 ResourceMonitor；加入 Docker/CI；发布包改名 |
| v4.3.2 | AC-55 纯自定义闭包、频率算法、UNKNOWN 保护及 Phase 0 兜底 |
| **v4.3.3** | **闭合 Pool Plan B、Gap 三态、样本陈旧、Cleanup 阻塞、队列隔离、CI 证据、状态错误、日志和变更流程；新增 AC-87～AC-94** |

本变更表是重要架构决策摘要，不替代对应完整章节。

# 附录 B：关键不变量

## B.1 QueueBudget

```text
0 <= used_bytes <= max_bytes
0 <= used_messages <= max_messages
```

正常任务终止：

```text
used_bytes = 0
used_messages = 0
```

## B.2 Pool

```text
capacity = in_use + global_free + tls_free
```

fallback 不参与该等式。

正常进程关闭：

```text
in_use = 0
fallback_in_use = 0
global_free = capacity
tls_free = 0
internal_error_count = 0
```

## B.3 Task Generation

```text
旧任务完全收尾
    →
允许新任务创建 Writer
```

正常：

```text
dropped_stale_generation = 0
```

## B.4 Topic Epoch

```text
同名 Topic 重添加
    →
新 topic_epoch
```

旧 epoch 已接纳消息可以在旧任务内延后写入，但不能被统计为新 epoch 消息。

## B.5 Writer

```text
只有 Writer Thread 调用 rosbag2 Writer
```

## B.6 磁盘

```text
disk.valid = false
    不等于
available_bytes = 0
```

UNKNOWN 与 DISK_FULL 必须区分。

## B.7 状态文件

```text
写入失败
    →
保留上次完整文件
```

不更新旧文件 updated_at_ns。

## B.8 操作终态

```text
COMPLETED = 成功
FAILED = 失败
PENDING = 未终结
```

## B.9 发布

```text
四平台 binary source_commit
    ==
Release source_commit
    ==
third_party.lock source_commit
    ==
build-manifest source_commit
```

# 附录 C：异常时序索引

| 场景 | 正式处理 |
|---|---|
| QueueBudget 不足 | dropped_capacity，释放已获取资源 |
| Pool 节点耗尽 | 有界堆回退，计数 |
| 堆回退失败 | dropped_allocation_failure |
| 无效 node_cookie | 内部不变量错误，安全收尾 |
| ProducerToken 使用旧队列 | 内部生命周期错误 |
| Writer 写入失败 | 主错误，安全 ERROR 收尾 |
| Topic 终态事件失败 | TOPIC_EVENT_PERSIST_FAILED，停止准入 |
| manifest 写入失败 | 保留旧完整文件，记录故障 |
| disk CRITICAL | 系统保护暂停 |
| disk UNKNOWN | 系统保护暂停 |
| 用户 PAUSED 遇 UNKNOWN | 保留 USER 原因，禁止不安全恢复 |
| ResourceMonitor statvfs 阻塞 | Coordinator 根据样本年龄独立判定 |
| Cleanup 删除阻塞 | 保留 DELETING，不阻塞 Coordinator |
| status.json 写入失败 | 保留旧文件，Web 识别过期 |
| 旧 generation 消息进入新任务 | GENRATION_VIOLATION（正式错误码拼写为 GENERATION_VIOLATION），禁止错误写入 |
| PREPARED 未决恢复 | 诊断，不伪造成功 |
| INCOMPLETE | 不自动删除 |
| Recorder SIGKILL | 恢复旧任务，新 Recorder IDLE |
| shutdown 超时 | 不伪造 EXITED，不自动 SIGKILL |
| AC-55 无法通过 | Phase 0 范围变更评审 |
| TLS Pool 不安全 | 采用经评审的单锁固定 Pool |
| CI Runner 不可用 | 同架构 Runner + 正确 Docker 环境 |
| Release SHA 不一致 | 拒绝发布 |

---

**文档版本：v4.3.3**

**产品版本：V1.0**

**状态：FROZEN — 架构与实施合同**

**注意：文档冻结不表示 Phase 0 技术验证已经实际通过；所有强制验证仍必须按文档执行。**

**文档结束。**
