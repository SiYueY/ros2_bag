> **CURRENT IMPLEMENTATION NOTE:** 本规范定义未来完整四层系统的技术选择；当前仓库处于 Recorder Only 阶段，尚未实现 Drogon Server、HTTPX Client、CLI/TUI/Web。该注释不改变原有技术选型。

# ros2_bag 系统技术选型规范

*System Technology Selection Specification*

| 文档属性 | 内容 |
|---|---|
| 项目名称 | ros2_bag |
| 文档版本 | v4.2.1 |
| 文档类型 | 系统级技术选型规范 |
| 文档状态 | DRAFT（开发中，技术选择可调整） |
| 目标产品版本 | V1.0 |
| 编制日期 | 2026-10-25 |
| 目标操作系统 | Ubuntu 22.04 / 24.04 LTS |
| 目标 ROS 2 | Humble / Jazzy |
| 目标架构 | AMD64 / ARM64 |
| 技术原则 | 成熟稳定、轻量低依赖、职责匹配、开放标准、长期可维护 |

---

# 1. 概述

## 1.1 文档目的

本规范用于确定 `ros2_bag` 的系统级技术栈，为系统架构设计、软件开发、依赖管理和产品交付提供统一、稳定的技术基线。

本规范重点解决以下问题：

1. 确定系统技术分层及各层开发语言。
2. 选择与系统职责相匹配的框架、第三方库和标准协议。
3. 比较主要候选技术，说明选型依据及技术权衡。
4. 明确所选技术的能力边界、适用限制和引入成本。
5. 确定第三方依赖的准入、版本和维护原则。
6. 确定基础平台、构建、链接和交付技术策略。
7. 为后续技术升级和替换提供一致的决策依据。

技术选型应尽可能保持长期稳定，不应随着业务逻辑、内部模块结构或源码组织方式的变化而频繁修订。

## 1.2 技术选型原则

### 成熟稳定

优先采用经过实际应用验证、具有清晰 API、良好维护记录和完善文档的开源技术。

成熟度不能仅根据 GitHub Stars、性能排行榜或最近发布日期判断，还应综合评估：

- API 稳定性与兼容性。
- 维护活跃程度。
- 缺陷修复和安全响应。
- 文档质量。
- 社区及实际应用生态。
- 长期维护成本。

存在重大未验证风险的技术，应明确准入条件，不能以社区规模代替专项验证。

### 适度轻量

在满足功能、性能、可靠性和安全要求的前提下，控制：

- 直接及传递依赖数量。
- CPU 和内存开销。
- 可执行文件及发布包体积。
- 构建、安装和部署复杂度。
- 项目自身需要维护的代码规模。

轻量不等于零依赖。

成熟库能够可靠解决的通用问题，应优先复用，而不是为了减少第三方依赖而增加大量自研代码。

### 职责匹配

每项技术应解决明确的系统问题。

不要求单个框架提供全部功能，也不应为了使用某个框架而改变系统分层。

### 开放标准

优先采用标准化通信协议、数据格式和语言接口。

避免不必要的专有协议、特殊运行环境和框架绑定。

### 可维护与可替换

第三方组件应具有明确的使用边界。

项目公共接口应尽可能使用标准语言类型或项目自身的数据类型，避免无必要地暴露第三方库内部类型。

关键技术的升级和替换应具有可管理的工程成本。

### Primitives, not Features

优先组合成熟、精简的基础组件，不因未来可能出现的需求而提前引入复杂基础设施。

不为简单控制与状态通信引入完整的分布式消息系统，不为局部功能引入通用 Actor Runtime、依赖注入容器或独立服务平台。

## 1.3 技术选型评价方法

| 评价维度 | 主要内容 |
|---|---|
| 功能适配 | 是否满足必要能力和使用场景 |
| 成熟度 | API、维护、文档及应用生态 |
| 性能 | 吞吐、延迟、CPU、内存及并发能力 |
| 依赖成本 | 直接依赖、传递依赖和运行资源 |
| 兼容性 | 操作系统、CPU 架构、语言版本及 ABI |
| 构建部署 | 静态链接、安装复杂度及发布方式 |
| 安全性 | 安全默认值、漏洞维护及资源保护 |
| 长期维护 | 升级、调试、替换及技术债务 |

评价优先级为：

**正确性与安全性 → 功能及平台兼容性 → 长期维护成本 → 性能与资源效率 → 依赖和体积优化。**

不依据单一指标确定技术选型。

对于承担关键能力但存在未验证风险的技术，可以作出有条件的选型决定，但不能直接认定其已经满足生产运行要求。

## 1.4 文档边界

本规范只记录相对稳定的技术决策。

| 本规范包含 | 本规范不包含 |
|---|---|
| 技术分层及语言选择 | 内部类、函数及详细模块设计 |
| 框架、库和协议选型 | 具体业务逻辑和业务状态机 |
| 候选技术比较 | HTTP 路由及 JSON Schema |
| 选择依据及技术限制 | 线程数量、锁顺序和具体算法 |
| 第三方依赖准入原则 | 项目缺陷修复和开发进度 |
| 构建、链接及发布技术策略 | 构建脚本及详细实施步骤 |
| 技术风险和强制准入条件 | 具体测试流程、数值阈值及实测结果 |

技术风险可以写入本规范。

具体测试负载、量化验收阈值和验证结果由专项验证规范及测试报告管理。

---

# 2. 系统技术架构

## 2.1 逻辑分层

`ros2_bag` 采用四层逻辑架构：

**Recorder – Server – Client – Application**

| 层次 | 开发语言 | 主要职责 | 主要技术 |
|---|---|---|---|
| Recorder | C++17 | ROS 2 数据接入、录制与存储 | rclcpp、Fast DDS、rosbag2、MCAP |
| Server | C++17 | 网络通信、请求处理和事件服务 | Drogon、HTTP、JSON、SSE |
| Client | Python | 统一的 Recorder 访问 SDK | HTTPX、httpx-sse |
| Application | Python | 用户交互与展示 | argparse、Textual、Starlette/Uvicorn |

四层表示逻辑职责，不要求每层对应独立进程。

## 2.2 开发语言

### Recorder / Server：C++17

Recorder 是性能敏感组件，需要直接使用 ROS 2 C++ API、并发机制、内存管理和存储基础设施。

Server 与 Recorder 使用相同语言，可以通过原生 C++ 接口在同一进程内集成，不需要额外的进程间通信。

采用 C++17 的主要原因：

- 与 ROS 2 Humble/Jazzy C++ 生态兼容。
- 适配现有 GCC 工具链。
- 标准库提供线程、原子、RAII 和智能指针等必要能力。
- 不要求 C++20 Coroutine 或独立异步运行时。
- 有利于统一编译标准和资源生命周期管理。

### Client / Application：Python 3.10+

Client 和 Application 主要承担低频控制、状态消费及用户交互，不处于 ROS 2 高频录制数据路径。

采用 Python 的主要原因：

- 同步/异步 HTTP 生态成熟。
- 终端 UI 和 Web 框架选择丰富。
- CLI、TUI、Web 能够共享统一 Client SDK。
- 用户交互代码的开发和维护效率较高。
- 可以独立于 Recorder 的 C++ 构建环境部署。

### 不引入 Python/C++ Binding

Client 与 Recorder Runtime 通过标准 HTTP/SSE 协议通信，不使用 pybind11、Cython 或其他进程内 Python/C++ 绑定作为基础方案。

原因包括：

1. 保持 Python Application 与 C++ Recorder 的进程及故障隔离。
2. 避免维护依赖 Python ABI 的 C++ 扩展模块。
3. 不需要针对不同 Python 版本分别构建绑定模块。
4. 允许 Client/Application 与 Recorder 独立部署及升级。
5. 降低跨语言对象生命周期和异常传播的复杂度。

HTTP/SSE 已能满足当前跨语言访问需求。

## 2.3 运行与部署形态

Recorder 和 Server 组合为 **Recorder Runtime**。

