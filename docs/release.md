# ros2_bag V1.0 构建、CI 与发布规范

| 属性 | 内容 |
|---|---|
| 版本 | v0.1.2 |
| 状态 | **DRAFT — 发布方案仍可调整，未产出 Release Artifact** |
| 需求 | requirements.md NFR-COMP-01～04 / AC-19～21，及 verification.md 的 V-* 测试 |
| 验证 | verification.md；任何历史测试结论都不代替当前代码的真实测试 |

# 1. Release 身份、依赖和构建输入

## 1.1 Source、Dependency Lock、Build Manifest

每个 Release Candidate 固定同一 Source Git SHA、所有 Submodule Commit SHA、ROS 2/RMW/FastDDS/rosbag2/MCAP/Compression/Drogon/Trantor/HTTPX/httpx-sse 依赖版本、对应构建镜像 digest 与系统 package lock。跨架构二进制哈希**无需相同**：同一 Source/Dependency Manifest SHA 对应四个平台各自的 artifact SHA、构建环境和测试证据。不得用开发机中偶然存在的动态库或 /home 用户源码树路径替代固定依赖。

| 输入 | 冻结内容 | 验证方式 |
|---|---|---|
| Source | Git SHA/tag、dirty=false、submodule URL/SHA | git rev-parse / status / submodule status |
| C++ | C++17、GCC/Clang、CMake/ament、选项/警告/Sanitizer | build.log / compile_commands.json |
| ROS/DDS | Ubuntu Image Digest、ROS/RMW/FastDDS/spdlog Build | apt/dpkg/包 manifest，跨节点实际发现与收发 |
| Storage | rosbag2、mcap plugin、LZ4、Schema/Writer 配置 | plugin load trace，Reader + 独立解码 |
| Server | Drogon v1.9.14、Trantor 及其动态依赖与许可证 | lock、动态链接闭包 |
| Python | Python 3.10/3.12、httpx 0.28.1、httpx-sse 0.4.3 | lockfile/hash、isolated wheel install |
| Security | Bearer Credential/Scope、TLS direct/trusted-proxy、证书/代理信任边界、Web Session/CSRF | Config 校验、Secret 泄漏检查与 AC-18 攻击用例 |
| Runtime Config | recorder.yaml、mcap.yaml、server.yaml 的实际内容与 SHA | 与 Direct C++ 构造默认值的单测 |

构建/打包的任何临时 patch 必须进入受控 Source Commit，不允许“开发机器上手工改过才成功”的无法再现制品。ROS 2/RMW 若为系统依赖，Manifest 应记录版本与先决条件而非私自将整个 ROS 安装目录复制进 tar。

## 1.2 四平台构建、运行与分析矩阵

| Target | Runtime 原生编译/运行 | Python SDK | Reader/Schema | 额外 |
|---|---|---|---|---|
| ubuntu22-humble-amd64 | MUST | 3.10 | MUST | Humble FastDDS TSan 风险 |
| ubuntu22-humble-arm64 | MUST | 3.10 | MUST | ARM64 30min 高负载 |
| ubuntu24-jazzy-amd64 | MUST | 3.12 | MUST | Jazzy Sanitizer |
| ubuntu24-jazzy-arm64 | MUST | 3.12 | MUST | ARM64 弱内存序 |

分离四平台原生验收与 MCAP 录制端×分析端 Humble/Jazzy 2×2。Docker L1（源码构建）/L2（真实 Runtime）/L3（故障与资源）/L4（打包重装）属于分层证据，不可把跨平台容器编译当成 ARM64 真实运行、FastDDS 真通信或 Schema 读回。四平台以同一 Release Source SHA 构建并留原始 Build/Run Log。

建议构建流水线：

