> **CURRENT STATUS: HISTORICAL (SUPERSEDED).** 该 v4.3.8 文档保留为 Recorder 和旧 UDS/CLI/Web 架构的历史依据；不再作为当前开发实施合同。Recorder Only 收尾期间不新建设计文档。

# ros2_bag 通用机器人数据录制工具架构与实施设计

**文档版本：v4.3.8**

**产品版本：V1.0**

**文档状态：FROZEN — Phase 1 Implementation & Execution Contract**

**目标平台：ROS 2 Humble / Jazzy × AMD64 / ARM64**

**目标系统：Ubuntu 22.04 / Ubuntu 24.04**

**开发语言：C++17、Python 3.10+、Bash**

**通信中间件：Fast DDS**

**存储格式：rosbag2 / MCAP**

**正式录制程序：`ros2_bag_recorder`**

**正式发布制品：`ros2_bag.tar.xz`**

**开发阶段：Phase 0 → Phase 1 → Phase 2 → Phase 3**

**发布原则：全部正式发布门槛满足后，一次性发布 V1.0**

---

# 0. 文档定位、修订范围与冻结原则

## 0.1 文档定位

本文是 `ros2_bag` V1.0 的完整架构、接口、实现约束、异常处理及验收规范。

v4.3.8 以 v4.3.7 为完整保留基线，修复新发现的 17 项问题，并补齐相关跨章节约束。

本版继续保留：

1. Recorder 总体架构和组件职责。
2. TopicInfo、TopicQos、RepeatPolicy。
3. SerializedPayload 模型选择及 G0 签审。
4. RecordedMessagePool、PoolState、MessageHandle。
5. Pool TLS 主方案及 global_mutex Plan B。
6. QueueBudget、BudgetLease 和 ProducerToken。
7. WriterAdapter 与 Writer 独占模型。
8. Normal、Timed、Rolling。
9. 动态 Topic、Topic Epoch 和 Generation。
10. MCAP 独立分片。
11. manifest、topic_events 和恢复。
12. ResourceMonitor、磁盘保护与自动恢复。
13. RecordingCoordinator 命令—状态矩阵。
14. ERROR 后重新 start 的准入条件。
15. UDS PENDING/COMPLETED/FAILED 协议。
16. `topics[]`、`topics_removed[]`、`process`、`system`、`disk` 完整状态结构。
17. actual_hz、gap 与状态新鲜度。
18. CLI、Web、SSE 和 Session。
19. Docker、CI、四平台构建和发布。
20. G0–G3。
21. AC-01～AC-94。
22. C++17 代码风格要求。

本版不减少既定 V1.0 功能范围，不将必需功能推迟到其他版本。

## 0.2 v4.3.8 主要修订

本轮新增或强化：

- 固定有界 `fallback_registry`。
- 明确回退对象申请、登记、释放及异常回滚。
- 将回退登记表纳入 PoolState 停止守恒。
- 明确 `std::abort()` fail-fast。
- 区分可恢复运行时错误与不可恢复内部不变量错误。
- 固定全部过渡状态下的控制操作矩阵。
- 明确 RESUMING → Stop 和 STOPPING 重复 Stop 的特殊规则。
- 明确 FAILED 响应及客户端解析。
- 固定回退上限的初始依据与性能验证。
- 补全 AC-48 空分片测试构造。
- 补全 AC-92 Recorder 实例变化。
- 明确 G0 `APPROVED_WITH_CONDITIONS`。
- 明确 G3 发布评审文件及批准条件。
- 明确空 MessageHandle 语义。
- 明确 CLI 连接与启动输出。
- 明确磁盘采样与普通资源遥测的独立性。

## 0.3 FROZEN 的含义

必须区分：

**架构合同冻结**

模块边界、状态机、所有权、错误语义、接口格式、资源保护和验收范围已经明确。

**技术选择批准**

SerializedPayload 模型 A 和 Pool 工作模式仍以 G0 正式决策为准。

**实际验证结果**

测试通过与否必须以真实日志和环境证据为准。

使用以下三类状态：

| 维度 | 合法状态 |
|---|---|
| Test Status | PASS / FAIL / BLOCKED / NOT RUN |
| Decision Status | APPROVED / PENDING / REJECTED |
| Admission Status | ALLOWED / BLOCKED |

例如：

```text
SerializedPayload probe = PASS
SerializedPayload decision = PENDING
Phase 1 data-path admission = BLOCKED
```

文档标注 FROZEN 不会自动将技术决策标记为 APPROVED。

## 0.4 G0–G3 定义

| 门槛 | 主要目的 | 通过后 |
|---|---|---|
| G0 | 核心技术可行性、方案批准及风险处置 | 允许实施正式数据路径 |
| G1 | Recorder 核心功能、所有权及并发验证 | 允许进入 Phase 2 |
| G2 | CLI/Web/Session 与 ARM64 编译链接 | 允许进入 Phase 3 |
| G3 | 四平台完整验收与正式发布 | 允许发布 V1.0 |

ARM64 原生性能与完整四平台远端 CI 不作为 G0 的前置条件。

ARM64 编译链接是 G2 强制条件。

四平台正式运行和 ARM64 原生性能是 G3 强制条件。

## 0.5 V1.0 发布前兼容原则

当前项目尚未正式发布 V1.0。

因此：

1. 可以调整尚未发布的内部实现。
2. 可以移除历史试验接口。
3. 新配置项必须提供明确默认值。
4. 当前仓库受支持的配置文件必须能正确解析。
5. 外部协议采用 schema_version。
6. 不要求兼容所有旧开发版本。
7. 发布后的破坏性接口变更必须遵循版本管理。

---

# 1. 项目定位、功能范围与性能目标

## 1.1 项目背景

机器人系统通过 ROS 2 Topic 发布：

- 关节位置、速度、力矩、电流。
- 底盘状态及里程计。
- 机械臂、夹爪、升降机构状态。
- IMU、LiDAR、Camera。
- 运动控制反馈。
- 定位和感知结果。
- 诊断、报警和故障信息。

实时可视化用于观察当前数据，但故障复现、算法分析和性能评估需要可靠的离线录制。

`ros2_bag` 以现有 ROS 2 序列化、rosbag2 和 MCAP 为基础，提供独立录制工具。

## 1.2 核心目标

1. 支持标准和自定义 Topic。
2. 动态发现 Topic 类型和发布者。
3. 自动适配 QoS。
4. 动态增加、移除 Topic。
5. 支持 Normal、Timed、Rolling。
6. 支持 Pause、Resume、Stop。
7. 保存有效 MCAP Schema/Channel。
8. 避免 Recorder 新增不必要的完整 payload 复制。
9. 使用固定对象池和有界回退。
10. 使用双配额 QueueBudget。
11. 保证 Generation/Topic Epoch 隔离。
12. 提供完整消息统计。
13. 提供 CPU、RSS、线程、FD、磁盘监控。
14. 提供磁盘危险和 UNKNOWN 保护。
15. 提供 UDS、CLI、Web、SSE。
16. 支持异常恢复。
17. 支持 PlotJuggler 跨发行版分析。
18. 支持 Humble/Jazzy × AMD64/ARM64。
19. 提供正式预编译压缩包。

## 1.3 设计原则

### 1.3.1 Primitives, not Features

优先使用：

| 组件 | 用途 |
|---|---|
| rclcpp | ROS 2 订阅及 Executor |
| rosbag2_cpp | Writer |
| rosbag2_storage_mcap | MCAP |
| concurrentqueue | 并发队列 |
| sockpp | UDS |
| nlohmann/json | JSON |
| yaml-cpp | YAML |
| Python 标准库 | CLI、Web |

不自行实现：

- DDS。
- ROS 2 序列化。
- MCAP Encoder。
- 通用 Actor Runtime。
- 通用对象池框架。
- 通用任务调度平台。
- 分布式 Recorder。
- 云存储后端。

### 1.3.2 数据与控制分离

高频数据路径：

```text
ROS 2
  → GenericSubscription
  → Owned SerializedMessage
  → QueueBudget
  → RecordedMessagePool
  → QueuedMessage
  → MessageQueue
  → Writer Thread
  → WriterAdapter
  → MCAP
```

控制路径：

```text
CLI / Web
    → UDS
    → RecordingCoordinator
```

Python、HTTP、JSON 和 SSE 不参与逐消息写入。

### 1.3.3 所有权清晰

| 对象 | 生命周期 |
|---|---|
| RecordingCoordinator | Recorder 进程 |
| TopicManager | Recorder 进程 |
| PoolState | 进程级共享状态 |
| RecordedMessagePool | 进程级控制接口 |
| MessageHandle | 单条消息 |
| QueueBudget | Recording Task |
| BudgetLease | 单条消息配额 |
| MessageQueue | Recording Task |
| ProducerToken | 任务队列及生产者线程 |
| WriterAdapter | Recording Task |
| Writer Thread | Recording Task |
| ResourceMonitor | Recorder 进程 |
| Cleanup Worker | Recorder 进程 |
| Session | Recorder 进程 |

### 1.3.4 正确性优先级

```text
消息所有权与生命周期
       >
Recording Generation / Topic Epoch 隔离
       >
持久化与异常恢复
       >
资源边界
       >
可观测性
       >
性能微优化
```

### 1.3.5 高频路径约束

GenericSubscription 回调不得直接：

- 调用 MCAP Writer。
- 执行文件 I/O。
- 生成 JSON。
- 执行 HTTP 或 UDS。
- 等待 Pool 节点归还。
- 等待 QueueBudget 空位。
- 执行磁盘采样。
- 持有长时间全局锁。

资源不足时采用受控失败和丢弃。

## 1.4 V1.0 功能范围

| 功能 | 要求 |
|---|---|
| Topic 动态发现 | 必需 |
| 标准 ROS 2 消息 | 必需 |
| 自定义 ROS 2 消息 | 必需 |
| GenericSubscription | 必需 |
| QoS 自动适配 | 必需 |
| 动态 Topic | 必需 |
| Normal/Timed/Rolling | 必需 |
| Pause/Resume/Stop | 必需 |
| RecordedMessagePool | 必需 |
| QueueBudget | 必需 |
| MCAP 独立分片 | 必需 |
| Snapshot | 必需 |
| manifest/topic_events | 必需 |
| ResourceMonitor | 必需 |
| 磁盘保护 | 必需 |
| UDS/CLI/Web | 必需 |
| Session | 必需 |
| PlotJuggler 2×2 | 必需 |
| Docker/CI | 必需 |
| 四平台二进制 | 必需 |

### 1.4.1 非目标

V1.0 不实现：

- ROS 2 Service/Action 录制。
- CycloneDDS。
- 自研 DDS/SHM。
- 分布式录制。
- 云端存储。
- 自定义 MCAP 编码框架。
- 通用对象池库。
- Kubernetes。
- 完整 ROS 2 Runtime 打包。

## 1.5 性能目标

标准负载：

```text
10 Topics × 1000 Hz
10,000 messages/s
持续 10 分钟
```

| 指标 | 目标 |
|---|---|
| 单 Topic 消息损失率 | 小于 1% |
| Recorder CPU | 固定参考负载单核归一化目标 ≤30% |
| QueueBudget | 64 MiB |
| Queue 消息数量 | 65536 |
| Pool 默认固定节点 | 81920 |
| Writer | 单线程 |
| status.json | 1 Hz |
| ResourceMonitor | 默认 2 秒 |
| 磁盘检查 | 默认 2 秒 |
| 正常 Stop | 排空已接纳消息 |
| 所有权 | 无 UAF、双重释放或悬空访问 |
| RSS | 无不受控长期增长 |

### 1.5.1 固定性能负载

正式基准：

```text
tests/performance/standard_profile.json
```

必须记录：

- Topic 名称与类型。
- 实际序列化 payload 字节数。
- 消息内容生成规则。
- 发布频率。
- 发布者数量。
- Executor 线程数。
- ROS 2/Fast DDS 版本。
- CPU、内存、存储设备。
- MCAP 压缩配置。
- Recorder 与发布者 CPU。

不能将小型状态消息的 CPU 目标推广到任意大型图像或点云。

大型载荷单独建性能配置。

### 1.5.2 RSS 预算

整体 RSS 包括：

```text
ROS 2 / Fast DDS
+ Executor / 线程栈
+ Queue
+ Pool Node Storage
+ SerializedPayload
+ Snapshot
+ Writer / MCAP
+ Topic 和控制结构
+ 运行时余量
```

QueueBudget 64 MiB 不表示 Recorder RSS 上限是 64 MiB。

G1 必须实测：

```cpp
sizeof(RecordedMessage)
sizeof(Node)
sizeof(MessageDeleter)
sizeof(MessageHandle)
sizeof(QueuedMessage)
```

### 1.5.3 RSS 泄漏诊断

每秒采样 RSS。

使用连续 5 分钟滑动窗口进行线性回归。

当增长斜率超过：

```text
1 MiB/min
```

触发 Heap Profiling 和泄漏调查。

这是诊断门槛，不是仅凭短期 RSS 变化直接判定泄漏。

---

# 1.6 MCAP、Schema 与 PlotJuggler

## 1.6.1 兼容性等级

| 等级 | 含义 |
|---|---|
| 基础录制 | 录制端可以正确保存 |
| 跨发行版分析 | 固定插件组合可以解析 |
| 自包含分析 | 分析端没有原自定义消息包仍能解析 |

三个等级分别验收。

## 1.6.2 PlotJuggler 2×2

| 录制端 | 分析端 |
|---|---|
| Humble | Humble |
| Humble | Jazzy |
| Jazzy | Humble |
| Jazzy | Jazzy |

每个组合记录：

- ROS 2 发行版。
- PlotJuggler 版本。
- MCAP Loader。
- ROS Parser。
- rosbag2_storage_mcap。
- Schema encoding。
- MCAP SHA256。
- 实际解析字段和值。

## 1.6.3 AC-55 纯自定义消息闭包

```text
RobotState
  |
  +-- JointState
  |      |
  |      +-- MotorState
  |
  +-- TestTimestamp
```

测试消息包：

```text
tests/msgs/ros2_bag_test_msgs/
```

**MotorState.msg**

```text
int32 id
float64 position
float64 velocity
float64 effort
```

**JointState.msg**

```text
string name
ros2_bag_test_msgs/MotorState[] motors
float64[] calibration_offsets
```

**TestTimestamp.msg**

```text
int32 seconds
uint32 nanoseconds
```

**RobotState.msg**

```text
ros2_bag_test_msgs/JointState[] joints
ros2_bag_test_msgs/TestTimestamp stamp
float64[3] base_position
bool enabled
```

消息类型引用必须符合目标 rosidl 实际支持的语法。

Humble/Jazzy 均需构建验证。

附加 MixedRobotState 测试：

```text
MixedRobotState
  |
  +-- RobotState
  +-- builtin_interfaces/msg/Time
```

混合闭包通过不能代替纯自定义闭包通过。

## 1.6.4 自包含分析

录制端：

1. 构建并安装测试消息包。
2. 发布确定的嵌套消息。
3. 写入 MCAP。
4. 记录 Schema/Channel。
5. 计算文件 SHA256。

分析端：