Recorder Runtime 采用单个 C++ 可执行程序运行。

Client 是 Python SDK，由 CLI、TUI 和 Web 等 Application 使用。

Application 可以独立启动和退出，不要求与 Recorder Runtime 处于同一进程。

这一形态的主要优势：

- Recorder 数据面保持原生 C++。
- Recorder 与 Server 不需要额外 IPC。
- Python Client 不依赖 `rclpy`。
- 用户界面与录制器进程解耦。
- Recorder 可以独立于 Application 运行。
- Python 依赖变化不要求重新编译 Recorder。

## 2.4 目标运行平台

| 操作系统 | ROS 2 | 系统 Python | CPU 架构 |
|---|---|---|---|
| Ubuntu 22.04 LTS | Humble | Python 3.10 | AMD64 / ARM64 |
| Ubuntu 24.04 LTS | Jazzy | Python 3.12 | AMD64 / ARM64 |

C++ Recorder 按目标 ROS 2 发行版和 CPU 架构分别构建。

Python Client/Application 以 Python 3.10 作为最低兼容基线，并验证 Python 3.12。

优先采用纯 Python Wheel，减少原生扩展和 Python ABI 维护成本。

---

# 3. Recorder 技术选型

Recorder 采用 ROS 2 原生 C++ 技术栈，优先复用 ROS 2 官方 API、成熟并发组件和标准存储格式。

## 3.1 ROS 2 客户端库

**选型：rclcpp**

`rclcpp` 是 ROS 2 官方 C++ 客户端库。

选择依据：

- 原生支持 ROS 2 Topic、QoS、Executor 和 Graph。
- 提供 GenericSubscription 等动态消息接入能力。
- 可以复用 ROS 2 类型支持和序列化机制。
- 与 ROS 2 官方工具及运行环境兼容。
- 不需要 Python 参与高频消息处理。

不通过 `rclpy` 或独立 DDS 客户端库替代 Recorder 的 ROS 2 数据接入。

## 3.2 DDS 中间件

**选型：Fast DDS / rmw_fastrtps_cpp**

选择依据：

- ROS 2 主流 DDS 实现之一。
- 支持可靠性、历史深度等标准 DDS QoS 能力。
- 支持 UDP 和共享内存等传输机制。
- 具备 Linux AMD64/ARM64 适配基础。
- 可通过 ROS 2 标准 RMW 接口使用。

Fast DDS 的共享内存能力不代表 Recorder 自动实现端到端零拷贝。

具体传输策略、QoS 和性能优化属于 Recorder 架构及运行配置范畴。

## 3.3 并发队列

**选型：moodycamel concurrentqueue**

选择依据：

- C++ 原生实现。
- 适合高并发消息传递。
- 支持 move-only 类型。
- 集成方式轻量。
- 不需要额外任务框架。

`concurrentqueue` 提供并发队列基础能力，不自动解决系统级容量预算和资源保护。

相关资源治理机制由 Recorder 架构设计确定。

## 3.4 Bag 存储接口

**选型：rosbag2_cpp / rosbag2_storage**

选择依据：

- ROS 2 官方 Bag 存储生态。
- 支持 ROS 2 标准消息格式。
- 提供统一存储插件接口。
- 可以利用现有消息类型及 Schema 能力。
- 降低自行维护 Bag Writer 的成本。

不自行开发通用 MCAP Writer，也不通过 Python 对录制消息进行二次写入。

**Humble/Jazzy Schema 适配是选型的一部分，而不是实现时再决定：** Jazzy 使用 rosbag2_cpp 的 MessageDefinition 能力；Humble 0.15.x 的 Writer API没有显式 MessageDefinition 参数，因此 Recorder 的 Humble Adapter 在 Topic/Segment 注册前调用**同版本 `rosbag2_storage_mcap` 导出的 MessageDefinitionCache**做完整定义 preflight/hash 校验，再使用标准 Writer API，让 MCAP Plugin 写 Schema。定义缺失时插件自身会容忍并产生空/不可用 Schema，因此 Recorder 的 preflight 是满足“自包含自定义消息”需求的强制安全门。项目不复制插件的定义解析算法，也不直接维护通用 MCAP 写入栈。由于 Humble Resolver 属于插件内部 namespace，rosbag2_storage_mcap 必须作为编译和运行时共同锁定的目标平台依赖；接口漂移通过编译/平台验证暴露。

## 3.5 存储文件格式

**选型：MCAP**

MCAP 是面向时间序列消息的开放容器格式，适合机器人及传感器数据存储。

主要优势：

- 支持多 Channel、多 Schema。
- 支持消息索引与 Chunk。
- 支持压缩。
- 支持自包含元数据。
- 具有跨语言工具生态。
- 适合离线分析和数据交换。

与 SQLite3 Bag Storage 相比，MCAP 更符合以消息流、Schema 和分片文件为核心的录制需求。

采用 ROS 2 的 MCAP Storage Plugin。

## 3.6 压缩算法

**选型：LZ4**

选择依据：

- 压缩和解压速度较高。
- 适合持续流式写入。
- 压缩 CPU 成本通常较低。
- 可以在存储空间与处理性能之间取得合理平衡。

与 Zstandard 等高压缩比算法相比，LZ4 通常以较低压缩率换取较高处理速度。

具体压缩级别和存储参数由性能评估及配置规范确定。

## 3.7 Recorder 选型汇总

| 领域 | 技术选型 | 主要原因 |
|---|---|---|
| 开发语言 | C++17 | ROS 2 兼容、性能及原生集成 |
| ROS 2 API | rclcpp | 官方生态与类型支持 |
| DDS | Fast DDS | 标准 RMW 与传输能力 |
| 并发队列 | concurrentqueue | 轻量、高并发 |
| Bag API | rosbag2_cpp | 官方存储生态 |
| 存储格式 | MCAP | 开放、自包含、可索引 |
| 压缩 | LZ4 | 高速压缩、较低 CPU 成本 |

---

# 4. Server 与通信协议技术选型

## 4.1 Server 技术要求

Server 需要具备：

- C++17 兼容性。
- HTTP/1.1 Server。
- JSON 请求和响应。
- 异步网络 I/O。
- 长时间保持的异步 HTTP 流式响应。
- 多客户端并发能力。
- 可控的资源占用。
- 静态链接支持。
- Linux AMD64/ARM64 兼容性。

## 4.2 C++ HTTP 框架比较

| 框架 | 主要优势 | 主要限制 | 选型结论 |
|---|---|---|---|
| **Drogon** | 成熟异步 HTTP 框架、完整 Web 基础能力 | 依赖和体积较大；无完整 SSE 专用框架 | **采用** |
| Oat++ | 相对轻量、支持异步 HTTP | 目标流式事件场景需要进一步适配 | 备选 |
| Crow | API 简洁、易于集成 | 长期异步流行为需按版本验证 | 不采用 |
| cpp-httplib | Header-only、依赖少 | 常规 Streaming 方式偏同步，长期连接并发需要谨慎评估 | 不采用 |
| Boost.Beast | 底层协议能力完整 | 需要自行建设更多 HTTP Server 基础设施 | 不采用 |

框架选型不仅考虑普通 HTTP 请求吞吐量，还需要考虑长期异步连接、连接生命周期、资源控制和维护成本。

## 4.3 HTTP 框架：Drogon

**正式选型：Drogon v1.9.14**

Drogon 是基于 C++ 的异步 HTTP 应用框架，提供路由、过滤器、请求/响应、JSON 及网络连接管理等能力。

### 选择依据

**成熟的异步网络基础设施**

Drogon 基于 Trantor 提供非阻塞、事件驱动网络 I/O。

适合统一处理短生命周期 HTTP 请求和长期异步流式响应。

**完整的 HTTP 框架能力**

Drogon 提供成熟的 HTTP Server 基础设施，避免项目自行维护完整 HTTP 协议处理和网络服务。

**C++17 兼容**