```text
source SHA + dependency lock
 → lint / formatting / C++17 / Python 3.10/3.12
 → ROS Humble/Jazzy × AMD64/ARM64 native build
 → Recorder + Server + Client + Application integration
 → Pool/Queue sanitizer, RMW concurrency, MCAP independent reader
 → perf / slow SSE / SIGKILL recovery / 2×2 schema
 → G0/G1/G2/G3 reviews and closed OPEN gates
 → per-platform packaging
 → clean environment install
 → build-manifest + SHA256SUMS + evidence-index.json
 → G3 Release Decision=APPROVED
```

# 2. 制品组成和验签

## 2.1 目标发布树

```text
release/
├── manifest/
│   ├── build-manifest.json
│   ├── dependencies.lock.json
│   ├── evidence-index.json
│   ├── platform-matrix.md
│   └── licenses/
├── ubuntu22-humble-amd64/ros2_bag-runtime.tar.xz
├── ubuntu22-humble-arm64/ros2_bag-runtime.tar.xz
├── ubuntu24-jazzy-amd64/ros2_bag-runtime.tar.xz
├── ubuntu24-jazzy-arm64/ros2_bag-runtime.tar.xz
├── python/
│   ├── ros2_bag_client-<version>-py3-none-any.whl
│   └── ros2_bag_apps-<version>-*.whl
└── SHA256SUMS
```

这是新分层的**目标包结构**，不是当前已有文件。是否把 CLI/TUI/Web 拆成三个 Wheel 取决于未来正式 Application 设计；需求规定的每个 MUST 应可安装。`ros2_bag.tar.xz` 单一包名不是新分层的兼容 MUST；但分发制品完整性、安装说明、依赖/许可/配置都是 MUST。

Manifest 至少包含此结构（所有占位值必须被生成脚本替换）：

```json
{
  "release_version": "1.0.0-rc1",
  "source_sha": "REPLACE_SOURCE_SHA",
  "dependency_lock_sha256": "REPLACE_DEPENDENCY_SHA256",
  "build_utc": "REPLACE_RFC3339_UTC",
  "targets": [
    {
      "platform": "ubuntu22-humble-amd64",
      "ros_distro": "humble",
      "artifact": "ubuntu22-humble-amd64/ros2_bag-runtime.tar.xz",
      "artifact_sha256": "REPLACE_ARTIFACT_SHA256",
      "build_image_digest": "sha256:REPLACE_IMAGE_DIGEST",
      "dependency_report": "manifest/dependencies.lock.json",
      "evidence_report": "manifest/evidence-index.json",
      "admission": "BLOCKED"
    }
  ],
  "release_decision": "PENDING"
}
```

必须检查所有必须平台的 Manifest 条目、文件存在性、Source/Dependency SHA、artifact SHA 格式和实值、License/Evidence Index 引用、动态加载插件闭包；若任一结果 NOT_RUN/BLOCKED/FAIL，不能改写 G3 APPROVED。跨编译器/架构 Artifact SHA 不同是正常的，不能混同于同一源版本构建漂移。

## 2.2 SHA256SUMS 与来源保障

生成标准 sha256sum 清单，采用相对路径与包内容一一对应。使用者可以执行：

```bash
sha256sum -c SHA256SUMS
```

此命令证明文件与**持有清单**一致，但未经独立认证的 SHA256SUMS 并不提供来源真实性。V1.0 可不强制 GPG，但 Release Channel/Tag/Source Commit/Manifest SHA 的来源须受保护且可审计；新增签名能力需另行定义密钥管理，不能凭 HTTPS 下载就承诺端到端软件供应链签名。

# 3. 干净环境安装、安全及代码风格

## 3.1 无开发树依赖的重装测试

在没有源码 checkout、没有用户 HOME 下非受控库、没有本地 venv 的隔离环境安装每个平台的 Runtime + SDK/Apps。实测步骤：