1. 不安装 ros2_bag_test_msgs。
2. 不挂载录制端源码工作区。
3. 检查 ROS Package Index。
4. 检查 AMENT_PREFIX_PATH。
5. 检查插件路径。
6. 使用实际 MCAP Loader。
7. 使用实际 ROS Parser。
8. 校验嵌套结构、数组、字符串、时间戳和数值。
9. 执行四个组合。
10. 保存日志和版本信息。

Phase 0 已有实验证据可以作为 G0 依据，但 G3 应使用最终正式 WriterAdapter 文件回归。

## 1.6.5 失败处理

依次检查：

1. 消息定义。
2. 直接与传递依赖。
3. WriterAdapter Schema。
4. MCAP Channel。
5. 插件编码。
6. 分析环境隔离。
7. 插件版本。

仍不可行则进行架构评审。

不得未经批准将“自包含分析”降级为“分析端安装消息包后可以打开”。

---

# 2. 核心架构与数据路径

## 2.1 总体架构

```text
                       ./ros2_bag
                            |
                       CLI / Web
                            |
                        UDS JSONL
                            |
+------------------------------------------------+
|                ros2_bag_recorder               |
|                                                |
|              RecordingCoordinator              |
|                        |                       |
|                   TopicManager                 |
|                        |                       |
|               GenericSubscription              |
|                        |                       |
|                SerializedMessage               |
|                        |                       |
|                   QueueBudget                  |
|                        |                       |
|              RecordedMessagePool               |
|                        |                       |
|                  QueuedMessage                 |
|                        |                       |
|           BlockingConcurrentQueue              |
|                        |                       |
|                  Writer Thread                 |
|                        |                       |
|                  WriterAdapter                 |
|                        |                       |
|                   rosbag2_cpp                  |
|                        |                       |
|                       MCAP                     |
|                                                |
|       ResourceMonitor → ResourceSnapshot       |
|              |                  |              |
|              v                  v              |
|        Coordinator       Status Publisher      |
+------------------------------------------------+
```

## 2.2 对象生命周期

```text
Recorder Process
  |
  +-- RecordingCoordinator
  +-- TopicManager
  +-- ROS Executor
  +-- RecordedMessagePool
  |      |
  |      +-- shared PoolState
  |
  +-- Recording Task
  |      |
  |      +-- QueueBudget
  |      +-- MessageQueue
  |      +-- ProducerToken
  |      +-- Writer Thread
  |      +-- WriterAdapter
  |
  +-- ResourceMonitor
  +-- Status Publisher
  +-- Cleanup Worker
  +-- ControlServer
```

Pool 为进程级。

QueueBudget、MessageQueue、ProducerToken 和 Writer 为任务级。

一个 Recorder 同时最多有一个当前 Recording Task。

## 2.3 线程

| 上下文 | 职责 |
|---|---|
| MultiThreadedExecutor | 订阅回调 |
| RecordingCoordinator | 状态机与控制操作 |
| Writer Thread | Writer 独占、分片 |
| ResourceMonitor Worker | 资源、磁盘采样 |
| Status Publisher | 状态文件 |
| Cleanup Worker | 删除完整分片 |
| ControlServer | UDS 请求处理 |

默认：

```yaml
executor:
  thread_count: 4
```

Executor 线程数在进程启动时确定。

## 2.4 锁与内存序约束

1. QueueBudget 锁中不操作消息队列。
2. Pool Global Free-List 锁中不执行 Writer I/O。
3. Pool 全局锁中不析构大型 SerializedPayload。
4. Coordinator 状态锁中不等待 Writer。
5. Writer 控制队列锁中不调用 rosbag2 Writer。
6. TopicManager 锁中不等待 Executor 退出。
7. 状态快照锁中不生成 JSON。
8. ResourceSnapshot 锁中不执行 statvfs。
9. MessageDeleter 不同步调用 Coordinator。
10. Cleanup 不持有 Coordinator 锁执行文件删除。

所有跨线程共享的非原子可变数据必须通过 mutex 或正确的 happens-before 关系保护。

允许标准 C++17 的 seq_cst。

使用 relaxed/acquire/release 时说明同步依据。

不能因为 AMD64 TSan 通过就宣称已经完成 ARM64 弱内存序验证。

---

# 2.5 TopicInfo、QoS 和 Topic 身份

## 2.5.1 TopicQos

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

V1.0 对外支持：

- reliability。
- durability。
- history。
- depth。

不支持：

- deadline。
- lifespan。
- liveliness。
- liveliness_lease_duration。

出现不支持字段返回 INVALID_ARGUMENT，并指出配置路径。

## 2.5.2 QoS 自动适配

优先级：

```text
用户显式 QoS
      >
当前发布者兼容 QoS
      >
默认 QoS
```

创建订阅之前读取：

```cpp
get_publishers_info_by_topic(...)
```

创建之后：

- 使用实际 RMW 支持的 Requested Incompatible QoS 事件。
- 观察匹配关系。
- 记录 QOS_DIAGNOSTIC。
- 处理发布者后续加入和退出。

不能依赖一次 ROS Graph 查询永久判断 QoS 兼容。

## 2.5.3 RepeatPolicy

```cpp
struct RepeatPolicy {
    bool enabled = false;
    std::size_t max_messages = 64;
    std::size_t max_bytes = 4194304;
};
```

独立于 TopicQos。

## 2.5.4 TopicInfo

```cpp
using TopicId = std::uint64_t;

constexpr TopicId kInvalidTopicId = 0;

struct TopicInfo {
    TopicId id = kInvalidTopicId;

    std::string name;
    std::string type;
    std::string serialization_format;

    TopicQos qos;
    std::optional<std::string> type_hash;
    std::optional<double> expected_hz;

    RepeatPolicy repeat_policy;
};
```

TopicInfo 是任务内不可变描述。

不包含订阅对象、Writer 对象或高频可变统计器。

## 2.5.5 身份

```text
recording_generation
topic_id
topic_epoch
```

- recording_generation：任务代际。
- topic_id：任务内 TopicInfo 身份。
- topic_epoch：订阅实例身份。

同名 Topic 重添加可以复用 topic_id，但必须生成新 topic_epoch。

## 2.5.6 type_hash

| 情况 | 处理 |
|---|---|
| 同标准哈希已知且相等 | 允许 |
| 同标准哈希已知但不同 | INVALID_ARGUMENT |
| 一方未知，基础属性相同 | 允许并记录 TYPE_DIAGNOSTIC |
| 双方未知，基础属性相同 | 允许 |
| 类型字符串不同 | INVALID_ARGUMENT |

不得伪造未知哈希。

## 2.5.7 Topic 容量

```yaml
topics:
  discovery_timeout_ms: 2000
  allow_empty_topics: false
  max_active_topics: 64
  max_distinct_topics: 256
  recent_removed_instances: 32
```

max_active_topics 满时：

- 新 add_topic 返回 RESOURCE_EXHAUSTED。
- 可以先 remove_topic 再添加。
- 不自动移除其他 Topic。

max_distinct_topics 满时：

- 当前任务不能创建新 Topic 身份。
- 删除 Topic 不重置 distinct 计数。
- 新 Recording Task 可以重新开始身份分配。

---

# 2.6 SerializedPayload

## 2.6.1 模型 A

G0 首选：

```cpp
std::shared_ptr<rclcpp::SerializedMessage>
```

必须证明：

- Humble/Jazzy API 合法。
- 回调结束后 owner 仍有效。
- Writer 延迟消费安全。
- allocator 释放路径正确。
- 不产生 Recorder 新增完整 CDR payload 深拷贝。

## 2.6.2 TimestampSource

```cpp
enum class TimestampSource {
    Unknown,
    Receive,
    Source
};
```

Unknown 仅用于未初始化或无法确定来源的中间状态。

正式入队前必须满足任务时间戳策略。

## 2.6.3 RecordedMessage

```cpp
struct RecordedMessage {
    std::uint64_t recording_generation = 0;

    TopicId topic_id = kInvalidTopicId;
    std::uint64_t topic_epoch = 0;

    std::shared_ptr<const TopicInfo> topic_info;

    std::shared_ptr<rclcpp::SerializedMessage>
        serialized_message;

    std::size_t payload_bytes = 0;

    std::int64_t recv_timestamp_ns = 0;
    std::int64_t send_timestamp_ns = 0;

    TimestampSource timestamp_source = TimestampSource::Unknown;

    bool is_replayed = false;
};
```

generation=0 和 topic_id=0 表示无有效任务或 Topic 身份。

acquire 后必须初始化实际值。

release 后恢复安全默认状态。

## 2.6.4 WriterAdapter 借用规则

WriterAdapter 可以构造 rosbag2 SerializedBagMessage 描述符。

要求：

1. 保留原 SerializedMessage owner。
2. 不错误释放借用的 rcutils buffer。
3. 不在 owner 消失之后访问底层数据。
4. 不在包装或适配层新增完整 payload 深拷贝。
5. 在 Humble/Jazzy 实际 Writer API 上验证。

## 2.6.5 A→B→C→D 替代路径

| 模型 | 说明 |
|---|---|
| A | shared_ptr<SerializedMessage> |
| B | 独占拥有型 SerializedMessage |
| C | 自有 vector payload |
| D | 分档/slab payload |

模型 A 被 G0 拒绝时：

1. 保存失败证据。
2. 评估 B。
3. B 不可行则评估 C。
4. C 不可行则评估 D。
5. 同步修订 WriterAdapter、Pool 和缓存预算。
6. 批准替代架构后继续对应正式数据路径开发。

全部失败：

```text
G0 = BLOCKED
```

项目负责人发起设计前提重新评审。

在新架构批准和重新完成 G0 前，不允许将不安全的临时方案提交为正式数据路径。

独立于该数据路径的其他工程工作可继续。

## 2.6.6 模型与 Pool 缓存

模型 A/B：

```yaml
pool:
  max_cached_payload_bytes: 0
  max_per_node_buffer_bytes: 0
```

模型 C/D 必须重新确定缓冲区缓存预算。

## 2.6.7 AC-80

要求：

```text
Recorder 包装、入队、WriterAdapter 适配阶段
新增完整 CDR payload 深拷贝次数 = 0
```

验证：

- 源码和所有权审查。
- 可控复制入口测试插桩。
- Writer API 实际调用验证。
- 必要的用户态性能探针。
- 相同 SerializedPayload 模型的对照测试。

不宣称整个 DDS 到磁盘路径零拷贝。

## 2.6.8 数据模型决策文档

```text
docs/recorded_message_data_model.md
```

至少记录：

1. 精确 C++ 类型。
2. Humble/Jazzy API。
3. 回调生命周期。
4. allocator。
5. 回调后延迟消费。
6. WriterAdapter。
7. 复制边界。
8. 数据一致性。
9. 消息尺寸分布。
10. Pool 边界。
11. 替代方案。
12. ASan。
13. MCAP 读回。
14. 依赖版本。
15. 源码 SHA。
16. 日志路径。
17. Reviewer。
18. Review Date。
19. APPROVED/PENDING/REJECTED。

---

# 2.7 RecordedMessagePool

## 2.7.1 正式地位

RecordedMessagePool 是 V1.0 必需组件。

主方案：

```text
TLS Cache
    |
Batch Transfer
    |
Global Free-List
    |
Fixed Node Storage
```

Plan B：

```text
Global Mutex
    |
Global Free-List
    |
Fixed Node Storage
```

G0 负责批准正式 Pool 工作模式。

不得删除 Pool 作为逃避并发问题的正式方案。

## 2.7.2 PoolState

PoolState 为进程级共享生命周期状态，持有：

- 固定 Node 存储。
- Global Free-List。
- TLS Cache 协作状态。
- 节点借出状态。
- 在途 acquire/release 状态。
- 回退对象注册表。
- Pool 统计。
- 互斥锁。
- Fatal Error。
- PoolRuntimeState。

RecordedMessagePool 是 PoolState 的外部控制接口。

MessageHandle 不通过裸 RecordedMessagePool* 归还节点。

## 2.7.3 PoolRuntimeState

```cpp
enum class PoolRuntimeState {
    Running,
    Stopping,
    Stopped
};
```

状态转换：

```text
Running → Stopping → Stopped
```

| 状态 | acquire | release |
|---|---|---|
| Running | 允许 | 允许 |
| Stopping | 拒绝新申请 | 接受全部合法在途归还 |
| Stopped | 拒绝 | 不允许再有合法待释放消息 |

### 2.7.3.1 Running → Stopping

1. 在定义的同步边界阻止新 acquire。
2. 等待已进入 acquire 流程的操作完成或回滚。
3. 保留固定节点和回退对象的合法 release。
4. 允许 TLS flush。
5. 保持 PoolState 有效。
6. 不因为进入 Stopping 就销毁 Node Storage。

### 2.7.3.2 Stopping → Stopped

全部条件必须满足：

```text
in_use = 0
fallback_in_use = 0
tls_free = 0
global_free = capacity
fallback_registry.size() = 0
```

并且：

- in-flight acquire=0。
- in-flight release=0。
- 所有 TLS Cache 退出。
- 没有新的 Pool 访问可能发生。
- 节点及回退计数守恒。
- 不存在未完成的回退登记操作。

不能只检查 fallback_in_use=0 而忽略登记表残留。

### 2.7.3.3 Stopped 后发生释放

这是不可恢复的内部生命周期协议违规。

规定：

1. 不把节点再次插入 Free-List。
2. 不尝试未知所有权情况下的强制 delete。
3. 尽可能设置已存在的原子内部错误码。
4. 调用 `fail_fast()`。
5. 不保证已损坏状态下的日志完整持久化。

## 2.7.4 fail-fast

正式采用标准 C++17 机制：

```cpp
enum class PoolErrorCode {
    None,
    InvalidNodeIndex,
    InvalidNodeOwnership,
    DoubleRelease,
    InvalidFallbackOwnership,
    ReleaseAfterStopped,
    InternalInvariantViolation
};

[[noreturn]] void fail_fast(
    PoolErrorCode error_code
) noexcept;
```

正式实现要求：

1. 尽可能记录首个内部错误码。
2. 不动态分配内存。
3. 不等待 Coordinator。
4. 不执行 Writer I/O。
5. 不构造复杂 JSON。
6. 调用 `std::abort()`。

**不能仅依赖 `assert()`，因为 NDEBUG 构建可能禁用断言。**

不为此新增 `ROS2_BAG_FAIL_FAST()` 宏。

可恢复错误与内部不变量错误必须区分。

| 错误 | 处理 |
|---|---|
| 固定节点暂时耗尽 | 受控回退 |
| 回退容量耗尽 | 分配失败、丢弃 |
| 系统内存分配失败 | 正常失败路径 |
| NodeIndex 越界 | fail-fast |
| 节点确认重复归还 | fail-fast |
| PoolState 身份破坏 | fail-fast |
| Stopped 后释放 | fail-fast |
| 回退对象身份破坏 | fail-fast |

普通 Pool 容量不足不得导致 std::abort()。

严重内部所有权损坏不得通过继续运行掩盖。

## 2.7.5 Node

```cpp
struct alignas(64) Node {
    RecordedMessage message;
    Node* next_free = nullptr;
};
```

存储：

