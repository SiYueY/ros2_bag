# Phase 0 validation report

Date: 2026-10-23 (Asia/Shanghai). Design: v4.3.3. Scope: Phase 0 only.

**Exit result: BLOCKED. Phase 0 has not passed; dependent Phase 1 work may not start.**

PASS means the stated scope was actually executed, not that a file or API exists.
FAIL means an executed check produced a failing result. BLOCKED means external
hardware, remote execution or required review is missing. NOT RUN is explicitly
untested work. Evidence paths below are relative to `docs/evidence/phase0/`.

## Task-by-task status

| User requirement | Status | Executed evidence and scope |
|---|---|---|
| 1. C++17/CMake/package.xml/test engineering | PASS (AMD64 Humble/Jazzy) | Both distro native container builds and five CTests; executable honestly reports Phase 0 only |
| 2. Four independent pinned submodules/verifier | PASS | `submodules.txt`; positive and isolated dirty/SHA/URL negative fixture tests; lock generator executed |
| 3. Docker/CI four-platform L1/L2/Runner | BLOCKED overall | Both dcl-based and both official pinned AMD64 jobs passed; native ARM64 and actual GitHub runs NOT RUN |
| 4. Fast DDS local-container and host/container | PASS (AMD64) | `dds-*-summary.txt`, subscriber/publisher logs; 100 unique sequences per case, reliable/transient-local match, actual received CDR MCAP counts/readback |
| 5. SerializedPayload API/lifetime/allocator/copy behavior | PASS for AMD64 experiments; final decision BLOCKED | Humble/Jazzy callback builds, delayed values/address identity, one-allocation/one-release test; formal choice pending ARM64/review |
| 6. WriterAdapter/MCAP/Schema | PASS (AMD64 Humble/Jazzy) | Actual two-bag write/read/replay and timestamps; Jazzy explicit MessageDefinition; C++ custom/mixed registration, LZ4 writes and full schemas |
| 7. move-only MessageQueue/MessageHandle/BudgetLease | PASS (native core) | TLS/global modes, concurrent producers, bulk dequeue, failure/destruction release, limits, repeated task queues; sanitizer core evidence |
| 8. PoolState/TLS/global-mutex/Executor/sanitizers | BLOCKED overall | Native correctness and ASan pass for both modes; Jazzy full ROS TSan passes; Humble full ROS TSan FAIL with external-library diagnostics |
| 9. AC-55 independent fixtures and clean PlotJuggler | PASS (AMD64 fixed 2×2 matrix) | Isolated analysis images, no fixture package/type support, network disabled; 26 numeric and 2 string series ×10 samples plus timestamps |
| 10. lock/runtime/packaging skeleton | PASS for Phase 0 tooling | Lock generation, ELF resolution and explicitly incomplete native preview; no production release claim |

The design's Phase 0 fourteen-item checklist is covered as follows: items 1–2 by
engineering/submodules; items 3–5 by Docker/L1/L2 with remote/ARM64 execution still
blocked; item 6 by DDS; items 7–8 by payload/Writer; items 9–10 by queue/Pool with
the Humble TSan gate unresolved; item 11 by AC-55; item 12 by the pending-review
decision record; item 13 by the independently built fixture package; item 14 by
the guarded preview packager. None of the incomplete items are declared passed.

## Environment and identity

- Host: Ubuntu 22.04.5 AMD64, GCC 11.4, CMake 3.22.1, clang-format 14; Docker 29.1.1.
- Humble container: Ubuntu 22.04, rclcpp 16.0.21, rosbag2_cpp 0.15.16,
  rosbag2_storage_mcap 0.15.17, rmw_fastrtps_cpp 6.2.10, Fast DDS 2.6.12.
- Jazzy container: Ubuntu 24.04, rclcpp 28.1.22, rosbag2_cpp/MCAP 0.26.11,
  rmw_fastrtps_cpp 8.4.4, Fast DDS 2.14.7.
- PlotJuggler core 3.17.2 in both analysis environments. Loader/parser binary
  hashes, exact Debian versions and actual base image IDs are retained.
- No usable native ARM64 target was supplied. Actual remote Runner eligibility
  remains unverified because the new workflows have not been published/executed.
- Validation baseline Git SHA: `5a737ef8ed569def6eb2b79502b1e22d925d1311`.
  These experiments ran before the source commit; their manifests retain the
  original dirty source hashes. A later user request authorizes the local source
  commit only. It does not turn these historical manifests into clean-commit
  builds, remote CI evidence or a Release.

Pinned third-party SHAs:

| Dependency | Upstream version selection | Actual gitlink |
|---|---|---|
| concurrentqueue | v1.0.4 | 6dd38b8a1dbaa7863aa907045f32308a56a6ff5d |
| sockpp | v1.0.0 | e6c4688a576d95f42dd7628cefe68092f6c5cd0f |
| nlohmann/json | v3.11.3 | 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 |
| yaml-cpp | 0.8.0 | f7320141120f720aecc4c32be25586e7da9eb978 |

## Reproducible commands

Run each ROS build on its supported Ubuntu/native architecture. The captured
local executions used containers `ros2-bag-phase0-humble` and `ros2-bag-phase0-jazzy`
with the checkout mounted at `/work`. Their package installation commands and
outputs are in `*-dependencies.txt`. Those temporary containers were removed
after validation; recreate trusted containers with the appropriate distro image,
`--network host --ipc host -v "$PWD:/work"`, before repeating the named-container
DDS commands below. This is a local trusted test configuration, not the PR runner.

```bash
bash tools/verify-submodules.sh
bash tools/generate-third-party-lock.sh build/third_party.lock
python3 tools/format-cpp.py --check

source /opt/ros/humble/setup.bash  # use Jazzy in the Jazzy container
bash tools/run-phase0.sh humble-amd64
# In the Jazzy container: bash tools/run-phase0.sh jazzy-amd64

cmake -S . -B build/core -DROS2_BAG_WITH_ROS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build/core --parallel 2
ctest --test-dir build/core --output-on-failure
```

CTest core includes concurrent wrapper/lease moves, successful/failed enqueue,
bulk dequeue, outstanding queue destruction, byte/message quota rejection, bounded
fallback, invalid cookie detection, cross-thread release, TLS exit, handle-held
state lifetime and same-worker queue reconstruction across generations. Actual ROS
ownership tests use a real four-thread MultiThreadedExecutor and both pool modes.

Sanitizers use separate build directories:

```bash
cmake -S . -B build/asan-core -DROS2_BAG_WITH_ROS=OFF -DROS2_BAG_SANITIZER=address
cmake --build build/asan-core --parallel 2
ctest --test-dir build/asan-core --output-on-failure

cmake -S . -B build/tsan-core -DROS2_BAG_WITH_ROS=OFF -DROS2_BAG_SANITIZER=thread
cmake --build build/tsan-core --parallel 2
setarch x86_64 -R ctest --test-dir build/tsan-core --output-on-failure
```

For ROS sanitizer builds, keep `ROS2_BAG_WITH_ROS=ON` and execute the
`phase0_payload|phase0_ros_ownership` subset. The trusted TSan container requires
process-local ASLR control (`--security-opt seccomp=unconfined`, then setarch).
No suppressions or sanitizer error-code overrides are used.

| Sanitizer scope | Result | Evidence |
|---|---|---|
| Core pool/queue ASan+UBSan | PASS | `asan-core.txt` |
| Core pool/queue TSan, ASLR disabled for process | PASS | `tsan-aslr-attempt.txt`, final scoped run |
| Independent payload binding ASan / TSan | PASS | `asan-payload.txt`, `tsan-payload.txt` |
| Humble container actual Executor ASan | PASS | `humble-asan.txt` |
| Jazzy actual Executor ASan | PASS | `jazzy-asan.txt` |
| Jazzy actual Executor TSan | PASS | `jazzy-tsan-runtime.txt` |
| Humble actual Executor TSan | FAIL | `humble-tsan-runtime.txt`: Fast DDS/spdlog mutex diagnostics |
| Older host Humble actual Executor ASan | FAIL | `asan-ros.txt`, independently reproduced in `asan-ros-baseline.txt` |
| Older host Humble minimal node TSan | FAIL | `tsan-ros-baseline.txt`, no project pool/queue |

The first TSan startup and Docker personality failures remain in the logs. Later
scoped successes are separate evidence, not rewritten failures. External-library
diagnostics are not proven harmless merely because functional output is correct.

Fast DDS commands (explicitly trusted host network/IPC containers only):

```bash
source /opt/ros/humble/setup.bash
bash tools/test-dds-containers.sh ros2-bag-phase0-humble \
  "$PWD/build/host/phase0_ros_probe" /work/build/humble-amd64/main/phase0_ros_probe humble 181
bash tools/test-dds-containers.sh ros2-bag-phase0-jazzy \
  "$PWD/build/host/phase0_ros_probe" /work/build/jazzy-amd64/main/phase0_ros_probe jazzy 182
```

Both local-container and host-Humble-to-container cases received all sequences.
The final `dds-*-delivery-summary.txt` and `*-delivery-*-subscriber.txt` runs
add actual received-CDR MCAP write/read-back checks (100 values in each case);
`dds-fixtures/` retains those four files. Containers execute participants with the
host UID. Earlier root-run failures remain recorded; matching alone was insufficient.

This scalar-message interoperability observation is not a promise of arbitrary
cross-distro ROS communication or bag playback.