能够与 Recorder 使用统一的语言和编译标准。

**社区及维护生态**

Drogon 具备较成熟的文档、开源社区及实际应用基础。

**静态集成能力**

Drogon、Trantor 及必要依赖可以通过受控构建方式静态集成。

实际静态依赖闭包仍须在目标环境验证。

### 主要代价

- 依赖闭包比极简 HTTP 库复杂。
- 框架能力超过 Server 最小需求。
- 静态集成需要额外构建配置。
- 二进制体积相对较大。
- SSE 专用能力需要额外适配。

选择 Drogon 是为了降低通用 HTTP Server 和异步网络基础设施的长期维护成本，而不是追求最小代码体积。

### 版本与构建来源

Drogon v1.9.14 作为当前技术选型的初始版本基线。

Drogon 及其 Trantor 依赖采用固定源码版本构建，不依赖 Ubuntu `apt` 软件仓库提供指定版本的 Drogon。

由此保持：

- Drogon 与 Trantor 的版本一致性。
- 静态链接配置可控。
- 可选功能裁剪可控。
- 构建环境可复现。

精确 Git Commit 由第三方依赖锁文件管理。

## 4.4 Drogon 的 SSE 能力边界

**Drogon 不提供完整的 SSE 专用框架。**

它提供实现 SSE 所需的通用异步 HTTP 流式响应能力。

Drogon v1.9.14 的相关 API 包括：

- `HttpResponse::newAsyncStreamResponse()`
- `ResponseStream::send()`
- `ResponseStream::close()`

这些接口为异步 Streaming 提供网络传输基础。

但 Drogon 不自动提供完整的：

- SSE 帧编码。
- SSE 订阅与广播。
- 连接断开后的业务资源清理。
- 慢客户端背压管理。
- 事件持久化及重放。

因此，其准确技术定位是：

**Drogon 提供异步 HTTP Streaming 基础设施，项目在此基础上集成 SSE 专用能力。**

不能将通用异步 Stream 等同于开箱即用的完整 SSE 框架。

### 关键技术限制

`ResponseStream::send()` 返回成功，不能作为客户端已经消费数据、底层网络缓冲已排空或发送已获得端到端确认的证明。

长期 SSE 连接必须关注应用缓冲、框架内部缓冲及 Socket 发送缓冲之间的资源关系。

以下官方 Issue 可作为技术风险参考。

**Issue #2192 — AsyncStream Memory Leak**

https://github.com/drogonframework/drogon/issues/2192

在 2026-10-25 的核验基线中：

- 状态：Open。
- 标签：`to be confirmed`。

该 Issue 报告慢速网络条件下持续发送流式数据可能导致内存增长。

其主要场景涉及连续 Streaming 数据，不是专门针对低频 SSE。

因此，它是需要验证的异步流缓冲风险证据，不能直接证明 Drogon v1.9.14 存在确定的 SSE 内存泄漏。

**Issue #2201 — Overhead of Creating New Threads in Streaming Response**

https://github.com/drogonframework/drogon/issues/2201

在 2026-10-25 的核验基线中状态为 Open。

该 Issue 关注官方 Streaming 示例中为连接创建独立线程的扩展性问题。

需要明确：

**每连接创建线程是示例代码的实现方式，并不是 Drogon AsyncStream 的强制技术要求。**

它说明官方示例不能直接作为生产级多客户端 SSE 实现，不代表 Drogon 网络模型本身必须为每个 SSE 连接分配一个线程。

Issue 的后续状态应以锁定版本和专项验证结果为准。

## 4.5 SSE 开源实现参考

优先参考已有开源实现，避免从头开发通用 Streaming 和标准 SSE 机制。

### Drogon 官方 async_stream 示例

https://github.com/drogonframework/drogon/tree/master/examples/async_stream

参考内容：

- 异步 Stream API 使用。
- 流式数据发送。
- Stream 生命周期。
- HTTP Streaming 基础能力。

官方示例属于 API 演示，不应直接视为生产级 SSE 服务。

尤其不应直接沿用为每个连接创建独立线程的演示调度方式。

### drogonR

项目：

https://github.com/Zabis13/drogonR

相关文档：

https://mirror.dogado.de/cran/web/packages/drogonR/vignettes/streaming.html

drogonR 是基于 Drogon 的 R 语言 HTTP Server 集成项目。

其 `dr_stream_sse()` 提供部分 SSE 专用封装，包括：

- `text/event-stream`。
- 多行 `data:` 编码。
- 常用 Streaming Header。
- 基本流关闭及取消通知。

需要注意：

- `dr_stream_sse()` 主要封装 SSE `data:` 内容。
- 不应将其视为完整支持 `event:`、`id:`、`retry:` 的通用 SSE SDK。
- 不应假定它已提供完整订阅管理和网络背压。
- drogonR 不是独立的通用 C++ SSE 库。

因此，drogonR 适合作为 SSE 编码和 Streaming 生命周期处理的参考，不作为新的运行时依赖。

### 开源参考策略

项目遵循以下原则：

1. 复用 Drogon 原生异步 HTTP Stream。
2. 参考官方示例理解 Stream API。
3. 参考 drogonR 的 SSE 编码及取消处理。
4. 不重复实现 HTTP Chunked Transfer-Encoding。
5. 不为 SSE 引入第二套 HTTP 框架。
6. 不引入未经充分验证的小众 SSE 运行库。
7. 必要的 SSE 专用适配保持轻量，限制在 Server 层。

该方案旨在减少通用基础设施的自研，而不是承诺完全不需要 SSE 适配代码。

## 4.6 通信协议

### HTTP/1.1

**选型：HTTP/1.1**

用于控制请求、状态查询及标准请求/响应。

选择依据：

- 标准化程度高。
- C++、Python 和浏览器支持完善。
- 调试及测试简单。
- 能与 SSE 使用统一通信技术栈。
- 满足低频控制和状态查询需求。

### 为什么不选择 HTTP/2？

HTTP/2 的主要优势包括多路复用、头部压缩及更高效的连接利用。

对于以低频控制、状态查询和单向 SSE 为主的通信场景，上述能力不是必要条件。

同时，Drogon v1.9.14 的服务端能力主要基于 HTTP/1.0 和 HTTP/1.1，不能将 HTTP/2 Server 视为该版本已具备的能力。

因此，采用 HTTP/1.1 能满足需求，并避免引入额外协议或服务端基础设施。

未来如出现明确的 HTTP/2 需求，可以独立评估。

### JSON

**选型：JSON**

用于低频控制、查询及结构化状态数据。

选择依据：

- 跨语言兼容。
- 可读性好。
- 解析库成熟。
- 适合 HTTP API。

JSON 不承担 Recorder 高频 ROS 2 原始消息数据传输。

### SSE

**选型：Server-Sent Events**

用于 Server 向 Client 单向发送状态和事件通知。

选择依据：

- 基于标准 HTTP。
- 适合长期单向事件流。
- 可以复用 HTTP Server 和 Client。
- 不需要额外引入双向长连接协议。

主要限制：

- SSE 本身不是可靠消息队列。
- 不自动提供应用级事件持久化。
- 不保证断线后完整重放。
- 不自动解决慢客户端背压。

具体事件语义及恢复要求由独立通信协议规范确定。

### 协议比较

| 协议 | 当前结论 | 主要原因 |
|---|---|---|
| HTTP/1.1 JSON | **采用** | 满足控制与查询 |
| SSE | **采用** | 满足单向事件通知 |
| HTTP/2 | 暂不采用 | 当前场景无必要的多路复用收益 |
| WebSocket | 暂不采用 | 无强制双向长连接需求 |
| gRPC | 暂不采用 | Protobuf 和运行时成本不必要 |
| JSON-RPC | 暂不采用 | 无需独立 RPC 协议层 |
| 自定义 TCP 协议 | 不采用 | 增加协议维护成本 |

## 4.7 Drogon SSE 技术准入

**Drogon 的 HTTP 框架选型已确定，但生产级 SSE 适用性必须通过专项验证。**