```cpp
std::unique_ptr<Node[]> storage_;
```

`alignas(64)` 规定对齐，不保证 Node 大小正好为 64 字节。

G1 必须记录：

```cpp
sizeof(Node)
alignof(Node)
```

不在没有性能证据时引入更复杂的 Cache-Line 布局。

## 2.7.6 NodeIndex

```cpp
using NodeIndex = std::uint32_t;

constexpr NodeIndex kInvalidNodeIndex =
    std::numeric_limits<NodeIndex>::max();
```

有效池节点：

```text
node_index < capacity
node_index != kInvalidNodeIndex
```

不从 RecordedMessage* 反向推断 Node*。

`kInvalidNodeIndex` 防止保留值被误当作池内索引。

实际默认 capacity=81920，远小于 uint32_t 极限；仍需要该检查以防止溢出和错误配置。

## 2.7.7 MessageDeleter

```cpp
class PoolState;

struct MessageDeleter {
    std::shared_ptr<PoolState> pool_state;

    NodeIndex node_index = kInvalidNodeIndex;

    void operator()(
        RecordedMessage* message
    ) const noexcept;
};

using MessageHandle =
    std::unique_ptr<RecordedMessage, MessageDeleter>;
```

**全部由 Pool 分配的消息，包括固定池节点与堆回退对象，都持有有效 PoolState。**

身份矩阵：

| pool_state | node_index | 说明 |
|---|---|---|
| 非 null | 小于 capacity | 固定池节点 |
| 非 null | kInvalidNodeIndex | PoolState 管理的堆回退对象 |
| null，message 非 null | 任意 | 非法 |
| 非 null，其他越界值 | 非法 |

## 2.7.8 空 MessageHandle

空句柄不代表 Pool 正在使用一个节点。

默认 Deleter：

```cpp
MessageDeleter{}
```

满足：

```text
pool_state == nullptr
node_index == kInvalidNodeIndex
```

标准 unique_ptr 持有 nullptr 时，析构不会调用 Deleter。

但 Deleter 本身也应安全处理显式的 null 参数：

```cpp
void MessageDeleter::operator()(
    RecordedMessage* message
) const noexcept {
    if (message == nullptr) {
        return;
    }

    // 验证身份并执行合法释放
}
```

实际空句柄默认构造必须通过 C++17 编译验证。

空 MessageHandle 不增加 `in_use` 或 `fallback_in_use`。

## 2.7.9 固定池节点释放

1. 验证 pool_state。
2. 验证 NodeIndex。
3. 定位固定 Node。
4. 验证 `&node.message == message`。
5. 验证借出状态。
6. 释放 SerializedPayload owner。
7. 释放 TopicInfo。
8. 重置 RecordedMessage。
9. 归还合法 TLS Cache 或 Global Free-List。
10. 更新统计。

不得在无身份验证的情况下访问可疑指针内部字段。

## 2.7.10 有界回退登记表

### 2.7.10.1 正式结构

采用：

```cpp
class PoolState {
private:
    std::mutex fallback_mutex_;

    std::unordered_set<RecordedMessage*>
        fallback_registry_;

    std::size_t max_fallback_in_use_ = 1024;
};
```

此为内部实现，不作为公共 API。

登记表负责：

- 证明回退对象属于当前 PoolState 的在途分配。
- 限制同时在途回退数量。
- 验证回退释放。
- 参与 PoolState 关闭守恒。

### 2.7.10.2 容量

必须满足：

```text
fallback_registry.size()
    <= max_fallback_in_use
```

注册表只登记当前在途回退对象。

释放后立即移除。

不保存所有历史回退指针。

### 2.7.10.3 初始化

PoolState 初始化时可执行：

```cpp
fallback_registry_.reserve(
    max_fallback_in_use_
);
```

注意：

**`reserve()` 只预留哈希桶，不保证后续 `insert()` 完全不分配内存。**

因此 `insert()` 仍必须处理 `std::bad_alloc` 等失败。

注册表自身分配失败不得导致返回不完整的 MessageHandle。

### 2.7.10.4 回退 acquire

正式操作顺序：

1. 通过 PoolState acquire 准入屏障。
2. 确认 PoolRuntimeState 允许申请。
3. 检查固定池节点不可用。
4. 检查 max_fallback_in_use。
5. 在锁外构造新的 RecordedMessage 对象。
6. 在 fallback_mutex_ 保护下再次确认容量及准入状态。
7. 向登记表插入该地址。
8. 插入成功后更新 fallback_in_use 和 fallback_count。
9. 构造具有同一 PoolState 所有权的 MessageHandle。
10. 返回句柄。
11. 结束 acquire 在途操作。

在步骤 6～9 中必须保证异常安全。

正式实现可以使用私有 RAII 工厂封装分配和登记过程。

不允许在登记成功以前将对象作为正式 MessageHandle 暴露给调用者。

### 2.7.10.5 acquire 失败回滚

可能失败：

- 已达到容量。
- RecordedMessage 对象分配失败。
- unordered_set 插入分配失败。
- Pool 进入 Stopping。
- 构造句柄失败或不满足内部前提。

要求：

1. 尚未登记的对象由局部 RAII owner 销毁。
2. 已登记但未成功交付的对象必须解除登记。
3. 计数不能泄漏。
4. 释放相关锁。
5. 返回失败给调用者。
6. 调用方释放 BudgetLease。
7. 不返回不完整 MessageHandle。

登记表分配失败属于正常资源失败，不能与确认的身份损坏混为一谈。

### 2.7.10.6 回退 release

1. 验证 pool_state 非 null。
2. 验证 node_index 为 kInvalidNodeIndex。
3. 获取 fallback_mutex_。
4. 按指针值在 fallback_registry_ 查找。
5. 找不到登记：内部不变量错误。
6. 找到登记：删除登记。
7. 更新 fallback_in_use。
8. 退出临界区。
9. 销毁 RecordedMessage。

不得在登记表 mutex 内执行可能昂贵的 SerializedPayload 释放。

关闭屏障必须考虑 release 进入临界区后、实际对象销毁前的在途释放阶段。

### 2.7.10.7 指针身份局限

`unordered_set<RecordedMessage*>` 可以验证：

**该地址当前是否处于本 PoolState 的回退登记集合。**

它不能独立证明：

- 任意内存破坏下的指针绝对安全。
- 一个非法复制的旧 Deleter 不会碰巧遇到已重新分配的同一地址。
- C++ 未定义行为可以完全通过登记表恢复。

正式系统依赖：

- MessageHandle 的唯一所有权。
- 禁止复制所有权句柄。
- 受控创建。
- 受控登记。
- 正确并发同步。
- 在确认不变量破坏时 fail-fast。

如果正式实现需要在已释放地址被重新使用后识别过期句柄，必须通过架构评审增加分配身份代际，不得声称仅用裸指针集合已经解决 ABA。

### 2.7.10.8 登记表与 Stopped

进入 Stopped 前：

```text
fallback_in_use = 0
fallback_registry.size() = 0
```

两个条件必须同时验证。

如果计数为 0 但登记表非空：

```text
InternalInvariantViolation
```

不得宣布 Stopped。

## 2.7.11 max_fallback_in_use

默认：

```yaml
pool:
  max_fallback_in_use: 1024
```

1024 约等于默认 Queue 最大消息数 65536 的 1/64。

这是初始保护参数，不是经过性能测试证明的最优值。

默认固定节点：

```text
capacity = 81920
```

已经大于默认 Queue 消息数：

```text
max_messages = 65536
```

因此，**队列接近满本身并不意味着一定要回退**。

正常情况下固定池应尽可能覆盖在途包装对象。

回退用于处理额外借出、节点暂未归还或异常峰值。

Phase 1 应比较：

```text
max_fallback_in_use = 0
max_fallback_in_use = 256
max_fallback_in_use = 1024
```

测量：

- 稳态回退次数。
- 峰值 fallback_in_use。
- 登记表锁竞争。
- CPU。
- P99。
- RSS。
- 失败与丢弃数量。

配置不得允许无界回退。

## 2.7.12 Pool 关闭屏障

PoolState 的准入、在途 acquire、在途 release 与回退登记必须协调。

不能仅靠：

```cpp
state.load() == PoolRuntimeState::Running
```

在检查后不受保护地执行分配与登记，因为状态可能立即变成 Stopping。

正式实现必须有明确的 acquire 准入线性化点。

推荐采用：

- 原子状态与在途操作计数；或
- 短 mutex 保护的状态和在途计数。

要求：

1. Stopping 之后不产生新的成功借出。
2. 已进入合法 acquire 的操作要么完成、要么回滚。
3. 已返回的句柄允许归还。
4. Stopped 前等待所有在途 release。
5. Stopped 前验证固定节点和回退登记守恒。

G1 必须对 acquire/Stopping、release/Stopped 竞争进行压力测试。

## 2.7.13 TLS 与跨线程 release

必须支持：

```text
Executor A acquire
        |
        v
MessageQueue
        |
        v
Writer Thread release
```

归还可以使用当前释放线程的 TLS Cache，不必返回原申请线程。

要求：

- TLS Cache 有界。
- PoolState 生命周期有效。
- 线程退出完成 flush。
- 无跨任务污染。
- 正常关闭恢复节点守恒。

不允许 TLS Cache 与 PoolState 形成无法释放的 shared_ptr 环。

## 2.7.14 Pool 容量

\[
capacity=\lceil max\_messages\times capacity\_factor\rceil
\]

默认：

```text
65536 × 1.25 = 81920
```

配置：

```yaml
pool:
  enabled: true
  mode: tls
  capacity_factor: 1.25
  max_capacity: 1000000
  max_fallback_in_use: 1024
  local_capacity: 64
  batch_size: 64
  max_cached_payload_bytes: 0
  max_per_node_buffer_bytes: 0
```

校验：

```text
1 <= max_capacity <= 1000000
1 <= calculated_capacity <= max_capacity
calculated_capacity < kInvalidNodeIndex
```

还必须检查内存分配大小溢出和申请失败。

## 2.7.15 global_mutex

| 配置 | TLS | global_mutex |
|---|---|---|
| capacity_factor | 有效 | 有效 |
| max_capacity | 有效 | 有效 |
| max_fallback_in_use | 有效 | 有效 |
| local_capacity | 验证并使用 | 忽略 |
| batch_size | 验证并使用 | 忽略 |
| payload 缓存 | 依据模型 | 依据模型 |

global_mutex 不创建 TLS Cache。

正式只维护 G0 批准的生产模式即可。

## 2.7.16 节点守恒

TLS：

```text
capacity = in_use + global_free + tls_free
```

global_mutex：

```text
tls_free = 0
capacity = in_use + global_free
```

正常 Stopped：

```text
in_use = 0
fallback_in_use = 0
fallback_registry.size() = 0
tls_free = 0
global_free = capacity
internal_error_count = 0
```

运行中多个原子统计快照可以近似一致。

静止状态下必须精确验证。

## 2.7.17 Pool 状态指标

至少：

```text
enabled
mode
capacity
in_use
available
fallback_in_use
fallback_count
allocation_failure_count
internal_error_count
tls_hit_rate
cached_payload_bytes
buffer_evict_count
global_lock_ops
```

### tls_hit_rate

- null：Pool 关闭、非 TLS 模式或尚无有效统计分母。
- 0：TLS 模式有效统计区间内命中数为 0。
- 0～1：有效命中比例。

global_mutex 下必须为 null，不是 0。

### global_lock_ops

表示自 PoolState 初始化以来，Global Free-List 相关锁成功获取次数的累计值。

不包含 QueueBudget、Coordinator 或 Writer 的锁。

## 2.7.18 POOL_CAPACITY_WARNING

任意 60 秒滑动窗口内：

```text
fallback_count 增量 > 100
```

触发：

```text
POOL_CAPACITY_WARNING
```

规则：

1. 使用增量而非累计总数。
2. 同一告警 60 秒内最多记录一次。
3. fallback 连续 60 秒无增长后解除活动告警。
4. 再次满足条件时允许重新触发。
5. 告警不直接进入 ERROR。

实现可使用有界时间桶或等价滑动窗口算法。

不要求采用特定容器。

## 2.7.19 MessageHandle 内存开销

G1 实测：

```cpp
sizeof(RecordedMessage)
sizeof(Node)
sizeof(MessageDeleter)
sizeof(MessageHandle)
sizeof(QueuedMessage)
```

64 位 Linux 上，含 shared_ptr 和 NodeIndex 的 Deleter 可能约 24 字节，MessageHandle 可能约 24～32 字节。

这是估算，不是 ABI 保证。

以实际编译器输出为准。

不为了减少少量句柄字节数而改回生命周期不安全的裸 Pool 指针。

---

# 2.8 QueueBudget 与 MessageQueue

## 2.8.1 默认配置

```yaml
queue:
  max_bytes: 67108864
  max_messages: 65536
  batch_size: 256
  wait_timeout_ms: 20
  overflow_policy: drop_newest
```

## 2.8.2 双重配额

所有时刻：

```text
used_bytes <= max_bytes
used_messages <= max_messages
```

bytes/messages 在一个线性化操作中申请及修改。

默认使用短 mutex 临界区。

## 2.8.3 BudgetLease

move-only RAII 凭证。

负责：

- 成功申请后的所有权。
- 入队成功后转移。
- Pool 失败回滚。
- 入队失败回滚。
- Writer 消费释放。
- Writer 错误释放。
- Queue 销毁释放。
- 恰好一次归还。

统计：

```text
budget_acquire_count
budget_release_count
budget_release_normal_count
budget_release_rollback_count
budget_double_release_count
budget_invalid_release_count
```

最终：

```text
budget_acquire_count == budget_release_count
budget_double_release_count = 0
budget_invalid_release_count = 0
used_bytes = 0
used_messages = 0
```

## 2.8.4 QueuedMessage

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

必须通过 C++17 实际编译验证 move-only 类型。

## 2.8.5 ProducerToken

- 绑定当前 MessageQueue。
- 不跨线程并发共享。
- 不逐消息创建。
- 不跨任务复用。
- 旧队列销毁前结束旧 Token 使用。
- Executor 线程复用时也必须使用新任务 Token。

## 2.8.6 入队流程

```text
GenericSubscription
       |
       v
Owned SerializedMessage
       |
       v
Generation / Topic Epoch / Ingress 检查
       |
       v
QueueBudget.try_acquire
       |
       +--失败→ dropped_capacity
       |
       v
Pool.acquire
       |
       +--失败→ BudgetLease 回滚
       |
       v
QueuedMessage
       |
       v
try_enqueue
       |
       +--失败→ RAII 回滚
       |
       v
Writer Thread
       |
       v
MCAP
       |
       v
释放 BudgetLease + MessageHandle
```

## 2.8.7 Generation 隔离

正常 Stop→Start：

1. 关闭 ingress。
2. 等待 Producer。
3. 排空 Queue。
4. 关闭 Writer。
5. QueueBudget 归零。
6. MessageHandle 释放。
7. ProducerToken 失效。
8. 旧 Queue 销毁。
9. 新任务创建。

正常：

```text
dropped_stale_generation = 0
```

违规：