1. 输出完整版本与第三方许可证，检查动态库 SONAME/RPATH/RUNPATH、ldd、ROS 插件 XML/MCAP 插件、Drogon/Trantor 链接和实际加载来源；
2. 检验 Config 文件可读、unknown key 或不安全端口配置被拒绝，Recorder output_root 归一化、权限、两进程 lock 互斥；
3. 用真实 ROS 2 Publisher 发有序消息、Start/Add/Stop，验证 MCAP CDR/Schema 和 Manifest/Event，一段可以独立读回；
4. 通过 Python wheel 发受保护 GET/控制 POST、消费 SSE、故障模拟 UNKNOWN/断线/重连，SDK 无 rclpy 强依赖；
5. CLI/TUI/Web 的 MUST 接口实际启动，显示 UNKNOWN/INCOMPLETE/Topic Graph/History；Runtime Bearer Scope、TLS/受信代理及 Web Session/CSRF 通过 AC-18；
6. 退出并核对 Writer/Reader/Pool/Server Stream 均无悬挂内存/FD/活跃写入，保留允许的数据目录和恢复证据。

每个平台需要 Install Log、Dependency Tree、Plugin Trace、Config/Artifact SHA、原始 MCAP、验收输出、Owner 签审；不能把插件“能找到”替代“确实从该路径加载”。

## 3.2 统一编码规范/静态检查

统一代码质量合同：C++17 头文件/实现/namespace/符号命名成体系，include guard 和 ABI/所有权清楚；高频回调禁止磁盘/HTTP/长锁；Raw Pointer 不得暗示强所有权；Mutex、Atomics、exception/noexcept、Fatal/Recoverable 区分；不使用 Debug-only assert 作为生命周期安全保障。

CI 最低执行固定版本 clang-format、Clang 静态检查及 `-Wall -Wextra -Wpedantic`；项目现有 .clang-format 若缺失须经代码评审建立，不能悄悄格式化所有来源文件当成“通过质量验收”。Python 基础 Client 在 Python 3.10/3.12 的语法、导出、Typed DTO、sync/async Close、MockTransport 与 HTTP/SSE Parser 测试均需实际执行；基础 SDK 不引入 ROS Python ABI/应用依赖。检查器应只检查或生成 Diff，未经审查不得自动改写实现并宣布 PASS。


## 3.3 与现有 .clang-format 对齐的代码质量合同

当前仓库已有 `.clang-format`（Google 基准、ColumnLimit=100、IndentWidth=4、PointerAlignment=Left、SortIncludes=false），因此**不应另造不一致的格式默认值**。具体 C++17 编码契约：

| 对象 | 规则 | CI/Review 判据 |
|---|---|---|
| 类型、enum class | PascalCase；enum 枚举值按既有 `kName` 惯例 | 不混用相似名字表示不同领域状态 |
| 函数、局部变量 | snake_case | 保持 API 命名一致，不临时创造别名掩盖迁移 |
| 类成员 | snake_case_ | 私有成员不得借裸指针破坏生命周期 |
| 编译期常量 | kConstantName 或显式 C++17 constexpr | 配置参数/单位不得藏为 magic number |
| 宏 | UPPER_SNAKE | 仅 Header Guard、平台条件或第三方 ABI；不用宏替代普通业务逻辑 |
| 头文件 | 对应头文件、C、C++ STL、第三方、项目内 Include 分组 | 头独立编译；不得在 Header 使用 using namespace |
| 格式 | 遵循仓库实际 clang-format 版本，100列、4空格，逻辑表达式优先可读性 | 复杂函数参数/初始化不挤一行、不套多层 ?: |
| C++ 标准 | C++17；禁止 concepts、ranges、coroutines、modules；不使用 std::expected 假设 | 两发行版 GCC/Clang 均可编译 |
| 资源管理 | RAII/unique_ptr/shared_ptr 所有权有注释；避免业务裸 new/delete | 所有失败/异常分支释放恰好一次；不会析构在用对象 |
| 并发 | 明确执行域、lock-order、happens-before、Atomic 生命周期 | Writer/Executor/Drogon I/O 不互相同步等待；TSan/Barrier |
| 错误 | 可恢复内存不足与确认的 Pool Fatal 区分；noexcept 有证明 | 不靠 release 被禁用的 assert 保证安全 |
| 日志/注释 | 公共接口说明输入、输出、线性化/线程、错误/副作用 | 错误有阶段/身份，不能泄漏密钥或 CDR Payload |
| Python SDK | Python 3.10 可用语法、Typed DTO、Async Cancel/Close 明确 | PyCompile/Test/Formatter 受锁定版本执行 |