准入条件：

1. SSE 协议输出正确。
2. 长时间异步连接稳定。
3. 慢客户端条件下资源占用有界。
4. 连接断开后相关资源能够及时释放。
5. 多客户端之间不存在不可接受的阻塞影响。
6. 网络异常不会影响 Recorder 高频数据处理。
7. 关闭过程能够正确终止连接并释放资源。

### 强制要求

如果 Drogon 通用异步 Stream 无法在合理资源预算内满足长期 SSE 连接要求，并且无法通过局部、可维护的适配解决，则必须重新评估 Server 框架选型。

这是影响技术选型成立的强制准入条件，不是可忽略的性能优化项。

具体负载、内存增长斜率、连接回收时限和其他数值阈值，由专项性能与可靠性验证规范确定。

本规范不预设缺乏实测依据的数值。

---

# 5. Client 与 Application 技术选型

## 5.1 Client 技术要求

Client 应具备：

- Python 3.10+ 兼容性。
- 同步 HTTP。
- 异步 HTTP。
- JSON 请求与响应。
- HTTP Streaming。
- SSE 消费。
- 连接池及超时控制。
- Linux AMD64/ARM64 兼容性。
- 无需依赖 ROS 2 Python API。

Client 负责通信能力的复用，不承担 Recorder 数据录制工作。

## 5.2 HTTP Client：HTTPX

### 候选比较

| 库 | 同步 HTTP | 异步 HTTP | 主要特点 | 结论 |
|---|---|---|---|---|
| **HTTPX** | 支持 | 支持 | 统一 API、连接池、Streaming | **采用** |
| Requests | 支持 | 不原生支持 | 成熟简单 | 不采用 |
| aiohttp | 以异步为主 | 支持 | 完整异步 HTTP 能力 | 不采用 |
| urllib | 支持 | 无同等级现代异步接口 | 标准库 | 不采用 |

### 正式选型

**HTTPX**

初始版本基线：**0.28.1**

选择依据：

1. 同时支持同步和异步 API。
2. 提供成熟的 HTTP/1.1 客户端能力。
3. 支持连接池、超时、认证及流式响应。
4. 可以统一服务 CLI、TUI 和 Web。
5. 依赖规模合理。
6. 与 Python 3.10/3.12 兼容。

HTTPX 具有 httpcore、anyio、certifi、idna 等基础依赖，属于合理的 HTTP Client 基础设施成本。

首期不引入无明确需求的 HTTP/2 或其他 Optional Extras。

## 5.3 SSE Client：httpx-sse

### 正式技术选型

**选型：httpx-sse**

初始版本基线：**0.4.3**

与 HTTPX 配套使用：

| 用途 | Python 包 | 版本 |
|---|---|---|
| HTTP Client | `httpx` | 0.28.1 |
| SSE Parser | `httpx-sse` | 0.4.3 |

**技术决策状态：SELECTED**

**生产准入状态：PENDING**

这一状态表示：

- 已正式选择 `httpx-sse` 作为 V1.0 的 SSE Parser。
- 不再将多个 SSE 库的选型比较作为开发前置条件。
- 但必须通过规定的兼容性和稳定性验证。
- 如发生无法合理规避的阻塞性缺陷，允许重新评估 SSE Parser。

### 选择依据

httpx-sse 是专门面向 HTTPX 的 SSE 消费组件。

它提供：

- 同步 SSE 消费接口 `connect_sse()`。
- 异步 SSE 消费接口 `aconnect_sse()`。
- SSE 事件解析。
- 事件数据、事件类型、事件 ID 和重试间隔字段访问。
- 对 HTTPX Streaming 的集成支持。

主要优势：

1. 与 HTTPX 原生集成，不需要替换 HTTP 网络栈。
2. 同时支持同步和异步 SSE。
3. 纯 Python 实现。
4. 无新增第三方运行时依赖。
5. Wheel 体积较小。
6. 避免自行实现完整 SSE 协议解析。
7. 适合轻量控制和状态事件消费。

与引入另一套 SSE HTTP Client 相比，HTTPX + httpx-sse 的职责更清晰，依赖更集中。

### 版本与维护状态

截至 2026-10-25：

- 最新公开版本：**0.4.3**。
- 发布日期：2025-10-10。
- 官方 Development Status：**Beta**。
- Python 要求：≥3.9。
- 发布形式：纯 Python Wheel。
- 官方建议锁定依赖版本。

因此，0.4.3 不是已知落后的旧版本，而是当前可用的最新发布基线。

但需要明确：

**httpx-sse 的发布维护活跃度较低，长期上游支持存在不确定性。**

这一风险需要考虑，但不能仅凭长时间未发布新版本就断言项目已经停止维护。

### 历史缺陷与修复

httpx-sse 在 2025 年曾修复过以下问题：

| 版本 | 日期 | 主要修复 |
|---|---|---|
| 0.4.1 | 2025-06-24 | 修复异步 SSE 生成器关闭问题 |
| 0.4.2 | 2025-10-07 | 修复不符合 SSE 规范的换行解析 |
| 0.4.3 | 2025-10-10 | 修复 0.4.2 引入的解析性能回归 |

其中，历史换行问题涉及错误识别 Unicode `U+2028`、`U+2029` 为 SSE 行终止符。

SSE 规范允许的行终止符是 CRLF、LF 和 CR。

因此，SSE Parser 必须保留数据内容中的其他 Unicode 字符，不得错误拆分事件。

0.4.3 已包含上述修复，但仍需要通过协议兼容性验证。

### 已知未修复问题：Issue #35

参考：

https://github.com/florimondmanca/httpx-sse/issues/35

**Issue #35 — Incorrect comment parsing when using ids**

该问题在 0.4.3 中仍需要关注。

典型条件是：

- SSE 流已经接收过非空事件 ID。
- 服务端随后发送仅包含注释的心跳块。
- Parser 可能产生额外的空数据事件。

按照 SSE 规范，仅包含注释的心跳块不应产生业务事件。

这属于已知的 SSE 协议一致性问题。

需要特别区分：

**合法的空 `data:` 事件与仅包含注释的心跳块并不等价。**

SSE 标准允许数据为空字符串的合法事件。

因此，不应将“无条件丢弃所有空数据事件”作为通用 SSE Parser 的正确修复。

项目应通过协议定义与 Client 内部兼容性边界，确保异常解析结果不会错误传递为有效业务事件。

具体兼容措施和测试由 Client 设计及验证规范确定。

### 技术使用边界

httpx-sse 只承担 SSE 协议解析。

不将其视为完整的业务事件客户端，也不依赖它提供：

- 可靠事件持久化。
- 应用级事件重放。
- 业务状态一致性保证。
- 自动业务恢复。
- 通用事件消息队列。

SSE Parser 的第三方类型不应成为跨层公共协议。

Client 对 Application 提供的接口应与具体 SSE Parser 解耦。

### 准入要求

httpx-sse 0.4.3 必须满足：

1. 与 HTTPX 0.28.1 兼容。
2. 在 Python 3.10/3.12 上运行正确。
3. 标准 SSE 协议解析正确。
4. 正确处理 CR、LF、CRLF 及 Unicode 数据。
5. 正确区分注释心跳与业务事件。
6. 对 Issue #35 的影响具备明确、可验证的处理方案。
7. 与 Drogon SSE 输出正常互操作。
8. 异步取消、异常关闭和资源释放行为可控。
9. 不产生不可接受的内存增长或额外依赖。
10. 能在不改变 Application 公共接口的情况下替换。

这些条件属于正式生产准入要求。

### 替换与回退策略

如果 httpx-sse 出现无法合理规避的阻塞性缺陷，应按以下优先级处理：

1. 优先评估经过验证的上游修复版本。
2. 其次评估成熟、轻量的独立 SSE Parser。
3. 仅在第三方方案不能满足需求时，考虑自行维护最小 SSE 解析实现。

替换时应尽量保持：