- 旧 Generation 消息不能写入新 Writer。
- 每条违规消息递增计数。
- 首次触发 GENERATION_VIOLATION。
- 关闭 ingress。
- 安全 ERROR。
- 不重复启动错误收尾。

---

# 2.9 动态 Topic 生命周期

## 2.9.1 状态

```text
ABSENT → ADDING → ACTIVE → REMOVING → REMOVED
              |
              +→ ARMED
              |
              +→ FAILED
```

## 2.9.2 操作边界

- Accepted：请求接纳。
- Effective：订阅/ingress 生效。
- Committed：终态 durable 提交。

PREPARED 仅代表意图。

## 2.9.3 add_topic

1. 校验请求。
2. 分配 topic_id/topic_epoch。
3. durable TOPIC_ADD_PREPARED。
4. Writer 注册 TopicMetadata。
5. 创建 GenericSubscription。
6. Executor 接纳订阅。
7. 开放 ingress。
8. durable TOPIC_ADDED。
9. 操作 COMPLETED。

PAUSED 下可以进入 ARMED。

不开放 ingress。

## 2.9.4 Writer 注册后订阅失败

```text
TOPIC_ADD_PREPARED
        |
        v
Writer REGISTER_TOPIC 成功
        |
        v
GenericSubscription 创建失败
        |
        v
TOPIC_ADD_FAILED
```

保留已注册 Schema/Channel 至 Segment 关闭。

不调用无安全回滚保证的 Writer::remove_topic。

不开放 ingress。

不伪造 TOPIC_ADDED。

## 2.9.5 remove_topic

1. durable TOPIC_REMOVE_PREPARED。
2. 关闭当前 epoch ingress。
3. 等待 Producer。
4. 解除订阅。
5. durable TOPIC_REMOVED。
6. 操作 COMPLETED。

旧 epoch 已接纳消息继续正常写入。

## 2.9.6 remove 失败

订阅仍 ACTIVE：

- 留在 topics[]。
- state=ACTIVE。
- last_operation_error 记录失败。

订阅失效但尚未安全退出：

- 留在 topics[]。
- state=FAILED。

实例已确定退出：

- 进入 topics_removed[]。

不能因为操作 FAILED 就删除仍然活跃的 Topic 实例。

## 2.9.7 同名重添加

可以复用 topic_id。

必须生成新 topic_epoch。

旧 epoch 消息不能归属新 epoch。

## 2.9.8 Topic 事件持久化失败

1. 关闭 ingress。
2. 拒绝新的 Topic 操作。
3. 等待 Producer。
4. 尽可能排空已接纳消息。
5. 安全关闭 Writer。
6. 进入 ERROR。
7. 保存诊断。

---

# 2.10 WriterAdapter 与 Writer Thread

## 2.10.1 WriterAdapter 职责

- Humble/Jazzy API 差异。
- Writer.open/close。
- TopicMetadata。
- MessageDefinition。
- Schema/Channel。
- SerializedBagMessage。
- 时间戳适配。
- Writer 消费期间 payload owner 存活。

## 2.10.2 Writer 独占

只有 Writer Thread 调用 rosbag2 Writer。

不允许 TopicManager 或 GenericSubscription 直接并发调用 Writer。

## 2.10.3 Writer 控制队列

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

控制队列有界。

安全 Stop/shutdown 必须始终存在可执行路径。

## 2.10.4 公平调度

每轮最多：

- 8 条控制命令。
- 256 条数据消息。

队列为空时可使用：

```text
wait_dequeue_bulk_timed
```

最大等待：

```text
queue.wait_timeout_ms
```

默认 20ms。

不能因数据队列永久阻塞而导致控制饿死。

不能因大量控制命令而永久阻塞数据写入。

## 2.10.5 Stop

1. 关闭 ingress。
2. 等待 Producer。
3. 完成允许安全收尾的控制操作。
4. 排空合法消息。
5. Writer 完成当前批次。
6. Writer.close。
7. QueueBudget 归零。
8. 释放任务级 MessageHandle。
9. durable 提交终态。

## 2.10.6 Resume Prepare/Commit

```text
PAUSED
   |
   v
RESUMING
   |
   v
准备 Segment
   |
   v
注册 Topic
   |
   v
验证 Writer
   |
   v
开放 ingress
   |
   v
RECORDING
```

Prepare 失败且无不可逆副作用时返回 PAUSED。

保留 paused_reason。

不提前开放 ingress。

## 2.10.7 Resume 与 Stop

RESUMING 期间接收到 Stop：

1. Coordinator 接纳停止意图。
2. 返回 PENDING。
3. 不并发调用 Writer。
4. Resume Prepare 达到明确安全边界。
5. 如果 Prepare 失败，回到 PAUSED，再执行 Stop。
6. 如果 Prepare 成功但已经有 Stop 意图，不继续正常开放新 ingress。
7. 安全关闭候选 Segment。
8. 完成 Stop。

Stop 不需要抢占正在执行的不可中断 Writer 调用。

但必须避免无限等待。

---

# 3. 录制模式、持久化和磁盘保护

## 3.1 目录结构

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

每个 Segment 是独立 rosbag2 Bag。

录制任务根目录不直接作为一个 Bag。

## 3.2 Normal

手动 start/stop。

允许零消息正常完成。

如果 Writer 没有实际打开 Segment：

```json
"segments": []
```

如果 Writer 已经打开空 Segment，必须正确关闭并如实记录。

不持续生成空分片。

## 3.3 Timed

使用单调时间。

暂停计入总时长。

到期进入 STOPPING。

记录：

```text
elapsed_wall_sec
elapsed_active_sec
paused_total_sec
```

## 3.4 Rolling

默认：

```yaml
recording:
  segment_duration_sec: 60
  retention_duration_sec: 600
```

只删除 COMPLETE Segment。

**PAUSED 期间保留窗口继续推进。**

例如暂停一小时，600 秒保留窗口内的旧 COMPLETE 分片可能被删除。

CLI/Web 必须提示：

```text
Rolling 模式下，暂停期间保留窗口仍会推进，
已完成分片可能继续被清理。
```

用户希望保留已有完整分片时应使用 Stop。

## 3.5 MCAP

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

rosbag2：

```yaml
storage:
  max_cache_size: 0
```

低 CPU 模式：

```yaml
compression: "None"
```

压缩策略在 Task Start 时固定。

改压缩时 Stop 当前任务、更新配置、Start 新任务。

不要求重启整个 Recorder。

## 3.6 Snapshot

```yaml
repeat_on_segment:
  enabled: true
  max_messages: 64
  max_bytes: 4194304

snapshot:
  max_total_bytes: 16777216
```

Writer Thread 管理快照。

按 payload bytes 计入预算。

Topic 移除后不无限缓存旧 epoch。

## 3.7 Segment 状态

```text
WRITING → FINALIZING → COMPLETE → DELETING → REMOVED
             |
             v
         INCOMPLETE
```

只有 COMPLETE 自动清理。

DELETING 前先 durable 持久化删除授权。

INCOMPLETE 不自动删除。

## 3.8 Cleanup Worker

一个线程，顺序处理。

```yaml
cleanup:
  queue_capacity: 16
  warning_timeout_sec: 30
```

30 秒为诊断阈值。

不是能够安全中断文件系统调用的保证。

阻塞超过阈值：

- 记录 CLEANUP_TIMEOUT。
- 保留 DELETING。
- 不阻塞 Coordinator。
- 不并发重试相同路径。
- 重启后根据 durable 记录恢复。

如果底层删除永久阻塞，单线程 Cleanup Worker 无法继续执行后续删除任务。

不承诺无法提供的强制中断。

---

# 3.9 manifest.json

## 3.9.1 必需字段

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

示例：

```json
{
  "schema_version": 1,
  "session_id": "default",
  "recording_id": "run1",
  "mode": "normal",
  "recording_state": "COMPLETED",
  "timestamp_policy": "receive",
  "topic_events_file": "topic_events.jsonl",
  "last_error": null,
  "secondary_errors": [],
  "segments": [
    {
      "segment_id": 1,
      "path": "segment_000001",
      "state": "COMPLETE"
    }
  ],
  "cleanup_pending": []
}
```

## 3.9.2 持久化

1. 临时文件。
2. fsync 临时文件。
3. rename。
4. 必要时 fsync 父目录。

不能将 rename 单独等同于完整断电持久化。

---

# 3.10 topic_events.jsonl

## 3.10.1 事件类型

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

## 3.10.2 必需字段

```text
schema_version
event
event_seq
recording_id
operation_id
topic_id
topic_epoch
name
at_ns
```

可选：

```text
type
error_code
error_message
details
```

含义：

- schema_version：事件格式版本。
- event：事件类型。
- event_seq：任务内单调递增顺序号。
- recording_id：录制任务。
- operation_id：逻辑操作。
- topic_id/topic_epoch：Topic 实例。
- name：Topic 名称。
- at_ns：Unix Epoch 纳秒。

## 3.10.3 JSONL 示例

```json
{"schema_version":1,"event":"TOPIC_ADD_PREPARED","event_seq":1,"recording_id":"run1","operation_id":"op-001","topic_id":3,"topic_epoch":1,"name":"/joint_states","type":"sensor_msgs/msg/JointState","at_ns":1792670400000000000}
{"schema_version":1,"event":"TOPIC_ADDED","event_seq":2,"recording_id":"run1","operation_id":"op-001","topic_id":3,"topic_epoch":1,"name":"/joint_states","type":"sensor_msgs/msg/JointState","at_ns":1792670400100000000}
{"schema_version":1,"event":"TOPIC_REMOVE_PREPARED","event_seq":3,"recording_id":"run1","operation_id":"op-002","topic_id":3,"topic_epoch":1,"name":"/joint_states","type":"sensor_msgs/msg/JointState","at_ns":1792670500000000000}
{"schema_version":1,"event":"TOPIC_REMOVED","event_seq":4,"recording_id":"run1","operation_id":"op-002","topic_id":3,"topic_epoch":1,"name":"/joint_states","type":"sensor_msgs/msg/JointState","at_ns":1792670500100000000}
```

每行是独立 JSON。

## 3.10.4 多次 PREPARED

使用：

```text
operation_id + event_seq
```

匹配。

规则：

1. 相同 request_id 的幂等重试复用逻辑 operation_id。
2. 真正的新操作产生新 operation_id。
3. 按 operation_id 分组。
4. 在组内检查合法事件顺序。
5. 有合法 durable 终态则按终态解释。
6. 仅有 PREPARED 则标记未决。
7. 出现冲突终态时记录元数据错误。
8. 不仅按 topic_epoch 选择最后一条事件。

UDS、operation_status 和 topic_events 必须使用同一逻辑 operation_id。

---

# 3.11 异常恢复

1. 检查旧 Session 所有者。
2. 读取 manifest。
3. 读取 topic_events。
4. 检查 Schema 版本。
5. 按 operation_id 匹配操作。
6. 检查真实 Segment。
7. 检查 MCAP Schema/Channel。
8. 检查实际消息。
9. 标记未决 PREPARED。
10. 恢复 DELETING。
11. 保留 INCOMPLETE。
12. 保存恢复诊断。
13. 新 Recorder 进入 IDLE。

MCAP 有消息但没有 durable TOPIC_ADDED：

- 不删除实际消息。
- 不伪造操作成功。
- 记录 TOPIC_ADD_UNCERTAIN。
- 在恢复报告中说明证据。

未知异常终止：

```text
UNEXPECTED_TERMINATION
```

不无依据归因 WRITER_ERROR 或 DISK_FULL。

---

# 3.12 ResourceMonitor

统一负责：

- 进程 CPU。
- RSS。
- FD。
- 线程数。
- 系统可用内存。
- CPU 核数。
- 磁盘总容量。
- 目标可用空间。
- 使用率。
- 采样失败。

一个独立 ResourceMonitor Worker。

不新增 DiskMonitor。

## 3.12.1 默认配置

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

要求：

```text
disk.check_interval_sec >= resource.sample_interval_sec
```

并且 disk.check_interval_sec 为 resource.sample_interval_sec 的整数倍。

## 3.12.2 普通遥测与磁盘独立性

`resource.enabled=false`：

- 不采集普通 CPU/RSS/FD/线程遥测。
- process.sampling_enabled=false。
- 相关未采集指标为 null。
- **仍然执行磁盘采样。**
- **仍然执行磁盘 UNKNOWN/CRITICAL 保护。**

ResourceMonitor 可以使用同一 Worker 调度两类采样，不需要创建第二个 DiskMonitor。

但不得将 `resource.enabled=false` 理解为禁用磁盘安全。

## 3.12.3 CPU 百分比

\[
cpu\_percent =
\frac{\Delta process\_cpu\_seconds}
{\Delta steady\_seconds}\times100
\]

进程 CPU 可以超过 100%。

首次有效采样前为 null。

## 3.12.4 磁盘可用空间

使用：

```text
f_bavail × f_frsize
```

不能直接用 f_bfree 代替当前 Recorder 实际可用空间。

## 3.12.5 磁盘状态

```text
NORMAL
WARNING
CRITICAL
UNKNOWN
```

UNKNOWN：

- 连续采样失败达到阈值；或
- 最近有效样本年龄超过 max_sample_age_sec。

采样失败不伪造可用空间为 0。

## 3.12.6 时间戳

- updated_at_ns：状态文件生成时间。
- disk.sampled_at_ns：最近成功磁盘采样时间。
- disk.last_attempt_at_ns：最近采样尝试时间。

对外使用 Unix Epoch 纳秒。

内部超时及样本年龄使用单调时钟。

## 3.12.7 UNKNOWN 保护

初次磁盘采样失败，没有有效安全样本：

```text
start → DISK_STATE_UNKNOWN
```

运行中 UNKNOWN：

1. Coordinator 关闭 ingress。
2. 等待 Producer。
3. 排空已接纳消息。
4. 安全关闭 Writer。
5. 进入 PAUSED。
6. paused_reason=SYSTEM_UNKNOWN。

无法安全关闭 Writer 时进入 ERROR。

如果 statvfs 阻塞，Coordinator 依据单调时间判断样本陈旧，不等待 ResourceMonitor 返回。

---

# 3.13 paused_reason

值：

```text
null
USER
SYSTEM_DISK
SYSTEM_UNKNOWN
```

## 3.13.1 状态矩阵

| recording_state | paused_reason |
|---|---|
| IDLE | null |
| STARTING | null |
| RECORDING | null |
| PAUSING | 当前暂停原因 |
| PAUSED | USER / SYSTEM_DISK / SYSTEM_UNKNOWN |
| RESUMING | 保留原原因 |
| STOPPING | null |
| FINALIZING | null |
| COMPLETED | null |
| ERROR | null |

## 3.13.2 PAUSING

Coordinator 决定进入 PAUSING 时，与 recording_state 同时设置暂停原因。

## 3.13.3 RESUMING

成功：

```text
RESUMING → RECORDING
paused_reason = null
```

在同一次状态更新中生效。

Prepare 失败并返回 PAUSED 时保留原原因。

## 3.13.4 ERROR

ERROR 中：

```text
paused_reason = null
```

但是之前的暂停来源必须写入 last_error：