静态检查建议先运行 `clang-format --dry-run --Werror`（固定工具版本并明确选择的源码集合），然后以 `-Wall -Wextra -Wpedantic` 编译；更严格的 `-Werror` 是否能覆盖第三方头编译警告须单独配置，不得以禁用全部警告换取构建通过。需要更改 Format Style 时应先审查既有代码的可读性 Diff，拒绝在功能性 PR 中一次性自动格式化全仓导致历史无法审计。

变更审查至少检查本次使用的 Header Include、异常安全、移动语义、C++17 不可复制资源、Thread/Executor/Pool shutdown 的锁序与引用；底层逻辑性能优化要有可重复 Benchmark 证据，不凭局部微基准推翻安全路径。生产准入必须同时满足代码正确性与实际通过验证，格式检查 PASS 绝不是数据安全 PASS。

# 4. G0～G3 证据、TSan 风险与发布决策

## 4.1 Gate 文件与输入

| Gate | 文件 | 最低合格证据 | Gate 结果 |
|---|---|---|---|
| G0 | docs/g0_admission_review.md | Pool/Queue/DataModel 结构、Schema Fixture、项目自有 ASan/TSan、Humble 外部 RMW 风险复现及责任记录 | ALLOWED / ALLOWED_WITH_CONDITIONS / BLOCKED；条件准入仅允许继续开发 |
| G1 | docs/g1_review.md | Recorder/Writer/Recovery/HTTP/Client/SSE 的故障注入、首轮标准负载和问题登记 | ALLOWED / BLOCKED |
| G2 | docs/g2_review.md | Humble/Jazzy × AMD64/ARM64 原生运行、2×2 MCAP 自包含解码、依赖链接验证 | ALLOWED / BLOCKED |
| G3 | docs/g3_release_review.md | AC-01～27 及当前 V-* 测试、OPEN-01/02、AC-18 Security、TSan 最终处置、发布包/哈希/干净安装 | APPROVED / REJECTED |

G0 的 ALLOWED_WITH_CONDITIONS 不等于 G3 可发布。当前 Humble Fast DDS TSan 风险仍 OPEN；必须使用实际 Humble RMW/FastDDS/spdlog 插桩与最小 ROS Reproducer，记录传播栈、缓解/修复证据。即使确认某个告警来自外部依赖，也不说明项目自有 C++ 代码无竞态。确认数据损坏、UAF 或严重并发失效不得凭签字跳过。可以单人同时担任 Tech Lead/Project Owner，但需要如实记录，不能伪造独立审查人。

## 4.2 发布判定的机器门禁

```text
G3 RELEASE APPROVED only if:
  all current V1.0 MUST features implemented
  AND AC-01..27 per required platform PASS
  AND all required verification.md V-* cases evidenced
  AND OPEN-01, OPEN-02 formally closed
  AND AC-18 V1.0 Security PASS
  AND Humble FastDDS TSan risk disposition evidenced and approved
  AND MCAP standalone custom Schema 2x2 PASS
  AND native AMD64/ARM64 runs, sanitizers, 600s standard load PASS
  AND Recovery/SIGKILL and SSE slow client/backpressure PASS
  AND artifacts can clean-install without development source tree
  AND build-manifest, dependency lock, SHA256SUMS, licenses, evidence index exist
  AND Release Owner explicitly records APPROVED.
Otherwise: REJECTED.
```

CI/发布脚本要拒绝缺测试 ID、PENDING/NOT_RUN 伪装为 PASS、找不到的证据文件、Hash 不匹配或未审核的替代测试。修改源码、依赖、Config 或 Docker Base Image 后，受影响的验证证据必须重跑，不能无条件继承Release Candidate 的 PASS。正式发布不是复制文档里曾写过的 FROZEN 字样。