- HTTPX 不变。
- Client 公共接口不变。
- Application 调用方式不变。
- Server SSE 协议不变。

不因为 httpx-sse 的局部缺陷而默认替换整个 HTTP Client 技术栈。

### 最终判断

**采用 HTTPX 0.28.1 + httpx-sse 0.4.3。**

接受 httpx-sse 处于 Beta、维护活跃度较低以及存在已知协议边界问题的事实。

通过限制依赖使用范围、保留可替换性和强制兼容性验证控制风险。

该选择不表示已知缺陷可以忽略，也不表示该组件已经通过生产准入。

## 5.4 Python 基础能力

优先使用标准库：

| 模块 | 用途 |
|---|---|
| asyncio | 异步 I/O 协作 |
| dataclasses | 轻量数据模型 |
| typing | 类型标注 |
| enum | 枚举 |
| json | JSON 编码与解析 |
| pathlib | 路径处理 |
| logging | 日志基础能力 |

不为简单协议数据模型强制引入 Pydantic。

## 5.5 CLI：argparse

**选型：Python argparse**

选择依据：

- 标准库，无额外依赖。
- 支持命令行参数和子命令。
- 便于 Shell 自动化。
- API 稳定。
- 维护成本低。

不将 Click 或 Typer 作为基础依赖。

## 5.6 TUI：Textual

**选型：Textual**

参考兼容版本：**8.2.8**

Textual 提供终端布局、组件、事件交互和异步应用基础能力。

选择依据：

- 具有成熟的 Python TUI 组件生态。
- 适合实时状态展示和交互。
- 与 Python 异步应用模式匹配。
- 避免自行实现复杂终端渲染。
- 适合组件化用户界面。

### 兼容性

Textual 8.2.8 支持 Python 3.9 及以上、低于 Python 4 的版本范围，覆盖目标 Python 3.10/3.12。

Textual 作为 Application 可选依赖，不进入基础 Client 的必需依赖。

不默认将 `textual-dev` 等开发工具打入正式发布包。

## 5.7 Web：Starlette + Uvicorn

### Starlette

**选型：Starlette**

Starlette 是基于 ASGI 的轻量 Python Web 框架。

选择依据：

- 采用开放 ASGI 标准。
- 适合异步 Web Application。
- 支持路由、静态文件及 Streaming Response。
- 不需要大型 Web 框架的全部功能。
- 可以复用 Python Client。

### Python 版本兼容性

Python 3.10 兼容性是 Starlette 选型的重要约束。

截至本规范编制日期：

| Starlette 版本 | Python 最低版本 |
|---|---|
| 1.7.0 | ≥3.10 |
| 1.8.0 | ≥3.11 |
| 1.9.0 | ≥3.11 |

因此，在 Web Application 必须同时支持 Python 3.10 和 Python 3.12 的前提下，**Starlette 1.7.0 是当前适用的版本基线**。

不能直接采用需要 Python 3.11 及以上的 Starlette 版本。

长期维护中必须关注 Python 3.10 兼容分支的安全更新及依赖支持情况。

如果上游安全维护要求与 Python 3.10 兼容性发生不可接受的冲突，应独立评估 Python 运行环境策略。

### Uvicorn

**选型：Uvicorn**

参考兼容版本：**0.54.0**

选择依据：

- 成熟的 Python ASGI Server。
- 与 Starlette 自然配合。
- 支持 asyncio。
- 运行及部署方式清晰。

Uvicorn 0.54.0 支持 Python 3.10 及以上。

首期优先使用基础 Uvicorn 安装，不默认引入 `uvicorn[standard]` 的额外原生扩展依赖。

### 为什么不采用 FastAPI？

FastAPI 更适合需要声明式请求模型、自动 API 文档和完整业务 API 开发体验的服务。

Recorder 的核心 HTTP 服务已由 C++ Drogon 提供。

Python Web 主要承担用户交互，因此 Starlette 更符合职责和依赖规模要求。

## 5.8 Web 前端

**选型：HTML + CSS + JavaScript**

选择依据：

- 浏览器原生支持。
- 无额外前端运行时依赖。
- 不需要强制引入 Node.js 构建链。
- 适合轻量 Web 控制及状态展示。
- 部署简单，维护成本低。

React、Vue 等 SPA 框架不作为 V1.0 强制依赖。

## 5.9 Python 选型汇总

| 技术领域 | 选型 | 初始版本或约束 |
|---|---|---|
| Python 语言 | Python 3.10+ | 必须验证 3.10/3.12 |
| HTTP Client | HTTPX | 0.28.1 |
| SSE Parser | httpx-sse | 0.4.3，已选定、准入待验证 |
| 异步基础 | asyncio | 标准库 |
| 数据模型 | dataclasses / typing / enum | 标准库 |
| CLI | argparse | 标准库 |
| TUI | Textual | 8.2.8 |
| Web Framework | Starlette | 1.7.0 |
| ASGI Server | Uvicorn | 0.54.0 |
| Web 前端 | HTML/CSS/JavaScript | 原生技术 |

以上版本是当前兼容性基线，不替代正式依赖锁文件。

---

# 6. 基础组件、构建与部署技术

## 6.1 配置与序列化

### YAML

**选型：YAML + yaml-cpp**

用于结构化配置。

选择依据：

- 可读性较好。
- 适合分层配置。
- 与 ROS 2 生态一致。
- C++ 解析库成熟。

### JSON

C++ 业务侧：

**nlohmann/json**

Drogon 内部：

**JsonCpp**

Python：

**标准库 json**

允许 nlohmann/json 与 Drogon 内部 JsonCpp 共存，但应避免无必要地向其他技术层扩散第三方 JSON 类型。

不引入第三套 C++ JSON 框架。

## 6.2 C++ 构建系统

**选型：CMake + ament_cmake**

选择依据：

- ROS 2 标准构建生态。
- C++17 兼容。
- 支持静态及动态库构建。
- 便于不同 ROS 2 发行版适配。
- 可以利用成熟 CMake 工具生态。

不引入 Bazel、Conan 或 vcpkg 作为强制构建基础。

## 6.3 C++ 第三方依赖管理

**选型：Git Submodule + 精确版本锁定**

选择依据：

- 源码来源清晰。
- 能固定 Git Commit。
- 适合私有依赖静态构建。
- 可追溯依赖变化。
- 有利于离线构建和版本复现。

关键 C++ 第三方依赖应记录：

- 项目名称。
- 源码仓库。
- 精确版本或提交。
- 许可证。
- 构建选项。
- 直接及传递依赖。

### Drogon 源码构建策略

Drogon 和 Trantor 使用固定源码版本构建，不依赖 Ubuntu 发行版软件仓库提供特定版本。

不将 `apt install libdrogon-dev` 作为正式构建方式。

此策略能够控制：

- Drogon 与 Trantor 版本一致性。
- 静态链接配置。
- 可选功能裁剪。
- 构建环境复现。

精确 Git Commit 由独立依赖锁文件记录。

## 6.4 Python 包管理

**选型：pyproject.toml + 可复现依赖锁定**

按功能区分基础依赖和可选依赖。

| 安装范围 | 主要依赖 |
|---|---|
| Client | HTTPX、httpx-sse |
| CLI | Client、argparse |
| TUI | Client、Textual |
| Web | Client、Starlette、Uvicorn |

要求：

1. 支持 Python 3.10/3.12。
2. 完整锁定直接和传递依赖。
3. 不自动引入无关 Optional Extras。
4. 优先选择纯 Python Wheel。
5. 避免依赖目标设备临时联网安装。
6. 不污染 ROS 2 的系统 Python 环境。

### 依赖锁定工具

可以采用 uv、pip-tools 或其他能够生成可复现依赖集合的成熟工具。

具体工具由构建规范选择，本规范不强制指定。

所选工具应支持：

- 目标 Python 版本约束。
- 传递依赖锁定。
- 安装制品可追溯。
- 可重复构建。
- 必要的 Hash 或完整性校验。