```json
{
  "code": "WRITER_ERROR",
  "message": "writer close failed",
  "previous_paused_reason": "SYSTEM_DISK"
}
```

任务此前并未暂停时可为 null。

## 3.13.5 USER 优先级

USER 暂停期间磁盘进入 UNKNOWN：

- paused_reason 保持 USER。
- disk.protection_state 显示 UNKNOWN。
- 不自动恢复。

手动 Resume：

- UNKNOWN 返回 DISK_STATE_UNKNOWN。
- CRITICAL 返回 DISK_FULL。
- 只有磁盘安全才进入 Resume Prepare。

## 3.13.6 自动恢复

仅 SYSTEM_DISK 和 SYSTEM_UNKNOWN 允许自动恢复。

要求：

- 有效磁盘采样。
- 可用空间充足。
- 使用率安全。
- 稳定达到 resume_stable_sec。

USER 不自动恢复。

Timed 到期不恢复。

ERROR 不自动恢复。

---

# 4. Recorder 生命周期、控制协议和状态 Schema

## 4.1 Recorder 状态

```text
STARTING → READY → SHUTTING_DOWN → EXITED
```

## 4.2 Recording 状态

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

## 4.2.1 稳态命令—状态矩阵

| 命令 | IDLE | RECORDING | PAUSED | COMPLETED | ERROR |
|---|---|---|---|---|---|
| start | 允许 | INVALID_STATE | INVALID_STATE | 允许 | 满足安全准入后允许 |
| stop | INVALID_STATE | 允许 | 允许 | INVALID_STATE | INVALID_STATE |
| pause | INVALID_STATE | 允许 | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| resume | INVALID_STATE | INVALID_STATE | 磁盘安全时允许 | INVALID_STATE | INVALID_STATE |
| add_topic | INVALID_STATE | 允许 | 允许，进入 ARMED | INVALID_STATE | INVALID_STATE |
| remove_topic | INVALID_STATE | 允许 | 允许 | INVALID_STATE | INVALID_STATE |
| status | 允许 | 允许 | 允许 | 允许 | 允许 |
| operation_status | 允许 | 允许 | 允许 | 允许 | 允许 |
| shutdown | 允许 | 允许 | 允许 | 允许 | 允许 |

## 4.2.2 过渡状态矩阵

| 命令 | STARTING | PAUSING | RESUMING | STOPPING | FINALIZING |
|---|---|---|---|---|---|
| start | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| stop | INVALID_STATE | INVALID_STATE | **接受停止意图** | **复用正在执行的 Stop** | INVALID_STATE |
| pause | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| resume | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| add_topic | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| remove_topic | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE | INVALID_STATE |
| status | 允许 | 允许 | 允许 | 允许 | 允许 |
| operation_status | 允许 | 允许 | 允许 | 允许 | 允许 |
| shutdown | 接受退出意图 | 接受退出意图 | 接受退出意图 | 接受退出意图 | 接受退出意图 |

### 4.2.2.1 RESUMING 中 Stop

- 接纳 Stop。
- 返回 PENDING。
- 登记停止意图。
- 等待 Resume Prepare 达到 Writer 安全边界。
- 执行一次安全 Stop。
- 不创建第二个 Writer 关闭流程。

### 4.2.2.2 STOPPING 中 Stop

不同 request_id 可以加入现有 Stop 的结果关联。

但：

- 不启动第二个 Stop。
- 不重复关闭 Writer。
- 不重复终态提交。
- 每个已接纳请求仍有可查询结果。
- 原有幂等规则继续生效。

### 4.2.2.3 过渡状态 Shutdown

在任何过渡状态：

1. 接纳退出意图。
2. 返回 PENDING 或对应协议结果。
3. 不直接抢占正在使用 Writer 的临界操作。
4. 等待到达安全边界。
5. 执行受控 Shutdown。

不引入通用优先级操作队列框架。

## 4.2.3 ERROR 后 start

八项条件全部满足：

1. Writer 已关闭。
2. 旧订阅已退出。
3. Producer 在途计数为 0。
4. QueueBudget 归零。
5. 任务级 MessageHandle 全部归还。
6. 旧 MessageQueue 生命周期结束。
7. PoolState 没有 Fatal Error，仍为 Running。
8. 当前磁盘状态有效且安全。

任何一项不满足：

```text
start → INVALID_STATE
```

错误 details 应指出未满足的条件。

Pool Fatal Error 后不能清除错误标志直接继续运行新任务。

---

# 4.3 UDS 控制协议

## 4.3.1 基础规则

- Unix Domain Socket。
- sockpp。
- JSON Lines。
- UTF-8。
- request_id。
- 有副作用命令幂等。

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

## 4.3.2 PENDING

```json
{
  "schema_version": 1,
  "request_id": "req-001",
  "state": "PENDING",
  "code": null,
  "data": {
    "phase": "QUEUED",
    "operation_id": "op-001"
  }
}
```

PENDING 只表示已接纳、尚未完成。

code 必须为 null。

## 4.3.3 COMPLETED

```json
{
  "schema_version": 1,
  "request_id": "req-001",
  "state": "COMPLETED",
  "code": "OK",
  "data": {
    "operation_id": "op-001",
    "topic_id": 3,
    "topic_epoch": 1
  }
}
```

COMPLETED 只表示操作成功。

## 4.3.4 FAILED：已接纳后失败

```json
{
  "schema_version": 1,
  "request_id": "req-001",
  "state": "FAILED",
  "code": "WRITER_ERROR",
  "data": {
    "operation_id": "op-001",
    "topic_id": 3,
    "topic_epoch": 1
  },
  "error": {
    "msg": "writer failed",
    "details": {}
  }
}
```

要求：

- operation_id 必须存在。
- 已分配 topic_id 时应返回 topic_id。
- 已分配 topic_epoch 时应返回 topic_epoch。
- 未分配的可选身份字段可以省略。
- 不伪造未实际分配的字段。

## 4.3.5 FAILED：未接纳请求

```json
{
  "schema_version": 1,
  "request_id": "req-002",
  "state": "FAILED",
  "code": "INVALID_ARGUMENT",
  "data": {
    "operation_id": null
  },
  "error": {
    "msg": "unsupported QoS field",
    "details": {
      "field": "qos.deadline"
    }
  }
}
```

未接纳请求：

- 没有进入执行队列。
- 没有创建逻辑 Operation。
- operation_id=null。
- 不能查询不存在的操作终态。

### 4.3.5.1 客户端解析规则

客户端必须：

1. 检查 state。
2. PENDING：检查 operation_id 并等待或查询。
3. COMPLETED：检查 code=OK。
4. FAILED：检查 operation_id。
5. FAILED 且 operation_id=null：请求未接纳。
6. FAILED 且 operation_id 非 null：已接纳操作执行失败。
7. 对 operation_id=null 不调用 operation_status。

不增加第四种 REJECTED 状态。

## 4.3.6 幂等

```yaml
ipc:
  idempotency_cache_size: 256
  max_pending_operations: 32
```

相同 request_id、相同请求：

- 只执行一次。
- 返回同一逻辑操作状态。
- 复用 operation_id。

相同 request_id、不同内容：

```text
INVALID_ARGUMENT
```

PENDING 不能因完成态缓存 FIFO 淘汰而丢失身份。

## 4.3.7 operation_status

使用 request_id 或 operation_id 查询。

查询返回：

- PENDING。
- COMPLETED。
- FAILED。
- NOT_FOUND。

operation_status 查询本身的成功结果与被查询业务操作的状态必须区分。

例如查询成功，但目标业务操作 FAILED，不应将查询自身也误判为执行失败。

---

# 4.4 schema_version

四类协议/文件分别管理：

| 协议/文件 | 版本字段 |
|---|---|
| status.json | schema_version |
| UDS 请求/响应 | schema_version |
| manifest.json | schema_version |
| topic_events.jsonl | 每条记录的 schema_version |

V1.0 初始均为整数 1。

未来可以独立演进。

规则：

1. 破坏性修改递增对应 Schema。
2. 兼容性可选字段可不递增。
3. 不支持版本时明确报错。
4. 不将未知字段格式默认为成功。
5. manifest/topic_events 升级时需要恢复策略。
6. 不强制四类 Schema 永远同步递增。

---

# 4.5 Session

默认：

```text
session_id = default
```

目录：

```text
$XDG_RUNTIME_DIR/ros2_bag/<session>/
```

内容：

```text
recorder.sock
status.json
recorder.pid
web.pid
session.lock
```

Session 目录 0700。

Socket 0600。

同一 Session 只有一个活跃 Recorder。

## 4.5.1 recorder_instance_id

每次启动从可靠系统随机源获取 16 字节。

格式：

```text
recorder-<32位十六进制>
```

结合：

- PID。
- 启动身份。
- Session Lock。
- UDS 握手。

判断所有者。

不单独依赖 PID 文件。

---

# 4.6 status.json

## 4.6.1 顶层结构

必须包含：

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

