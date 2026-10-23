# Phase 0 SerializedPayload decision — pending final review

Date: 2026-10-23 (Asia/Shanghai). Design: ros2_bag v4.3.3.

## Decision and precise representation

The **technically recommended candidate is model A**:

```cpp
std::shared_ptr<rclcpp::SerializedMessage>
```

It is not yet a formally approved Phase 1 type. AMD64 Humble and Jazzy have
compiled and exercised this owning callback API. ARM64 and the Humble full ROS
TSan gate remain outstanding; no reviewer signature is invented.

The proposed production RecordedMessage holds this owner, immutable TopicInfo,
generation/epoch and timestamps as prescribed by the design. The probe pool's
`shared_ptr<const void>` is explicitly a test-only type erasure so synthetic and
ROS payloads share the same lifetime experiment; it is **not** a replacement for
the frozen production payload field.

## Allocation and release ownership

GenericSubscription supplies an owning shared_ptr. The callback retains that
owner, records the buffer address/length and returns. After Executor shutdown,
the consumer waits 500 ms, verifies the same address/length and deserializes all
100 sequence values. Both pool modes exercise four actual Executor callback
threads and return every wrapper after cross-thread release.

`tests/support/payload_binding.hpp` binds the original owner to a
SerializedBagMessage descriptor. Only `rcutils_uint8_array_t` descriptor metadata
is copied. A descriptor deleter retains both the original owner and descriptor;
it never calls release_rcl_serialized_message and never frees a borrowed buffer.
The resulting buffer pointer equals the input pointer. A counted rcutils allocator
test proves one allocation and one matching release, with release deferred until
the last descriptor owner disappears. The allocator state outlives all objects.

This proves no added full-payload copy in this probe forwarding/binding path.
It does not claim DDS/RMW zero copy or no copies inside MCAP compression/storage.
The independent custom fixture writer records payload-size quantiles in its output;
both distributions were P50=117 B and P95/P99/max=128 B
(`humble-custom-size-probe.txt`, `jazzy-custom-size-probe.txt`). These fixture
sizes do not constitute the robot's production size distribution.

## WriterAdapter and snapshot boundary

The actual C++ writer probe writes the same retained payload into two independent
MCAP bags, closes each and independently reads all 100 values and timestamps.
The payload remains valid after the first close and can be replayed into the next
bag. This specifically covers the mutable shared_ptr overload's buffer-transfer
hazard seen in the Humble API/source inspection.

Humble uses TopicMetadata registration and plugin-side installed definition lookup;
its installed public Writer API lacks Jazzy's explicit MessageDefinition overload.
Jazzy additionally compiles and executes LocalMessageDefinitionSource::get_full_text
and Writer::create_topic(topic, definition). Humble maps the receive timestamp to
time_stamp; Jazzy maps receive/send timestamps separately. No Humble send-time
preservation beyond that API is claimed. These are tested adapter capabilities,
not a new timestamp-policy decision for Phase 1.

The Python writer first serializes deterministic custom fixtures. A further C++
probe reads those owned CDR records, explicitly registers their actual TopicMetadata
and (on Jazzy) complete MessageDefinitions, and writes 20 records using the design's
LZ4 config. The retained AC-55 files are the **C++ outputs**, not merely the Python
inputs. Their chunks, complete definitions and every parsed field are verified by
the independent PlotJuggler loader/parser matrix without the original message package.
The additional official Humble build exercises rosbag2_cpp 0.15.17 as well as the
original container's 0.15.16; this is recorded separately in the final C++ logs.

## Pool boundary and alternatives

Pool nodes own wrappers only; A uses zero cached-payload budgets. Snapshot owners
are separate from wrapper/QueueBudget lifetimes and must be charged to SnapshotBudget
in Phase 1. PoolState shared ownership protects TLS caches and cross-thread handles;
the test-only deleter's shared state is not the production raw-pool declaration.

TLS and global-mutex experiments both pass native correctness tests and ASan.
Core TSan and Jazzy actual Executor TSan pass. Humble actual Executor TSan reports
external-library mutex diagnostics, also reproduced by a minimal ROS node without
our pool. This remains an unresolved validation gate, not evidence to silently
switch to Plan B or discard the pool.

B is not needed because A works and snapshots require shared ownership. C would
add an unnecessary full-payload copy here. D has no demonstrated allocation
bottleneck justifying a slab implementation. Neither C nor D is implemented.

## Evidence

All paths below are relative to `docs/evidence/phase0/`:

- `humble-build-test.txt`, `jazzy-build-test.txt`: real API compilation and CTest.
- `humble-final-cpp-tests.txt`, `jazzy-final-cpp-tests.txt`: final five-test suites
  and C++ custom-schema registration/LZ4 output; `custom-cpp-provenance.json`.
- `humble-asan.txt`, `jazzy-asan.txt`: real callback/Pool ownership under ASan.
- `asan-core.txt`, `asan-payload.txt`: independent memory/allocator validation.
- `tsan-aslr-attempt.txt`, `tsan-payload.txt`, `jazzy-tsan-runtime.txt`: clean scoped TSan runs.
- `humble-tsan-runtime.txt`, `tsan-ros-baseline.txt`: unresolved Humble runtime diagnostics.
- `humble-custom-schema.json`, `jazzy-custom-schema.json`: actual dependency closure.
- `ac55-*-to-*.txt`, `fixtures/*.mcap`: complete field/value/time checks in isolated images.
- `*-environment.txt`, generated build manifests: package versions and binary/source identity.

The validation runs used baseline `5a737ef8ed569def6eb2b79502b1e22d925d1311`
with an uncommitted implementation. The later local source commit does not change
that historical build identity. Build manifests retain `working_tree_dirty` and
file hashes so the baseline is not mistaken for the tested source revision.

## 评审记录

评审日期：待正式评审。

评审人：待 Tech Lead 或指定技术负责人。

评审结论：PENDING；不能据此进入依赖最终数据模型的 Phase 1。

选定数据模型：技术推荐 A；正式冻结选择尚待批准。

测试日志：上述 evidence 文件及 phase0_validation_report.md。

验证时源代码 SHA：当时的基线 SHA 加未提交源码清单/哈希，见构建证据；后续本地提交不改写原始证据。

仍需处理问题：ARM64、实际远端 CI/Runner、Humble ROS TSan 诊断及最终评审。