## 6.5 Python 依赖体积预算

Python 依赖以约 **20 MiB 的压缩发布体积**作为初始工程预算。

预算覆盖 Client、CLI、TUI 和 Web 所需的正式第三方 Python 运行时依赖。

### 统计口径

主预算：

**所有正式第三方 Python 运行时依赖去重后，对最终发布归档增加的压缩体积。**

不包括：

- C++ Recorder ELF。
- ROS 2 系统运行库。
- Ubuntu 系统 Python 解释器。
- 开发和测试工具。
- 不参与正式发布的 Optional Extras。

辅助统计：

- 去重后的 Wheel 文件总大小。
- Python 依赖实际安装大小。
- 最终归档中 Python 依赖的压缩增量。

上述口径不能混用。

### 传递依赖

需要纳入统计的依赖包括但不限于：

- httpcore。
- anyio。
- h11。
- idna。
- certifi。
- click。
- rich。
- pygments。
- markdown-it-py。
- mdurl。
- Textual 的其他必要传递依赖。

最终以锁定后的实际依赖图为准。

### 预算管理

逐包版本、Wheel 大小、安装大小及压缩体积由独立的 Dependency Size Report 管理。

不将动态变化的逐包体积数据写入技术选型正文。

20 MiB 属于初始工程预算，不是未经验证即可宣称满足的指标。

超预算时，应分析原因、评估优化空间并记录接受依据。

## 6.6 CI/CD 与构建环境

**选型：GitHub Actions + Docker**

GitHub Actions 用于自动化构建、测试及制品管理。

Docker 用于隔离和固定目标 ROS 2 构建环境。

选择依据：

- 与 GitHub 自然集成。
- 适合组织多平台构建。
- 可以固定构建环境和依赖。
- 降低开发机环境差异。
- 有利于版本及制品追溯。

Docker 是构建与验证技术，不要求最终用户必须在容器中运行 Recorder。

## 6.7 C++ 链接与依赖合规

### 链接策略

**Recorder 与 Server 静态集成，ROS 2 和 Ubuntu 系统运行库动态链接。**

| 依赖类别 | 目标链接策略 |
|---|---|
| Recorder C++ 实现 | 静态集成 |
| Server C++ 实现 | 静态集成 |
| Drogon | 静态 |
| Trantor | 静态 |
| JsonCpp | 优先静态 |
| libuuid / zlib | 根据实际依赖闭包优先静态 |
| yaml-cpp | 静态 |
| concurrentqueue | 编译期集成 |
| nlohmann/json | 编译期集成 |
| ROS 2 / RMW / rosbag2 | 动态、按目标发行版锁定 |
| MCAP Storage Plugin | 目标 ROS 2 环境提供并锁定；Humble Adapter 还在编译期使用同版本 Schema Resolver 接口 |
| glibc / libstdc++ | 系统动态库 |

该策略不要求完全静态链接 Linux 可执行文件。

### 选择依据

静态集成私有 C++ 依赖能够：

- 减少随程序交付的私有 `.so`。
- 降低动态库版本冲突。
- 简化安装与部署。
- 提高发布制品可重复性。

主要代价：

- 静态构建配置更复杂。
- ELF 体积可能增加。
- 第三方库安全更新需要重新构建。
- 必须审查静态链接及再分发义务。

静态链接是一项部署便利性选择，不代表其在所有条件下都具有最高性能或最小体积。

### 许可证基线

| 组件 | 许可证 |
|---|---|
| Drogon | MIT |
| Trantor | BSD-3-Clause |
| JsonCpp | MIT / Public Domain（按适用条款） |
| 其他依赖 | 按锁定源码逐项确认 |

正式发布应以实际锁定版本的许可证文本为准。

MIT、BSD-3-Clause 等宽松许可证通常不会仅因静态链接而要求提供可重新链接的目标文件，但必须满足版权声明、许可证文本保留等义务。

如果完整依赖闭包中存在 LGPL 或其他附加许可要求，应根据具体条款单独审查。

### 强制合规要求

**所有直接及传递依赖必须在正式发布前完成许可证和再分发义务审查。**

包括：

1. 识别实际使用的第三方组件。
2. 确认精确版本及许可证。
3. 检查静态链接和再分发条件。
4. 保留必要的版权和许可证文本。
5. 识别许可证冲突。
6. 形成可追溯的依赖许可证清单。

不因主框架采用宽松许可证，就默认全部传递依赖具有相同许可条件。

## 6.8 发布归档

### 默认格式：tar.xz

选择依据：

- Ubuntu 环境支持成熟。
- 压缩率较高。
- 适合多平台预编译制品分发。
- 无需专用安装程序。

需要注意：

Ubuntu 22.04/24.04 可以通过标准系统软件包处理 XZ，但不能假定所有精简 Ubuntu 镜像均预装 `xz-utils`。

发布说明必须明确解压工具要求。

### 兼容备选：tar.gz

`tar.gz` 的解压工具兼容性通常更广，适合未预装 XZ 工具的极简环境或其他系统。

因此：

- `tar.xz` 为默认归档格式。
- `tar.gz` 为兼容性备选。
- 不强制同时发布两套格式。

如未来目标平台或部署环境变化，可以重新评估归档兼容性需求。

### C++ 制品

Recorder Runtime 每个目标平台交付一个原生 C++ 可执行文件：

`ros2_bag_recorder`

目标组合：

- Humble AMD64。
- Humble ARM64。
- Jazzy AMD64。
- Jazzy ARM64。

Python Client 和 Application 尽量作为平台公共组件交付，避免重复打包相同的纯 Python 代码。

---

# 7. 技术准入与风险控制

本章只定义与技术选型有关的必要准入条件，不记录具体源码缺陷修复、测试实现或项目阶段状态。

## 7.1 Drogon SSE

**风险级别：高。**

Drogon 的通用异步 HTTP Stream 不自动提供完整的 SSE 订阅管理与背压控制。

必须验证：

| 验证领域 | 强制要求 |
|---|---|
| 协议兼容 | 正确输出标准 SSE |
| 长连接稳定性 | 能稳定维持长期异步连接 |
| 内存有界性 | 慢客户端不得造成持续无界增长 |
| 网络缓冲 | 框架内部缓冲不得失控 |
| 连接回收 | 断线后及时释放相关资源 |
| 并发隔离 | 慢客户端不应阻塞正常客户端 |
| 数据面隔离 | 网络问题不得影响 Recorder 高频数据处理 |
| 关闭行为 | 能受控结束流并回收资源 |

### 准入判定

Drogon SSE 必须通过专项兼容性、资源及生命周期验证，才能确认其生产适用性。

测试应覆盖持续事件发送、多客户端并发、慢速客户端、异常断开、长期运行和受控关闭。

不能仅以正常连接测试或普通 HTTP Benchmark 作为正式准入依据。

### 退出条件

**如果 Drogon 的异步 Stream 无法在合理资源预算内提供稳定的长期 SSE 服务，并且无法通过局部、可维护的适配解决，则必须重新评估 Server 框架选型。**

该要求是强制技术准入条件。

具体验收阈值由专项验证规范确定。

## 7.2 httpx-sse

**风险级别：中。**

**技术决策：SELECTED**

**生产准入：PENDING**

httpx-sse 0.4.3 已被正式选择为 Python Client 的 SSE Parser。

但其生产适用性仍存在以下需要验证的风险。

### 维护风险

- 官方仍标记为 Beta。
- 发布及代码维护活跃度偏低。
- 上游未来修复和升级响应存在不确定性。

低维护活跃度不等于已经确认项目停止维护，但应纳入长期依赖风险管理。

### 协议正确性风险

历史修复涉及：

- 异步生成器关闭。
- 不符合规范的 Unicode 换行解析。
- 解析性能回归。

此外，Issue #35 反映了事件 ID 与注释心跳组合情况下的解析问题。

这些风险与 SSE Parser 的核心职责直接相关。

### 准入要求