## 4.6.2 topics[] 完整元素

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
  "write_errors": 0,
  "last_operation_error": null
}
```

要求：

- name/type 来自 TopicInfo。
- topic_id/topic_epoch 正确。
- expected_hz 可为 null。
- actual_hz 未形成有效窗口时可为 null。
- gap 为 true/false/null。
- matched_publishers 表示当前已知匹配数。
- 计数非负。
- last_operation_error 可为 null 或错误对象。

remove 失败但仍 ACTIVE，必须保留在 topics[]。

## 4.6.3 topics_removed[]

```json
{
  "topics_removed": [
    {
      "topic_id": 3,
      "topic_epoch": 1,
      "name": "/camera/image_raw",
      "type": "sensor_msgs/msg/Image",
      "removed_at_ns": 1792670440000000000,
      "final_state": "REMOVED"
    }
  ],
  "topics_removed_total": 1
}
```

final_state：

```text
REMOVED
REMOVE_FAILED
```

只有实例确认退出活动集合后才进入 topics_removed。

完整历史以 topic_events 为准。

## 4.6.4 process

```json
{
  "sampling_enabled": true,
  "rss_bytes": 104857600,
  "cpu_percent": 18.5,
  "thread_count": 9,
  "fd_count": 24,
  "pool": {
    "enabled": true,
    "mode": "tls",
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

global_mutex：

```json
{
  "mode": "global_mutex",
  "tls_hit_rate": null
}
```

这是 process.pool 中对应字段的示意，不替代完整 Schema。

测试关闭 Pool：

```json
{
  "enabled": false,
  "mode": null,
  "capacity": null,
  "in_use": null,
  "available": null,
  "tls_hit_rate": null
}
```

## 4.6.5 system

```json
{
  "memory_available_bytes": 8589934592,
  "cpu_count": 8
}
```

采样不可用时相应字段为 null。

## 4.6.6 disk

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

无有效采样时使用 null。

不伪造 0。

不保留重复顶层 disk_free_gb/disk_used_percent。

---

# 4.7 actual_hz 与 gap

## 4.7.1 来源

使用 Recorder 回调到达的单调时钟。

不使用 Header Timestamp 计算实际接收频率。

## 4.7.2 窗口

默认：

```yaml
status:
  frequency_window_sec: 1
  frequency_bucket_ms: 10
```

\[
actual\_hz(t)=N(t,W)/W
\]

N 为窗口内实际收到的消息数。

## 4.7.3 有界时间桶

默认 10ms。

要求：

- 内存有界。
- 桶轮换线程安全。
- 不重复读取过期桶。
- 不保存无限逐消息时间戳。
- 快照不长时间阻塞回调。

配置：

```text
frequency_window_sec >= 0.1
frequency_bucket_ms >= 1
frequency_bucket_ms <= window_ms
bucket_count <= 1000
```

bucket_count 必须包含边界所需余量。

## 4.7.4 预热

未收到消息：

```json
"actual_hz": null
```

观测时间太短：

```json
"actual_hz": null
```

有足够非零观测时长时，可以显示 provisional：

\[
actual\_hz^{provisional}=N(t,elapsed)/elapsed
\]

elapsed 采用秒。

provisional 不参与 gap 判定。

## 4.7.5 gap

```text
true
false
null
```

expected_hz 未配置、预热、PAUSED、REMOVING 或不可评估：

```json
"gap": null
```

阈值：

\[
expected\_hz\times0.5
\]

连续两个完整评估周期低于阈值：

```json
"gap": true
```

恢复到阈值或以上：

```json
"gap": false
```

UDS status、status.json、SSE 使用同一语义。

不能把 null 转换为 false。

---

# 4.8 Status Publisher

默认：

```yaml
status:
  update_hz: 1
  max_file_bytes: 131072
  max_sse_payload_bytes: 262144
```

status.json 为紧凑单行 JSON。

通过临时文件原子替换。

失败时：

- 保留旧文件。
- 不修改旧 updated_at_ns。
- 记录 STATUS_WRITE_FAILED。
- 陈旧时间超过阈值记录 STATUS_WRITE_STALE。
- ResourceMonitor 和磁盘保护继续工作。

客户端顺序：

1. 校验 JSON。
2. 校验 schema_version。
3. 判断整个文件新鲜度。
4. 再判断磁盘样本新鲜度。

状态文件已过期时，不能因其中某个采样字段而把整个文件当作实时状态。

---

# 4.9 CLI

## 4.9.1 命令

```bash
./ros2_bag recorder

./ros2_bag cli start --topics /joint_states,/odom

./ros2_bag cli pause
./ros2_bag cli resume
./ros2_bag cli stop

./ros2_bag cli add-topic /camera/image_raw \
  --type sensor_msgs/msg/Image \
  --expected-hz 30

./ros2_bag cli remove-topic /camera/image_raw

./ros2_bag cli topics
./ros2_bag cli status

./ros2_bag cli operation-status \
  --request-id req-001
```

## 4.9.2 recorder

```bash
./ros2_bag recorder
```

前台运行。

Ctrl+C 发起受控 Shutdown。

不依赖 CLI/Web 继续存活。

## 4.9.3 cli start：已有 Recorder

1. 查找当前 Session。
2. 确认 Recorder READY。
3. 连接 UDS。
4. 发送 start。
5. 不启动第二个 Recorder。

输出：

```text
Connected to existing recorder (PID 1234, session default)
```

已有 Recorder 的日志仍由原控制终端或部署服务管理。

## 4.9.4 cli start：无 Recorder

1. 验证 Session 没有有效所有者。
2. 解析 output_root。
3. 启动受管理 Recorder。
4. 等待 READY。
5. 建立 UDS。
6. 发送 start。
7. 处理操作状态。
8. CLI 返回后 Recorder 继续运行。

输出：

```text
Started new recorder (PID 5678, session default)
```

受管理 Recorder 的 stdout/stderr 必须有明确去向。

不要求 Recorder 自行实现日志轮转。

## 4.9.5 Session 残留

- 确认旧 Recorder 已退出：安全清理残留。
- 活跃：SESSION_CONFLICT。
- 无法确认：SESSION_CONFLICT，包含诊断 details。

不能直接覆盖身份不明确的 Socket。

## 4.9.6 CLI PENDING 超时

收到 PENDING 后若等待超时：

- 退出码 4。
- 输出 request_id。
- 输出已知 operation_id。
- 明确操作可能仍在执行。
- 提示 operation-status 查询。

```text
Operation is still pending or its final state is unknown.
Request ID: req-001
Query with:
./ros2_bag cli operation-status --request-id req-001
```

不能把超时解释为操作自动取消。

## 4.9.7 CLI 退出码

```text
0 成功
1 参数错误
2 Recorder/Session 连接错误
3 控制操作失败
4 超时或终态未知
5 内部错误
```

---

# 4.10 Web / SSE / HTTP

默认：

```text
127.0.0.1:8000
```

支持：

- Start/Stop。
- Pause/Resume。
- Normal/Timed/Rolling。
- Topic 管理。
- PENDING。
- Queue/Pool。
- CPU/RSS。
- 磁盘保护。
- 错误诊断。
- Session。

Rolling PAUSED 时必须显示保留窗口继续推进的提示。

## 4.10.1 HTTP 映射

| 结果 | HTTP |
|---|---|
| 成功 | 200 |
| PENDING | 202 |
| INVALID_ARGUMENT | 400 |
| NOT_FOUND | 404 |
| INVALID_STATE | 409 |
| SESSION_CONFLICT | 409 |
| QOS_INCOMPATIBLE | 409 |
| RESOURCE_EXHAUSTED | 429 |
| DISK_FULL | 503 |
| DISK_STATE_UNKNOWN | 503 |
| STORAGE_UNAVAILABLE | 503 |
| WRITER_ERROR/INTERNAL | 500 |
| TIMEOUT | 504 |

## 4.10.2 安全

- 默认回环。
- Token。
- HttpOnly Cookie。
- SameSite。
- CSRF。
- 远程访问 TLS。
- 不在 URL 或日志泄漏凭据。

## 4.10.3 Recorder 实例变化

Web/CLI 必须观察：

```text
recorder_instance_id
```

如果实例 ID 发生变化：

1. 识别为新的 Recorder 进程。
2. 清除旧实例暂存状态。
3. 清除旧实例的本地 Topic/操作缓存。
4. 不延续旧实例 recording_state。
5. 获取新实例状态。
6. 新状态有效：显示新 Recorder。
7. 新状态不可用：显示状态不可用及最后成功时间。

**实例 ID 改变不等于新 Recorder 不可用。**

---

# 4.11 Shutdown

必须维护以下生命周期屏障：

1. 停止接纳新的有副作用控制操作。
2. 关闭 ingress。
3. 等待在途 Producer。
4. 排空合法消息。
5. 关闭 Writer。
6. 请求 ROS Executor cancel。
7. 显式等待 Recorder 管理的 Executor 线程 join。
8. 销毁任务级 ProducerToken 和 MessageQueue。
9. 释放剩余任务级 MessageHandle。
10. 验证 QueueBudget 归零。
11. Pool 进入 Stopping。
12. 停止其他可能访问 Pool 的线程。
13. 完成 TLS Cache 退出和 flush。
14. 确认固定池节点守恒。
15. 确认 fallback_in_use=0。
16. 确认 fallback_registry 为空。
17. 确认全部在途 Pool 操作结束。
18. Pool 进入 Stopped。
19. 停止其他服务。
20. 关闭 UDS。
21. 清理 Session。
22. 最后释放 PoolState。

`MultiThreadedExecutor::cancel()` 不代替 Recorder 对其启动线程的显式 join。

具体执行顺序可以在满足依赖的前提下调整，但 Stopped 屏障不能降低。

Shutdown 超时不能伪造 EXITED。

---

# 5. 工程结构、配置、Docker/CI 与发布

## 5.1 工程目录

```text
ros2_bag/
├── CMakeLists.txt
├── package.xml
├── README.md
├── .clang-format
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
│   ├── Dockerfile.analysis
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
├── tools/
│   ├── verify-submodules.sh
│   ├── verify-clean-analysis-env.sh
│   ├── verify-runtime-deps.sh
│   ├── generate-build-manifest.py
│   ├── generate-third-party-lock.sh
│   └── package.sh
│
├── tests/
│   ├── msgs/
│   │   └── ros2_bag_test_msgs/
│   ├── probes/
│   ├── support/
│   ├── unit/
│   ├── integration/
│   └── performance/
│       └── standard_profile.json
│
└── docs/
    ├── ros2_bag.md
    ├── deployment.md
    ├── recovery.md
    ├── implementation_notes.md
    ├── phase0_validation_report.md
    ├── recorded_message_data_model.md
    ├── humble_tsan_risk.md
    ├── g0_admission_review.md
    ├── g1_review.md
    ├── g2_review.md
    ├── g3_release_review.md
    └── evidence/
```

已有功能等价的 Phase 0 工具可保留原文件名，不进行无意义迁移。

不保留：

```text
RecordingSession
disk_monitor.hpp
disk_monitor.cpp
```

## 5.2 第三方依赖

四个独立 Git submodule：

```text
third_party/concurrentqueue
third_party/sockpp
third_party/json
third_party/yaml-cpp
```

固定 SHA。

验证：

- gitlink。
- URL。
- 完整 40 位 commit SHA。
- checkout 状态。
- 工作树修改。
- 未初始化状态。

## 5.3 命名检查

```bash
test -f include/ros2_bag/recording_coordinator.hpp

test -f src/recording_coordinator.cpp

grep -q 'class RecordingCoordinator' \
  include/ros2_bag/recording_coordinator.hpp

! grep -RInE \
  '^[[:space:]]*(class|struct)[[:space:]]+RecordingSession([[:space:]:{]|$)' \
  include/ src/
```

基础静态检查不能替代实际 C++17 编译。

---

# 5.4 完整默认配置

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
  max_capacity: 1000000
  max_fallback_in_use: 1024
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
  frequency_bucket_ms: 10
  max_file_bytes: 131072
  max_sse_payload_bytes: 262144
```

---

# 5.5 配置校验

必须：

1. max_messages > 0。
2. max_bytes > 0。
3. capacity_factor ∈ [1.0,2.0]。
4. capacity 计算无溢出。
5. max_capacity ∈ [1,1000000]。
6. calculated_capacity ≤ max_capacity。
7. calculated_capacity < kInvalidNodeIndex。
8. max_fallback_in_use 为合法有界值。
9. TLS 模式 local_capacity > 0。
10. TLS 模式 batch_size > 0。
11. global_mutex 模式忽略 TLS 参数。
12. Executor thread_count > 0。
13. Writer 控制队列容量 > 0。
14. disk.check_interval_sec ≥ resource.sample_interval_sec。
15. 磁盘周期为资源周期整数倍。
16. max_sample_age_sec > 磁盘采样周期。
17. unknown_after_failures > 0。
18. warning_percent < pause_percent。
19. resume_percent < pause_percent。
20. frequency_window_sec ≥ 0.1。
21. frequency_bucket_ms ≥ 1。
22. 频率桶数量含边界余量后 ≤1000。
23. status/UDS/SSE 合法最大数据可完整容纳。
24. 模型 A 的 payload 缓存配置为 0。
25. 不支持 QoS 字段必须拒绝。
26. output_root 合法。
27. 正式构建不能关闭 Pool。

## 5.5.1 正式构建的 Pool 开关

使用正式 CMake Target 控制的编译定义：

```text
ROS2_BAG_PRODUCTION_BUILD
```

正式发布构建拒绝：

```yaml
pool:
  enabled: false
```

测试构建可以关闭 Pool 做 A/B 验证。

QueueBudget 仍必须有效。

## 5.5.2 output_root

默认：

```yaml
output_root: ./recordings
```

相对路径以 Recorder 启动时工作目录为基准。

启动时解析为绝对路径。

CLI 自动管理 Recorder 时显式传递解析后的 output_root，避免工作目录差异。

---

# 5.6 四平台

| ROS 2 | Ubuntu | 架构 |
|---|---|---|
| Humble | 22.04 | AMD64 |
| Humble | 22.04 | ARM64 |
| Jazzy | 24.04 | AMD64 |
| Jazzy | 24.04 | ARM64 |

正式程序与目标 ROS 2 ABI 匹配。

---

# 5.7 Docker

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

Docker 用于构建与验证。

正式用户不要求安装 Docker。

基础镜像固定 Digest 并记录。

---

# 5.8 CI L1–L4

| 层级 | 作用 |
|---|---|
| L1 | Humble AMD64 快速构建与单测 |
| L2 | 四平台构建和基础测试 |
| L3 | Recorder/DDS/CLI/Web/恢复集成 |
| L4 | 真实硬件性能与正式发布验收 |

L2 是 CI 层级，不是 G1/G2 的同义词。

G1 不要求 ARM64 L2 已完成。

G2 必须完成 Humble/Jazzy ARM64 编译链接。

G3 必须完成四平台正式 CI 和 ARM64 原生验收。

## 5.8.1 ARM64 构建方式

优先：

1. 原生 ARM64 Runner。
2. ARM64 Runner + 目标 ROS Docker。
3. 正确 sysroot/toolchain 的真实交叉编译。
4. QEMU 模拟 ARM64 容器作为备用。

`docker buildx --platform linux/arm64` 本身不能证明是真正交叉编译。

QEMU 不替代原生性能验证。

## 5.8.2 G2 编译失败

任何正式 ARM64 构建失败：

```text
G2 = BLOCKED
```

不得标记 Phase 2 完成。

---

# 5.9 third_party.lock

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

与实际 checkout 一致。

---

# 5.10 build-manifest

每平台清单：

```text
build-manifest-humble-amd64.json
build-manifest-humble-arm64.json
build-manifest-jazzy-amd64.json
build-manifest-jazzy-arm64.json
```

至少记录：

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

## 5.10.1 汇总清单

```json
{
  "schema_version": 1,
  "source_commit": "<release-sha>",
  "third_party_shas": {
    "concurrentqueue": "<sha>",
    "sockpp": "<sha>",
    "json": "<sha>",
    "yaml-cpp": "<sha>"
  },
  "platforms": {
    "humble-amd64": {
      "manifest": "build-manifest-humble-amd64.json",
      "binary": "humble/amd64/ros2_bag_recorder",
      "binary_sha256": "<sha256>"
    },
    "humble-arm64": {
      "manifest": "build-manifest-humble-arm64.json",
      "binary": "humble/arm64/ros2_bag_recorder",
      "binary_sha256": "<sha256>"
    },
    "jazzy-amd64": {
      "manifest": "build-manifest-jazzy-amd64.json",
      "binary": "jazzy/amd64/ros2_bag_recorder",
      "binary_sha256": "<sha256>"
    },
    "jazzy-arm64": {
      "manifest": "build-manifest-jazzy-arm64.json",
      "binary": "jazzy/arm64/ros2_bag_recorder",
      "binary_sha256": "<sha256>"
    }
  }
}
```

## 5.10.2 权威性

汇总 manifest 是正式发布索引。

单平台 manifest 是平台构建证据。

两者冲突时：

- 发布校验失败。
- 不静默覆盖。
- 重新核实或重建。

## 5.10.3 package.sh

流程：

1. 收集四个平台二进制。
2. 收集四个平台 manifest。
3. 比较 source_commit。
4. 比较 third_party SHA。
5. 检查架构和 ROS_DISTRO。
6. 校验 binary SHA256。
7. 执行动态依赖检查。
8. 生成汇总 manifest。
9. 打包。
10. 生成 SHA256SUMS。

---

# 5.11 正式发布包

```text
ros2_bag/
├── ros2_bag
├── bag_cli
├── bag_web
├── scripts/
│   ├── launch_recorder.sh
│   ├── bag_cli.py
│   ├── bag_ipc.py
│   ├── web_server.py
│   └── status_parser.py
├── config/
├── web/
├── docs/
│   ├── deployment.md
│   └── recovery.md
├── README.md
├── LICENSES/
├── third_party.lock
├── build-manifest.json
├── build-manifest-humble-amd64.json
├── build-manifest-humble-arm64.json
├── build-manifest-jazzy-amd64.json
├── build-manifest-jazzy-arm64.json
├── humble/
│   ├── amd64/ros2_bag_recorder
│   └── arm64/ros2_bag_recorder
└── jazzy/
    ├── amd64/ros2_bag_recorder
    └── arm64/ros2_bag_recorder
```

三个顶层入口为 Bash：

```bash
#!/usr/bin/env bash
```

可执行权限 0755。

不要求用户重新编译 C++。

## 5.11.1 启动

```bash
tar -xJf ros2_bag.tar.xz

cd ros2_bag

./ros2_bag recorder
```

另一个终端：

```bash
./ros2_bag cli start --topics /joint_states,/odom
```

连接已运行 Recorder，不创建重复进程。

---

# 6. G0–G3 实施与验收

## 6.1 Phase 0 / G0

目标：

- 工程基础。
- Submodule 锁定。
- Docker AMD64。
- GenericSubscription 所有权探针。
- Queue move-only 探针。
- WriterAdapter 探针。
- Pool TLS/Plan B 探针。
- AC-55。
- Humble TSan 风险记录。
- 技术决策签审。

## 6.1.1 G0 判据

| 项目 | 通过要求 |
|---|---|
| SerializedPayload | Humble/Jazzy AMD64 所有权、ASan、数据一致性 |
| Pool | TLS/单锁技术可行、生命周期和守恒 |
| Queue | move-only、BudgetLease、Token 重建 |
| WriterAdapter | Humble/Jazzy MCAP 写入及读回 |
| AC-55 | 固定 2×2 干净环境解析 |
| Docker | Humble/Jazzy AMD64 构建 |
| L1 | 实际远端 CI 成功记录 |
| Humble TSan | 明确的 G0 风险处置 |

## 6.1.2 Queue 最小测试集

至少：

- move-only 编译及运行。
- BudgetLease 正常释放 100 次。
- Pool 失败回滚 100 次。
- 入队失败回滚 100 次。
- Queue 销毁释放 100 次。
- ProducerToken 至少 3 轮重建。
- 累计消息移动/入队/出队 ≥10000 次。
- 最终配额守恒。

100 万次正式压力属于 G1。

## 6.1.3 G0 有条件批准

如果 Humble 全栈 TSan 仍存在经确认的依赖栈告警，且允许开发继续：

```text
G0 Admission = ALLOWED
TSan Disposition = APPROVED_WITH_CONDITIONS
```

必须在：

```text
docs/g0_admission_review.md
```

记录：

- 条件清单。
- 负责人。
- G3 前的关闭要求。
- 现有证据。
- 剩余风险。
- 批准日期。

不得因为批准继续开发就将 TSan 状态改为 PASS。

---

# 6.2 Phase 1 / G1

实施：

1. RecordedMessage。
2. PoolState。
3. RecordedMessagePool。
4. MessageHandle。
5. fallback_registry。
6. QueueBudget。
7. BudgetLease。
8. MessageQueue。
9. TopicManager。
10. WriterAdapter。
11. RecordingCoordinator。
12. Normal/Timed/Rolling。
13. 动态 Topic。
14. manifest/topic_events。
15. ResourceMonitor。
16. 磁盘保护。
17. Status Publisher。

验收：

- Humble/Jazzy AMD64 构建。
- 单元测试。
- 核心集成测试。
- Pool ASan。
- 项目自身适用 TSan。
- Queue ≥100 万次压力。
- fallback_registry 竞争和异常安全。
- Pool Stopping/Stopped。
- Stop→Start。
- Writer 故障。
- Topic 生命周期。
- 恢复。

---

# 6.3 Phase 2 / G2

实施：

- UDS。
- CLI。
- Web/SSE。
- Session。
- 用户操作流程。
- 完整配置。
- 错误展示。
- 部署与恢复文档。

强制：

- Humble ARM64 编译链接。
- Jazzy ARM64 编译链接。
- CLI/Web/UDS/Session 功能通过。

ARM64 编译失败：

```text
G2 = BLOCKED
```

---

# 6.4 Phase 3 / G3

必须：

- 四平台 CI。
- ARM64 原生功能。
- ARM64 原生多线程压力。
- ARM64 30 分钟标准负载。
- AMD64 真实硬件性能。
- PlotJuggler 2×2。
- AC-55 最终回归。
- Humble TSan 最终处置。
- AC-01～AC-94。
- 正式发布包。
- G3 Release Decision=APPROVED。

ARM64 ASan/TSan 在目标工具链支持时执行。

不能将不可运行的 Sanitizer 测试伪造 PASS。

---

# 6.5 Sanitizer 阶段

| 测试 | G0 | G1 | G2 | G3 |
|---|---|---|---|---|
| Pool 探针 ASan | 必须 | 回归 | — | 必要时 |
| Queue 探针 TSan | 必须 | 回归 | — | 必要时 |
| 正式 Pool ASan | — | 必须 | — | 回归 |
| 正式 Pool TSan | — | 必须 | — | 回归 |
| 正式 Queue ASan/TSan | — | 必须 | — | 回归 |
| Humble 全栈 TSan | 风险处置 | 持续定位 | — | 最终处置 |
| ARM64 编译 | — | 可选 | 必须 | 回归 |
| ARM64 原生 ASan | — | — | — | 环境支持时 |
| ARM64 原生 TSan | — | — | — | 环境支持时 |
| ARM64 原生压力 | — | — | — | 必须 |

---

# 6.6 AC-01～AC-68

| ID | 验收项 | 通过要求 |
|---|---|---|
| AC-01 | 四平台 | 构建、运行 |
| AC-02 | 标准/自定义消息 | MCAP 内容正确 |
| AC-03 | QoS | 自动适配、覆盖、诊断 |
| AC-04 | 时间戳 | WriterAdapter 映射正确 |
| AC-05 | Normal | start/stop/close |
| AC-06 | Timed | 单调计时、到期 |
| AC-07 | Rolling | 分片与保留 |
| AC-08 | Segment | 独立 rosbag2 可读取 |
| AC-09 | manifest | 原子提交、恢复 |
| AC-10 | Snapshot | 双重预算 |
| AC-11 | Queue | move-only MPSC |
| AC-12 | QueueBudget | 字节/条数不超限 |
| AC-13 | Generation | Task 隔离 |
| AC-14 | Stop | Producer 屏障 |
| AC-15 | Writer Error | 安全 ERROR |
| AC-16 | UDS | 命令响应正确 |
| AC-17 | 幂等 | 不重复副作用 |
| AC-18 | operation_status | 超时后可查询 |
| AC-19 | Session | 冲突及隔离 |
| AC-20 | status.json | Schema、原子更新 |
| AC-21 | CLI | Ctrl+C、退出码、日志 |
| AC-22 | Web | SSE、安全、控制 |
| AC-23 | 磁盘保护 | ResourceMonitor + Coordinator |
| AC-24 | Fast DDS | 发现、匹配、实际消息 |
| AC-25 | 异常恢复 | DELETING/INCOMPLETE |
| AC-26 | Submodule | SHA 锁定 |
| AC-27 | 动态依赖 | 可解析、可加载 |
| AC-28 | Python | 标准库 |
| AC-29 | 发布 | 统一压缩包 |
| AC-30 | 资源清理 | 无线程/FD/Pool 泄漏 |
| AC-31 | Web 安全 | Token/CSRF/TLS |
| AC-32 | ARM64 | 30 分钟负载 |
| AC-33 | 动态 Topic | RECORDING/PAUSED |
| AC-34 | Topic 统计 | Epoch 归属 |
| AC-35 | Topic 隔离 | Generation |
| AC-36 | Topic 幂等 | 重试语义 |
| AC-37 | Writer 注册 | Schema/Channel |
| AC-38 | 并发控制 | Pause/Resume/Stop |
| AC-39 | Topic 添加失败 | 不错误开放 ingress |
| AC-40 | CLI/Web | UDS 语义一致 |
| AC-41 | Topic 压力 | 3000 次增删 |
| AC-42 | 空 Topic | 不持续生成空分片 |
| AC-43 | Topic Event | PREPARED/终态 durable |
| AC-44 | Session 所有者 | PID/实例身份 |
| AC-45 | 自动恢复 | 仅系统暂停 |
| AC-46 | 删除恢复 | DELETING 幂等 |
| AC-47 | 同名重添加 | 新 Epoch |
| AC-48 | 零消息 | start/stop/manifest 一致 |
| AC-49 | PlotJuggler | 2×2 |
| AC-50 | ros2 bag play | 同发行版 |
| AC-51 | MCAP Schema | 依赖闭包 |
| AC-52 | 命名 | 静态检查及编译 |
| AC-53 | ResourceMonitor | 正常及异常采样 |
| AC-54 | status 容量 | 最大状态完整 |
| AC-55 | 自包含 | 无原消息包 |
| AC-56 | 多 PENDING | 容量/顺序/终态 |
| AC-57 | 重启 | 新进程 IDLE |
| AC-58 | 操作边界 | Accepted/Effective/Committed |
| AC-59 | Shutdown 超时 | 不伪造 EXITED |
| AC-60 | 实例身份 | 128 位随机及 PID 重用 |
| AC-61 | Topic Event 故障 | 安全 ERROR |
| AC-62 | Coordinator 命名 | 无 RecordingSession |
| AC-63 | 默认 Session | default |
| AC-64 | 历史错误 | 不伪造根因 |
| AC-65 | 状态快照 | 不逐消息深拷贝 |
| AC-66 | 资源采样干扰 | 不阻塞回调 |
| AC-67 | 联合恢复 | PREPARED/DELETING/INCOMPLETE |
| AC-68 | 发布结构 | 路径、权限、文档 |

## 6.6.1 AC-12

至少 10 个生产者。

注入：

- 预算不足。
- Pool 分配失败。
- 入队失败。
- Writer 错误。
- Stop。
- Generation 违规。

最终：

```text
used_bytes = 0
used_messages = 0
```

## 6.6.2 AC-41

至少 3000 次动态 Topic 增删。

记录：

- RSS。
- FD。
- 线程。
- TopicInfo。
- Pool。
- topic_events。
- Schema/Channel。

RSS 每秒采样。

完整 5 分钟线性回归窗口。

## 6.6.3 AC-48

必须测试：

1. start 后立即 stop，无消息。
2. allow_empty_topics=true。
3. 所有 Topic 移除。
4. PAUSED 后无消息。
5. Writer 已打开但没有消息写入。

第五项构造方法：

1. Start 当前 Task。
2. 不启动消息发布者。
3. add_topic 触发 Writer TopicMetadata 注册。
4. 确认 Writer 实际打开 Segment。
5. 不发送消息。
6. Stop。
7. 检查生成的 Segment 是否有效且消息数为 0。
8. 检查 manifest 与实际文件一致。

如果 Writer 实现采用延迟打开，不能假设 add_topic 一定打开 Segment；测试应通过专用受控流程明确触发 OPEN_SEGMENT。

## 6.6.4 AC-54

构造：

```text
64 ACTIVE
32 REMOVED
32 PENDING
8 secondary_errors
完整 Pool Stats
完整 ResourceSnapshot
最大合法字符串
```

验证：

- status.json。
- UDS。
- SSE。

不静默截断。

## 6.6.5 AC-66

10×1000Hz：

- 普通遥测关闭。
- 普通遥测开启。
- 注入 10ms 采样延迟。

磁盘保护继续有效。

资源采样不得阻塞高频回调。

---

# 6.7 AC-69～AC-80

| ID | 验收 | 通过要求 |
|---|---|---|
| AC-69 | Pool 容量 | 固定且合法 |
| AC-70 | TLS 快路径 | 命中不取全局锁 |
| AC-71 | Batch | 锁频率、延迟 |
| AC-72 | Pool 回退 | 稳态目标 0 |
| AC-73 | 生命周期 | PoolState/TLS/登记表守恒 |
| AC-74 | RAII | 池/回退/跨线程/异常 |
| AC-75 | Pool A/B | CPU/RSS/P99 |
| AC-76 | Buffer | A 模型不缓存载荷 |
| AC-77 | 延迟消费 | 无 UAF |
| AC-78 | 数据模型 | 签审 APPROVED |
| AC-79 | BudgetLease | 恰好一次 |
| AC-80 | payload 拷贝 | 无 Recorder 新增完整拷贝 |

## 6.7.1 AC-72

预热同时满足：

- 至少 10 秒。
- 至少完成 1000 条有效消息处理。

即：

```text
预热时间 = max(10 秒, 达到 1000 条有效消息完成的时间)
```

预热后统计：

```text
fallback_count 增量目标 = 0
```

必须记录峰值 in_use 和 fallback_in_use。

## 6.7.2 AC-73

正常 Stopped：

```text
in_use = 0
fallback_in_use = 0
fallback_registry.size() = 0
tls_free = 0
global_free = capacity
internal_error_count = 0
```

## 6.7.3 AC-74

G1 AMD64：

- ASan。
- 项目自身适用 TSan。
- Pool Running/Stopping/Stopped。
- TLS 析构。
- NodeIndex。
- fallback_registry 并发创建/释放。
- fallback_registry 插入失败。
- 重复归还诊断。
- Writer 异常。
- BudgetLease。

G3 ARM64：

- 原生多线程压力。
- 可用的 Sanitizer 回归。

## 6.7.4 AC-75

同样 SerializedPayload 模型：

- A：无 Pool。
- B：有 Pool。

其他条件一致。

比较：

- CPU。
- RSS。
- P99。
- 分配数量。
- QueueBudget。
- 最终 MCAP 数量。

## 6.7.5 AC-77

所有权组：

- Writer 注入 500ms 延迟。
- 回调正常返回。
- Writer 延迟消费。
- ASan。
- 数据一致。

压力组：

- 1000Hz。
- 累计 10 分钟。
- 短时延迟。
- 统计丢弃。
- MCAP 读回。

所有权失败和队列容量丢弃分别判定。

---

# 6.8 AC-81～AC-86

| ID | 验收 | 通过要求 |
|---|---|---|
| AC-81 | Docker | 四平台构建 |
| AC-82 | CI | 四平台同 Release SHA |
| AC-83 | Fast DDS | 容器/宿主机实际收发 |
| AC-84 | 依赖锁定 | SHA 一致 |
| AC-85 | Release | 压缩包/依赖/哈希 |
| AC-86 | 真实硬件 | AMD64/ARM64 性能 |

---

# 6.9 AC-87～AC-94

| ID | 验收 | 通过要求 |
|---|---|---|
| AC-87 | Pool Plan B | 正式方案安全 |
| AC-88 | 频率 | 窗口、预热、gap |
| AC-89 | UNKNOWN | 失败、陈旧、保护、恢复 |
| AC-90 | Cleanup | 超时不阻塞 Coordinator |
| AC-91 | Generation | 正常切换不混写 |
| AC-92 | 状态文件 | 原子性、陈旧、实例变化 |
| AC-93 | ProducerToken | 队列重建无 UAF |
| AC-94 | 变更追溯 | G0–G3/依赖/发布 |

## 6.9.1 AC-88

精确时间戳滑动窗口为参考。

默认 10ms 桶。

要求：

```text
稳态平均相对偏差 < 2%
P95 相对偏差 < 5%
```

额外记录 Max。

接近零时使用绝对误差。

## 6.9.2 AC-89

覆盖：

- 初次采样失败。
- 连续失败。
- 样本过期。
- statvfs 阻塞。
- CRITICAL。
- UNKNOWN。
- USER 暂停。
- 系统自动恢复。

初次无有效安全样本：

```text
start → DISK_STATE_UNKNOWN
```

## 6.9.3 AC-90

通过可注入删除接口延迟 35 秒，验证 Coordinator 不阻塞。

另行验证实际文件系统异常。

不宣称任意文件系统调用可安全强制取消。

## 6.9.4 AC-91

至少 100 次 Stop→Start。

正常：

```text
dropped_stale_generation = 0
```

受控注入旧消息：

- 不写入新 Task。
- 计数增加。
- 首次设置 GENERATION_VIOLATION。
- 安全 ERROR。

## 6.9.5 AC-92

注入：

- status.json 写失败。
- 文件损坏。
- 文件缺失。
- 更新陈旧。
- recorder_instance_id 改变。

实例变化：

1. 清理旧 UI 状态。
2. 重新读取新状态。
3. 新状态有效则正常显示新 Recorder。
4. 新状态无效才显示不可用。

不能把新进程的正常启动误判为故障。

## 6.9.6 AC-93

重复：

```text
start → stop → start → stop
```

Executor 线程复用。

旧 Token 不访问旧队列的已释放存储，也不访问新队列。

---

# 6.10 真实硬件和 SIGKILL 恢复

## 6.10.1 ARM64

真实设备：

```text
10 Topics × 1000Hz
30 分钟
```

记录：

- CPU。
- RSS。
- FD。
- 线程。
- QueueBudget。
- Pool。
- Writer 延迟。
- 最终 MCAP 数量。

## 6.10.2 SIGKILL 恢复

至少 10 次：

1. 录制约 30 秒。
2. 发布带递增序号消息。
3. SIGKILL Recorder。
4. 重启恢复。
5. 检查 manifest。
6. 检查 topic_events。
7. 实际读取 MCAP。
8. 提取最后恢复序号。
9. 对比发送和接收序号。
10. 记录丢失窗口。

`ros2 bag info` 可以提供总量参考，但不能代替逐消息序号验证。

---

# 7. Humble TSan、风险与签审

## 7.1 风险文件

```text
docs/humble_tsan_risk.md
```

状态：

```text
OPEN
MITIGATED
ACCEPTED
RESOLVED
```

OPEN：尚未完成根因及影响处置。

MITIGATED：已采取有效措施并通过验证。

ACCEPTED：明确剩余风险并完成正式审批，不代表已修复。

RESOLVED：修复或消除，并通过回归。

## 7.2 G0 有条件继续开发

最低要求：

1. 最小 ROS 节点复现证据。
2. 项目自有 Pool/Queue/SerializedPayload 适用 ASan/TSan 结果。
3. 告警类型及依赖版本。
4. 风险影响分析。
5. G3 关闭计划。
6. Tech Lead 和项目负责人批准。

若依赖栈风险仍 OPEN：

```text
G0 Admission = ALLOWED
TSan Disposition = APPROVED_WITH_CONDITIONS
```

必须附条件清单。

该状态不等于技术风险已经通过。

## 7.3 G3 TSan

最终可以是：

- RESOLVED。
- MITIGATED 且验证有效。
- 正式 ACCEPTED 且剩余风险可接受。

确认会造成严重运行安全、内存安全或数据完整性损坏的缺陷，不能仅靠签字跳过修复。

## 7.4 缓解措施

根据调用栈评估：

- Fast DDS 版本。
- SHM/UDP-only 对比。
- spdlog 依赖。
- 编译与插桩配置。
- 受影响功能限制。

禁用 SHM 只是实验候选，不是预设修复。

必须验证 DDS 发现、接收、吞吐、延迟、CPU 和数据完整性。

## 7.5 单人项目签审

允许同一实际负责人担任 Tech Lead 和 Project Owner，但要明确标注单人签审。

建议 G3 前进行至少一次独立 code review 或设计 review。

若无法安排：

- 如实记录。
- 不伪造审查人。
- 不用缺少外部审查替代强制测试。
- 发布负责人依据剩余风险和全部验收证据决定是否发布。

## 7.6 G0 文件

```text
docs/g0_admission_review.md
```

至少：

```text
Document Version:
Source Commit:
Environment:

SerializedPayload:
  Test Status:
  Decision:
  Evidence:

Pool:
  Test Status:
  Decision:
  Evidence:

Queue:
  Test Status:
  Decision:
  Evidence:

WriterAdapter:
  Test Status:
  Evidence:

AC-55:
  Test Status:
  Evidence:

Docker/L1:
  Test Status:
  Evidence:

Humble TSan:
  Risk Status:
  Disposition:
  Evidence:
  Conditions to Close Before G3:

G0 Admission:
  ALLOWED / BLOCKED

Tech Lead:
Project Owner:
Review Date:
```

## 7.7 G1 文件

```text
docs/g1_review.md
```

必须有实际测试、问题、证据、审查和：

```text
G1 Admission = ALLOWED / BLOCKED
```

## 7.8 G2 文件

```text
docs/g2_review.md
```

必须包含 Humble/Jazzy ARM64 编译链接证据。

```text
G2 Admission = ALLOWED / BLOCKED
```

## 7.9 G3 文件

```text
docs/g3_release_review.md
```

必须包含：

- 四平台 CI。
- ARM64 原生验证。
- 性能。
- AC-01～AC-94。
- PlotJuggler。
- AC-55。
- Humble TSan 最终处置。
- Release SHA。
- build-manifest。
- 发布包。
- 发布签审。

最终：

```text
Release Decision = APPROVED
```

CONDITIONAL 不等于正式批准发布。

---

# 8. 交付与正式发布

## 8.1 代码与功能

1. Recorder。
2. TopicInfo。
3. TopicManager。
4. RecordingCoordinator。
5. RecordedMessage。
6. SerializedPayload。
7. PoolState。
8. RecordedMessagePool。
9. MessageHandle。
10. fallback_registry。
11. QueueBudget。
12. BudgetLease。
13. MessageQueue。
14. WriterAdapter。
15. Normal/Timed/Rolling。
16. 动态 Topic。
17. Snapshot。
18. manifest/topic_events。
19. ResourceMonitor。
20. 磁盘保护。
21. Status Publisher。
22. UDS。
23. CLI。
24. Web/SSE。
25. Session。
26. Cleanup/恢复。

## 8.2 验证与签审

必须：

1. 数据模型签审。
2. Pool 模式签审。
3. Phase 0 验证报告。
4. `docs/g0_admission_review.md`。
5. `docs/g1_review.md`。
6. `docs/g2_review.md`。
7. `docs/g3_release_review.md`，Release Decision=APPROVED。
8. `docs/humble_tsan_risk.md`。
9. AC-55。
10. PlotJuggler 2×2。
11. AMD64/ARM64 性能。
12. Pool/Queue Sanitizer。
13. AC-01～AC-94。
14. CI/依赖/构建证据。

## 8.3 正式发布制品

```text
ros2_bag.tar.xz
SHA256SUMS
```

SHA256SUMS 使用标准 sha256sum 格式。

用户执行：

```bash
sha256sum -c SHA256SUMS
```

V1.0 不强制提供 GPG 签名。

HTTPS 提供传输保护，但未经独立签名的 SHA256SUMS 不代表独立密码学来源认证。

## 8.4 G3 发布条件

以下三类全部满足：

1. 代码与功能。
2. 验证与签审。
3. 正式发布制品。

任何必需项缺失均阻止 G3 APPROVED。

---

# 附录 A：版本历史

| 版本 | 核心变更 |
|---|---|
| v4.0 | 初始 Recorder 架构 |
| v4.1 | 控制、状态和恢复 |
| v4.2 | 持久化、Schema、发布 |
| v4.2.1 | SerializedPayload、QueueBudget、ProducerToken |
| v4.3 | RecordedMessagePool |
| v4.3.1 | ResourceMonitor、Docker/CI |
| v4.3.2 | AC-55、actual_hz、UNKNOWN |
| v4.3.3 | Pool Plan B、Generation、风险 |
| v4.3.4 | G0–G3、模型 A、ARM64 分阶段 |
| v4.3.5 | PoolState/NodeIndex、状态边界 |
| v4.3.6 | Pool 三态、PENDING、构建清单 |
| v4.3.7 | 恢复 JSON/状态合同、统一 Pool/fallback 所有权 |
| **v4.3.8** | **固定 fallback_registry、std::abort fail-fast、过渡状态矩阵、响应与验收细节** |

## A.1 向后兼容

V1.0 发布前允许调整尚未发布协议。

新增字段必须在正式默认配置和 Schema 中定义。

发布后的不兼容修改按 schema_version 管理。

---

# 附录 B：核心不变量

## B.1 QueueBudget

```text
0 <= used_bytes <= max_bytes
0 <= used_messages <= max_messages
```

正常 Task 结束：

```text
used_bytes = 0
used_messages = 0
```

## B.2 Pool

TLS：

```text
capacity = in_use + global_free + tls_free
```

global_mutex：

```text
tls_free = 0
capacity = in_use + global_free
```

## B.3 回退登记

```text
fallback_registry.size()
    == fallback_in_use
```

该等式要求在相关同步保护下的静止状态验证。

并且：

```text
fallback_registry.size()
    <= max_fallback_in_use
```

## B.4 Pool Stopped

```text
in_use = 0
fallback_in_use = 0
fallback_registry.size() = 0
tls_free = 0
global_free = capacity
```

没有在途 acquire/release。

没有 TLS Cache 访问。

## B.5 Generation

正常 Stop→Start：

```text
dropped_stale_generation = 0
```

违规：

```text
GENERATION_VIOLATION
```

## B.6 Topic Epoch

同名重添加生成新 topic_epoch。

## B.7 Writer

只有 Writer Thread 调用 rosbag2 Writer。

## B.8 PENDING

```text
state = PENDING
code = null
```

不表示成功。

## B.9 Operation 身份

同一逻辑操作：

```text
UDS operation_id
    ==
operation_status operation_id
    ==
topic_events operation_id
```

## B.10 G0/G3

```text
G0 ALLOWED != G3 PASS
```

允许开发不等于允许发布。

---

# 附录 C：异常场景

| 异常 | 正式处理 |
|---|---|
| QueueBudget 不足 | dropped_capacity |
| Pool Node 不足 | 受控回退 |
| 回退容量耗尽 | 丢弃并释放 BudgetLease |
| 回退登记失败 | RAII 回滚 |
| 回退登记表不守恒 | 内部不变量错误 |
| NodeIndex 越界 | fail-fast |
| 已确认重复归还 | fail-fast |
| Pool Stopping | 拒绝 acquire，接受合法 release |
| Pool Stopped 后释放 | fail-fast |
| ProducerToken 失效 | 禁止访问旧 Queue |
| Writer Error | 安全 ERROR |
| Writer 注册成功但订阅失败 | 保留 Channel，记录失败 |
| RESUMING 中 Stop | 登记停止意图，安全收尾 |
| STOPPING 中重复 Stop | 复用正在执行的 Stop |
| 过渡状态 Shutdown | 登记退出意图 |
| Topic Event 提交失败 | ERROR |
| remove 失败仍 ACTIVE | 留在 topics |
| manifest 写入失败 | 保留旧文件 |
| 初次磁盘采样失败 | DISK_STATE_UNKNOWN |
| 磁盘 CRITICAL | SYSTEM_DISK |
| 磁盘 UNKNOWN | SYSTEM_UNKNOWN |
| statvfs 阻塞 | Coordinator 判断样本陈旧 |
| Cleanup 阻塞 | 保留 DELETING |
| status.json 损坏 | Web 显示不可用 |
| Recorder 实例变化 | 清除旧实例缓存，加载新状态 |
| PREPARED 未决 | UNCERTAIN |
| Rolling PAUSED | 保留窗口继续推进 |
| Generation 违规 | 计数、ERROR |
| A/B/C/D 全失败 | G0 BLOCKED，重审架构 |
| ARM64 编译失败 | G2 BLOCKED |
| ARM64 原生未完成 | G3 BLOCKED |
| 发布清单冲突 | 拒绝发布 |

---

# 附录 D：C++17 代码风格与质量要求

## D.1 原则

优先级：

1. 可读性。
2. 简洁性。
3. 性能微优化。
4. 新特性。

严格 C++17。

不引入 C++20/23。

## D.2 规范

遵循：

- Google C++ Style Guide。
- 项目 `.clang-format`。

## D.3 命名

| 对象 | 格式 |
|---|---|
| 类型 | PascalCase |
| 函数 | snake_case |
| 变量 | snake_case |
| 成员变量 | snake_case_ |
| 常量 | kConstantName |
| 宏 | UPPER_SNAKE |

## D.4 排版

- 函数参数每项一行。
- 函数实参每项一行。
- 构造初始化列表每项一行。
- 不使用高密度单行代码。
- 不写嵌套三元。
- 避免过长链式调用。

## D.5 include

顺序：

1. 对应头文件。
2. C 系统头。
3. C++ 标准库。
4. 第三方。
5. 项目头文件。

组间空行。

## D.6 禁止

- concepts。
- coroutines。
- modules。
- ranges。
- C 风格强制转换。
- 头文件 using namespace。
- 业务代码裸 new/delete。
- 无必要复杂模板。
- 魔法数字。
- 危险隐式转换。
- 无依据微优化。

## D.7 宏

允许：

- 头文件保护。
- 必需的平台条件编译。
- 第三方 API 要求的宏。

业务逻辑不用宏替代普通函数或 constexpr。

## D.8 注释

文件头：

- 文件职责。
- 所属模块。
- 关键约束。

Public 接口：

- 职责。
- 参数。
- 返回值。
- 错误/异常。
- 线程安全性。

注释重点解释原因与边界。

TODO/FIXME：

```cpp
// TODO(name): 说明 (issue #123)
```

没有真实负责人和 Issue 时，不随意留下 TODO。

---

# 附录 E：v4.3.5～v4.3.8 内容保留与变更审计

| 检查内容 | 既有来源 | v4.3.8 处理 |
|---|---|---|
| Recorder 总体架构 | v4.3.5 | 保留 |
| TopicQos / RepeatPolicy | v4.3.5 | 保留 |
| TopicInfo | v4.3.5 | 保留 |
| SerializedPayload 模型选择 | v4.3.5–v4.3.7 | 保留 |
| RecordedMessage | v4.3.5–v4.3.7 | 保留 |
| PoolState / NodeIndex | v4.3.5 | 保留 |
| MessageDeleter | v4.3.5–v4.3.7 | 保留并强化回退身份 |
| PoolRuntimeState | v4.3.6 | 保留 |
| Pool TLS / Plan B | v4.3.5 | 保留 |
| QueueBudget / BudgetLease | v4.3.5 | 保留 |
| ProducerToken | v4.3.5 | 保留 |
| WriterAdapter | v4.3.5 | 保留 |
| Writer 公平调度 | v4.3.6 | 保留 |
| Normal / Timed / Rolling | v4.3.5 | 保留 |
| Rolling PAUSED 清理 | v4.3.6 | 保留 |
| manifest | v4.3.5–v4.3.7 | 保留 |
| topic_events JSONL | v4.3.5、v4.3.7 | 保留 |
| operation_id | v4.3.7 | 保留 |
| ResourceMonitor | v4.3.5 | 保留 |
| 磁盘 UNKNOWN | v4.3.5 | 保留 |
| paused_reason | v4.3.5–v4.3.7 | 保留 |
| 稳态命令矩阵 | v4.3.5、v4.3.7 | 保留 |
| 过渡状态矩阵 | v4.3.8 | 新增完整定义 |
| ERROR 后 start | v4.3.5、v4.3.7 | 保留 |
| UDS 三态 | v4.3.5–v4.3.7 | 保留 |
| FAILED 未接纳响应 | v4.3.7 | 保留并补客户端规则 |
| 四类 Schema | v4.3.7 | 保留 |
| topics[] | v4.3.5、v4.3.7 | 保留完整结构 |
| topics_removed[] | v4.3.5、v4.3.7 | 保留 |
| process.pool | v4.3.5–v4.3.7 | 保留 |
| system | v4.3.5、v4.3.7 | 保留 |
| disk | v4.3.5 | 保留 |
| actual_hz / gap | v4.3.5–v4.3.7 | 保留 |
| CLI 两种 start | v4.3.5 | 保留 |
| CLI PENDING 超时 | v4.3.7 | 保留 |
| Web/SSE/HTTP | v4.3.5 | 保留 |
| Shutdown | v4.3.5–v4.3.7 | 保留 |
| Docker/CI | v4.3.5 | 保留 |
| ARM64 门槛 | v4.3.5 | 保留 |
| third_party.lock | v4.3.5 | 保留 |
| build-manifest | v4.3.6–v4.3.7 | 保留 |
| 发布结构 | v4.3.5 | 保留 |
| Humble TSan | v4.3.5 | 保留 |
| G0–G3 | v4.3.5 | 保留 |
| AC-01～AC-94 | v4.3.5 | 全部保留 |
| C++17 风格 | v4.3.5 | 保留 |
| fallback_registry | v4.3.8 | 新增具体结构及异常安全 |
| std::abort fail-fast | v4.3.8 | 新增具体实现合同 |
| Pool 回退关闭守恒 | v4.3.8 | 增加登记表清空条件 |
| CLI 实例提示 | v4.3.8 | 新增 |
| AC-48 构造 | v4.3.8 | 补充 |
| AC-92 实例变化 | v4.3.8 | 补充 |
| G0 条件清单 | v4.3.8 | 补充 |
| G3 审批条件 | v4.3.8 | 补充 |

本表用于检查主要技术合同是否得到保留，不替代正式文件之间的逐行 diff 与人工审查。

---

**文档版本：v4.3.8**

**文档状态：FROZEN — Phase 1 Implementation & Execution Contract**

**产品版本：V1.0**

**G0：须依据现有验证证据完成 SerializedPayload、Pool 模式及风险处置签审。**

**G1：正式 Recorder 核心开发、Pool/Queue 并发正确性与 AMD64 验收。**

**G2：完整 CLI/Web/Session 及 ARM64 编译链接。**

**G3：四平台 CI、ARM64 原生验收、Humble TSan 最终处置及正式 V1.0 发布。**

**文档结束。**
