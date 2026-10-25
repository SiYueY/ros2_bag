# ros2_bag

ROS 2 C++17 Recorder core, currently in a **Recorder-only development state**.
The project targets ROS 2 Humble/Jazzy on Ubuntu 22.04/24.04 (AMD64/ARM64),
with MCAP storage through rosbag2_cpp.

This is **not** a V1.0 release and **not** the completed four-layer system.

## Current scope

The repository currently develops and validates only the Recorder:

- Dynamic ROS 2 GenericSubscription topic handling and QoS.
- RecordingCoordinator lifecycle, normal/timed/rolling recording modes.
- Bounded message queue, message pool, WriterAdapter and MCAP/LZ4 storage.
- Snapshot recording buffer, manifest/event log, recovery and retention cleanup.
- Process and disk resource sampling and Recorder-native status snapshots.

The previous UDS/JSONL control server, Python IPC, CLI, and Python Web/SSE
gateway were retired during Recorder-only closeout. No HTTP Server, Python Client,
CLI, TUI or Web Application is currently provided. They are planned under the
separately selected technology stack, but are **not implemented here**.

## Build and test

Initialize pinned submodules:

```bash
git submodule update --init --recursive
bash tools/verify-submodules.sh
```

A full Recorder/MCAP test environment requires the matching ROS 2
rosbag2_cpp and rosbag2_storage_mcap packages. Before running recording
tests on a host, verify the plugin:

```bash
source /opt/ros/humble/setup.bash
ros2 pkg prefix rosbag2_storage_mcap
```

For Ubuntu 22.04 / Humble with tests enabled (the default):


```bash
source /opt/ros/humble/setup.bash
cmake -S . -B build/humble -DCMAKE_BUILD_TYPE=Debug -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/humble --parallel 2
ROS_DOMAIN_ID=218 ctest --test-dir build/humble --output-on-failure
```

To build and install **only** the Recorder development runtime, without
test executables or test-only std_msgs dependencies:

```bash
source /opt/ros/humble/setup.bash
cmake -S . -B build/humble-minimal -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/humble-minimal --parallel 2
cmake --install build/humble-minimal --prefix build/humble-install
```

Only the Recorder executable and configuration are installed. The internal
ros2_bag_core static library is **not yet an installed C++ SDK** and no public
CMake Target/header export is promised for external consumers.

For Jazzy, source the matching setup and use a separate build directory.
Use a ROS_DOMAIN_ID isolated from other active ROS 2 systems.

The minimal standalone executable can be started for initialization and
shutdown verification:

```bash
source /opt/ros/humble/setup.bash
build/humble/ros2_bag_recorder --config config/recorder.yaml
```

The standalone runtime holds a nonblocking advisory lock in its resolved
output directory before recovery, preventing two cooperating Recorder
processes from modifying the same recording root concurrently. The lock file
persists after shutdown; only the OS lock is released.

**Important:** The Recorder-only executable intentionally exposes **no external
control interface**. It remains idle until terminated; it does **not** start a
recording from the command line. C++ unit/integration tests call Recorder-native
methods to exercise actual recording and MCAP readback. Do not rely on the old
CLI/Web commands or assume this executable is a complete end-user recorder.

For code-format and dependency checks:

```bash
python3 tools/format-cpp.py --check
python3 tests/unit/test_tooling.py
```

## Documentation status

The current top-level documentation contains only the technology selection,
Recorder recovery behavior and still-open Humble TSan risk. Previous design,
research and validation records are stored separately under docs/archive/.
No new architecture or layer-specific design documents were created by this
cleanup.

| Document | Role |
|---|---|
| [System technology selection](docs/system_technology_selection.md) | Future four-layer technology choices, not an implementation claim |
| [Recorder recovery](docs/recovery.md) | Current recovery and output ownership rules |
| [Humble TSan risk](docs/humble_tsan_risk.md) | Unresolved ROS 2/Fast DDS TSan validation risk |
| [Historical v4.3.8 design](docs/archive/ros2_bag.md) | Superseded UDS/CLI/Web development contract |
| [Recorded message research](docs/archive/recorded_message_data_model.md) | Historical payload and ownership evidence |
| [Historical implementation notes](docs/archive/implementation_notes.md) | Engineering history |
| [Phase 0 validation](docs/archive/phase0_validation_report.md) | Historical tests and limitations |
| [Phase 1 validation](docs/archive/phase1_validation_report.md) | Historical tests and limitations |

Old G0-G3 reviews and the old CLI/Web deployment guide were removed from
the current tree, but remain recoverable through Git history. The local
docs/evidence/phase0 results were archived outside the repository before being
cleared; Git never tracked them. New temporary evidence belongs in build/.

Historical approvals cannot be transferred automatically to the current
implementation or future HTTP/SSE system. ARM64, remote CI, sanitizers and
complete V1.0 release readiness require separate evidence.

The tests/performance/standard_profile.json file describes a proposed load
profile; it does **not** have an automated benchmark runner or accepted
performance results.

## Recorder-only closeout verification

On 2026-10-25 the Recorder-only code passed **10/10 CTest tests** in each
of the existing Humble AMD64 and Jazzy AMD64 test containers, including
MCAP recording/readback and output-root process locking. The standalone Humble binary also passed a
SIGINT shutdown smoke test.

The host Ubuntu Humble environment lacks the rosbag2_storage_mcap
plugin, so three host MCAP tests cannot run successfully there until
that runtime dependency is installed. ARM64 was not reverified by this
closeout, and these results are not V1.0 product acceptance.

## Development boundary

Recorder-domain APIs and tests are retained. Future communication and UI
components must not be reintroduced until their respective designs are reviewed.
The project does not provide backward compatibility for the retired UDS/CLI/Web
implementations.