正式生产使用前必须验证：

1. HTTPX 0.28.1 与 httpx-sse 0.4.3 兼容。
2. Python 3.10/3.12 正常运行。
3. 标准 SSE 解析正确。
4. 注释心跳不得错误地表现为有效业务事件。
5. 合法空数据事件的语义不得被无条件破坏。
6. 特殊 Unicode 字符处理符合规范。
7. 异步流取消、异常结束和资源回收可靠。
8. 与 Drogon 的实际 SSE 输出互操作。
9. 不存在不可接受的资源增长。
10. SSE Parser 可以在不修改 Application 接口的情况下替换。

### 风险控制

原则上将 SSE Parser 限制在 Client 内部，不向 Application 暴露第三方事件类型。

已知解析问题应通过明确的协议约束和可验证的兼容机制处理。

不应让 Application 为第三方 Parser 的缺陷长期增加独立兼容补丁。

### 替换条件

如果出现无法合理规避的阻塞性缺陷，则重新评估 SSE Parser。

优先级为：

1. 上游修复版本。
2. 成熟、轻量的替代 Parser。
3. 自行维护最小解析实现。

替换 SSE Parser 不应导致 HTTPX、Server 协议或 Application 公共接口同时变更。

## 7.3 Python 版本兼容性

**风险级别：中。**

主要风险来自上游框架提高最低 Python 版本要求。

当前最重要的约束：

- Starlette 1.7.0 支持 Python 3.10。
- Starlette 1.8.0 起要求 Python 3.11 及以上。

因此，Python 依赖不能仅采用“始终使用最新版”的策略。

依赖锁定必须明确目标 Python 版本，并检查全部直接及传递依赖的 `Requires-Python` 约束。

如果关键框架的安全维护与 Python 3.10 兼容性发生不可接受的冲突，应重新评估应用运行环境策略。

## 7.4 依赖体积

**风险级别：低至中。**

Python 依赖采用约 20 MiB 的压缩发布体积预算。

主要要求：

- 完整统计传递依赖。
- 区分 Wheel、安装及归档体积。
- 避免重复打包。
- 不默认包含开发和测试工具。
- 不安装无需求的原生扩展。
- 明确超预算时的审查原则。

逐包体积及实测数据不属于技术选型正文。

## 7.5 静态链接与许可证

**风险级别：中。**

主要关注：

- 私有 C++ 依赖静态链接可行性。
- 直接及传递动态依赖闭包。
- 第三方许可证及再分发义务。
- 构建可复现性。
- 目标 Ubuntu/ROS 2 环境兼容性。

静态链接策略只有在依赖闭包和许可证审查通过后，才能视为满足交付要求。

## 7.6 平台兼容性

必须覆盖：

- Ubuntu 22.04 / 24.04。
- ROS 2 Humble / Jazzy。
- AMD64 / ARM64。
- C++17。
- Python 3.10 / 3.12。
- ROS 2 动态插件机制。
- Python 直接及传递依赖。

官方声明支持某个平台，并不意味着完整技术组合自动通过验证。

## 7.7 安全要求

HTTP 控制服务默认采用受限本机访问方式。

涉及远程访问时，必须采用可信的身份认证、授权及 TLS 保护机制。

Web Application 应具有适当的会话和 CSRF 防护。

Starlette 提供 Web 基础设施，但不会自动为所有修改状态的接口完成 CSRF 防护。

安全功能优先复用成熟框架和标准机制，不引入无必要的通用安全平台。

## 7.8 技术决策与生产准入

技术选择和生产适用性是两个不同维度。

**技术选择**用于确定使用哪种框架、协议和第三方库。

**生产准入**通过必要验证，确认所选技术满足实际运行条件。

| 信息 | 管理位置 |
|---|---|
| 技术选择和主要风险 | 本规范 |
| 强制技术准入条件 | 本规范 |
| 具体测试场景 | 专项验证规范 |
| 量化验收阈值 | 性能与可靠性验证规范 |
| 实际测试数据 | 验证报告 |
| 依赖体积 | Dependency Size Report |
| 许可证及 SBOM | 第三方合规清单 |
| 精确依赖版本 | 依赖锁文件 |
| 项目阶段及 Gate | 实施与验收计划 |

技术选型规范不直接绑定某个开发阶段或 Gate 编号。

即使实施计划调整，技术本身的准入条件仍然有效。

---

# 8. 最终技术选型与维护规则

## 8.1 技术选型总表

| 技术领域 | 最终选型 | 决策状态 |
|---|---|---|
| 系统分层 | Recorder–Server–Client–Application | SELECTED |
| Recorder 语言 | C++17 | SELECTED |
| Server 语言 | C++17 | SELECTED |
| Client 语言 | Python 3.10+ | SELECTED |
| Application 语言 | Python 3.10+ | SELECTED |
| ROS 2 | Humble / Jazzy | SELECTED |
| ROS 2 C++ API | rclcpp | SELECTED |
| DDS | Fast DDS | SELECTED |
| 并发队列 | concurrentqueue | SELECTED |
| Bag 存储接口 | rosbag2_cpp | SELECTED |
| Bag 格式 | MCAP | SELECTED |
| 压缩 | LZ4 | SELECTED |
| C++ HTTP 框架 | Drogon v1.9.14 | SELECTED / 准入待验证 |
| HTTP 协议 | HTTP/1.1 | SELECTED |
| 控制数据格式 | JSON | SELECTED |
| 服务端事件 | SSE | SELECTED |
| SSE Server 基础 | Drogon Async Stream | SELECTED / 准入待验证 |
| SSE 开源参考 | Drogon 官方示例、drogonR | REFERENCE |
| Python HTTP | HTTPX 0.28.1 | SELECTED |
| Python SSE | httpx-sse 0.4.3 | **SELECTED / 准入待验证** |
| Python 异步基础 | asyncio | SELECTED |
| CLI | argparse | SELECTED |
| TUI | Textual 8.2.8 | SELECTED |
| Web Framework | Starlette 1.7.0 | SELECTED |
| ASGI Server | Uvicorn 0.54.0 | SELECTED |
| Web 前端 | HTML/CSS/JavaScript | SELECTED |
| C++ YAML | yaml-cpp | SELECTED |
| C++ JSON | nlohmann/json、JsonCpp | SELECTED |
| Python JSON | 标准库 json | SELECTED |
| C++ 构建 | CMake + ament_cmake | SELECTED |
| C++ 依赖管理 | Git Submodule + 精确版本锁定 | SELECTED |
| Python 包管理 | pyproject.toml + 可复现锁定 | SELECTED |
| CI/CD | GitHub Actions + Docker | SELECTED |
| C++ 发布 | 每平台单 Recorder ELF | SELECTED |
| 默认归档 | tar.xz | SELECTED |
| 兼容归档 | tar.gz | OPTIONAL |

表中版本为本规范编制日期的技术基线。

实际构建使用的精确版本和提交以独立锁文件为准。

## 8.2 不作为 V1.0 基础依赖的技术

| 技术 | 结论与原因 |
|---|---|
| HTTP/2 | 当前通信场景无必要的多路复用收益 |
| gRPC | HTTP JSON/SSE 已满足需求，不引入额外 RPC 运行时 |
| WebSocket | 当前没有强制双向长连接需求 |
| Redis / 消息代理 | 不需要独立分布式消息基础设施 |
| 数据库 ORM | 与录制系统基础能力不匹配 |
| Actor Runtime | 增加不必要的执行模型复杂度 |
| Python/C++ Binding | HTTP/SSE 满足跨语言通信，并保持进程和 ABI 隔离 |
| C++ Client SDK | 当前没有明确的独立 C++ 客户端需求 |
| Requests / aiohttp | HTTPX 已覆盖所需 HTTP 能力 |
| 其他 SSE HTTP Client | httpx-sse 已满足当前技术组合要求，无需重复 HTTP 栈 |
| Click / Typer | argparse 满足当前 CLI 需求 |
| FastAPI / Django | Starlette 更符合轻量 Web Application 需求 |
| React / Vue | 首期没有引入 SPA 框架的必要 |
| 自研 DDS | ROS 2/Fast DDS 提供相关基础能力 |
| 自研 MCAP Writer | rosbag2/MCAP 提供标准存储能力 |
| 独立 Drogon SSE 框架 | 已有参考项目尚不能证明适合引入新的成熟独立依赖 |