## AC-55 evidence

The independently built fixtures are under `tests/msgs/ros2_bag_test_msgs/`.
Generate a fresh input bag while only the recording process has the fixture overlay:

```bash
source build/humble-amd64/message-install/share/ros2_bag_test_msgs/local_setup.bash
python3 tests/probes/write_custom_bag.py build/humble-amd64/custom-bag-new
build/final-humble/phase0_ros_probe rewrite-custom \
  build/humble-amd64/custom-bag-new build/custom-cpp-new config/mcap.yaml
python3 tools/inspect-mcap-schema.py --expect-custom \
  docs/evidence/phase0/fixtures/humble.mcap
```

Build the dedicated analysis images, then mount **only** the retained MCAP files:

```bash
docker build --build-arg BASE_IMAGE=dcl:humble --build-arg ROS_DISTRO=humble \
  -f docker/Dockerfile.analysis -t ros2-bag-analysis:humble .
docker build --build-arg BASE_IMAGE=dcl:jazzy --build-arg ROS_DISTRO=jazzy \
  -f docker/Dockerfile.analysis -t ros2-bag-analysis:jazzy .
bash tools/test-ac55.sh
```

Retained actual files are generated by the **C++** rewrite probe, which registers
custom TopicMetadata on both distros and explicitly supplies Jazzy MessageDefinitions.
They contain LZ4 chunks, ros2msg schemas, complete custom dependency bodies, Channel
links and 20 messages. All four loader/parser combinations validate both custom
and mixed topics, all 26 numeric and 2 string series, ten values per series, array
indices, nested fields and absolute log times. The analysis image contains no test
package, `/work` or generated type support; package/index checks run before parsing.
The actual installed loader/parser plugins are used, not a replacement decoder.
The final source-built harness binary was additionally mounted read-only into each
clean analysis image for the final matrix; no source tree/type support was mounted.
`custom-cpp-provenance.json` records the C++ output paths and retained hashes.

## Docker, CI and packaging boundary

`docker-*-build.txt` proves two native AMD64 builds using the existing dcl bases.
`official-humble-amd64.txt` additionally proves the pinned official Humble build
and all five runtime tests. `official-jazzy-amd64-retry.txt` proves the same for
Jazzy. Its initial disk-capacity interruption remains recorded separately in
`official-jazzy-resource-limit.txt`.
The exact official-base four-platform matrix is therefore incomplete.

```bash
bash tools/ci-platform.sh humble amd64
bash tools/ci-platform.sh jazzy arm64  # only on a native ARM64 Runner
bash tools/verify-runtime-deps.sh build/host/phase0_ros_probe
bash tools/package.sh --phase0-preview build/final-humble humble build/phase0-delivery
```

The GitHub connector returned no PR-triggered runs for the existing baseline; its
query scope is retained in `github-runs.json`. This does not prove account-wide
Runner eligibility. New local workflows cannot be executed on GitHub without
publishing them; the later local-commit request does not authorize a push. No remote CI PASS is claimed.

L2 additionally invokes `test-ac55-platform.sh` after the native suite. Its actual
local Humble/Jazzy helper executions passed custom C++ registration/writes, schema
inspection and clean installed-plugin parsing (`*-ci-ac55-details.txt`). These local
helper successes do not substitute for remote Runner evidence. The earlier full
2×2 matrix remains the cross-distro parsing evidence.

Final source suites are in `humble-delivery-tests.txt` and
`jazzy-delivery-tests-local-abi.txt`; the failed cross-SDK Jazzy rebuild and the
interrupted Humble helper invocation are explained in `implementation_notes.md`.
The separately executed official Humble runtime suite is
`official-humble-runtime-delivery.txt`.

The preview packager is exercised with licenses, dependency lock, dirty-source
build manifest, SHA256SUMS, safe extraction, exact 0755 modes on all three Bash entries,
fixture-package exclusion and execution of `--phase0-info`. The earlier preview
inherited 0775 modes; exit audit caught this and the delivered preview was rebuilt
with explicit mode normalization.
The final preview evidence is `package-delivery.txt`; artifact:
`build/phase0-delivery/ros2_bag.tar.xz`. Production invocation is deliberately rejected. A native-only preview is not the
required final four-platform release and does not claim Phase 1 functionality.

## Exit audit and remaining work

P0: native ARM64 builds/tests; actual remote L1/L2 and Runner evidence; disposition
of Humble ROS TSan diagnostics; prescribed data-model/Pool review. Alternatives,
affected ACs and ownership are recorded in `implementation_notes.md`.

P1: minimum supported dependency versions for the older host ASan issue and later
production acceptance. No frozen product
scope or architecture was changed to make a test pass.

**Phase 0 remains BLOCKED; no Phase 1 implementation or formal approval is implied.**