上述决定适用于当前产品范围，不表示这些技术在其他系统中不适用。

## 8.3 技术决策与生产准入状态

采用两套独立状态。

### 技术决策状态

| 状态 | 含义 |
|---|---|
| SELECTED | 已确定技术方案 |
| CONDITIONAL | 有条件选择，最终技术尚未批准 |
| DEFERRED | 当前不引入 |
| REFERENCE | 仅作为技术参考 |

### 生产准入状态

| 状态 | 含义 |
|---|---|
| PENDING | 尚未完成必要验证 |
| APPROVED | 通过必要验证和评审 |
| REJECTED | 未满足准入条件 |

技术决策与生产准入可以独立管理。

例如：

**Drogon v1.9.14**

- 技术决策：SELECTED。
- SSE 生产准入：PENDING。

**httpx-sse 0.4.3**

- 技术决策：SELECTED。
- SSE Parser 生产准入：PENDING。

这不代表技术选型尚未完成，而是明确表示已选定技术仍需满足实际运行要求。

**技术选型冻结不等于整个软件系统已通过生产验收。**

## 8.4 版本与依赖维护

本规范主要管理：

- 技术选择。
- 技术组合。
- 关键兼容性要求。
- 选择依据。
- 技术能力边界。
- 强制准入条件。

具体版本与构建信息由独立文件管理。

| 信息 | 管理位置 |
|---|---|
| 技术和协议选择 | 本规范 |
| C++ 精确 Git Commit | 第三方依赖锁文件 |
| Python 精确版本和依赖 | Python Lockfile |
| 构建环境 | 构建规范 |
| 运行配置 | 配置规范 |
| 通信 Schema | 协议规范 |
| 测试阈值与结果 | 验证规范和报告 |
| 许可证及 SBOM | 第三方合规清单 |

若某次兼容性升级不改变技术方向和关键约束，原则上不需要修订本规范。

## 8.5 文档修订条件

需要修订本规范的情况：

1. 改变四层架构或语言分工。
2. 更换 ROS 2、DDS、存储等核心技术。
3. 更换 Drogon、HTTPX、Textual、Starlette 等主要框架。
4. 更换正式采用的 SSE Parser 技术方案。
5. 改变 HTTP JSON/SSE 通信方案。
6. 引入新的关键基础设施。
7. 改变正式目标平台。
8. 改变主要构建和交付策略。
9. 发现影响选型成立条件的重大技术限制。

通常不需要修订本规范的情况：

- 内部类、函数和模块调整。
- 目录结构变化。
- 业务状态机变化。
- HTTP API 增删。
- SSE 事件字段调整。
- 线程数量及资源参数变化。
- 普通缺陷修复。
- 开发阶段调整。
- 测试或验收状态变化。
- 不改变技术决策的兼容性升级。

## 8.6 最终结论

`ros2_bag` V1.0 采用 **Recorder–Server–Client–Application** 四层逻辑架构。

**Recorder**

采用 C++17、rclcpp、Fast DDS、concurrentqueue、rosbag2_cpp 和 MCAP/LZ4，负责原生数据录制与存储。

**Server**

采用 C++17 与 Drogon v1.9.14，通过 HTTP/1.1 JSON 提供标准请求/响应，通过 Drogon 通用异步 HTTP Stream 实现 SSE。

SSE 专用能力优先参考官方示例和 drogonR，避免重复建设 HTTP 基础设施。

生产级 SSE 使用必须满足资源有界、连接清理和长期稳定性要求。

**Client**

采用 Python、HTTPX 0.28.1 与 httpx-sse 0.4.3，分别承担 HTTP 通信与 SSE 协议解析。

httpx-sse 已被正式选定，但仍须通过协议正确性、异常处理、生命周期和 Drogon 互操作性验证。

其已知解析缺陷和长期维护风险由明确的使用边界及替换机制控制。

**Application**

采用 Python，CLI 使用 argparse，TUI 使用 Textual，Web 使用 Starlette/Uvicorn，前端首期使用 HTML/CSS/JavaScript。

**构建与部署**

采用 CMake/ament_cmake、Git Submodule、Python 标准包管理、GitHub Actions 和 Docker。

Recorder 与 Server 静态集成为单个 C++ Runtime，ROS 2 与系统运行库保持动态链接。

**依赖治理**

以可追溯、可复现、可验证和许可证合规为必要条件。

Python 依赖采用约 20 MiB 压缩发布体积预算，默认归档为 tar.xz，tar.gz 保留为兼容选项。

本规范确立技术选择及适用约束，不规定具体业务实现。

**文档状态：REVIEW。**

完成正式技术评审后，可以冻结已确定的技术决策。

Drogon SSE 和 httpx-sse 的生产准入仍须通过相应专项验证，不因技术选型冻结而自动获得批准。

---

## 参考资料

### C++ Web 与 SSE

1. Drogon 官方仓库
   https://github.com/drogonframework/drogon

2. Drogon HttpResponse API
   https://github.com/drogonframework/drogon/blob/master/lib/inc/drogon/HttpResponse.h

3. Drogon async_stream 示例
   https://github.com/drogonframework/drogon/tree/master/examples/async_stream

4. Trantor
   https://github.com/an-tao/trantor

5. drogonR
   https://github.com/Zabis13/drogonR

6. drogonR Streaming / SSE
   https://mirror.dogado.de/cran/web/packages/drogonR/vignettes/streaming.html

7. Drogon Issue #2192 — AsyncStream Memory Leak
   https://github.com/drogonframework/drogon/issues/2192

8. Drogon Issue #2201 — Streaming Thread Overhead
   https://github.com/drogonframework/drogon/issues/2201

9. HTML Living Standard — Server-Sent Events
   https://html.spec.whatwg.org/multipage/server-sent-events.html

### Python

10. HTTPX
    https://www.python-httpx.org/

11. httpx-sse
    https://pypi.org/project/httpx-sse/

12. httpx-sse 官方仓库
    https://github.com/florimondmanca/httpx-sse

13. httpx-sse Issue #35 — Incorrect comment parsing when using ids
    https://github.com/florimondmanca/httpx-sse/issues/35

14. Textual
    https://textual.textualize.io/

15. Starlette
    https://www.starlette.io/

16. Uvicorn
    https://www.uvicorn.org/

17. Python Packaging User Guide
    https://packaging.python.org/

### ROS 2 与存储

18. ROS 2 rosbag2
    https://github.com/ros2/rosbag2

19. Fast DDS
    https://github.com/eProsima/Fast-DDS

20. MCAP
    https://mcap.dev/

21. moodycamel concurrentqueue
    https://github.com/cameron314/concurrentqueue

### 依赖与许可证

22. Drogon LICENSE
    https://github.com/drogonframework/drogon/blob/master/License

23. Trantor LICENSE
    https://github.com/an-tao/trantor/blob/master/License

24. JsonCpp
    https://github.com/open-source-parsers/jsoncpp

---

## 文档修订记录

| 版本 | 主要修订 |
|---|---|
| v4.0 | 统一四层技术架构，明确 Drogon SSE 能力边界 |
| v4.1 | 增加技术准入条件、Python 兼容版本、许可证审查和发布约束 |
| v4.2 | 核实依赖及 Issue 状态，补充 HTTP/1.1、源码构建及语言绑定排除依据 |
| **v4.2.1** | 正式确定 HTTPX 0.28.1 + httpx-sse 0.4.3；补充 Issue #35、维护风险、SSE Parser 的生产准入和替换策略，明确技术决策与生产验证独立管理 |
