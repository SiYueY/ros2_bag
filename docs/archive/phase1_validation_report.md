> **CURRENT STATUS: HISTORICAL VALIDATION.** 本报告记录原实现下的验证事实，不代表 Recorder Only 收尾后的完整平台验收。

# Phase 1 validation report

Design version: v4.3.8. Report date: 2026-10-24 (Asia/Shanghai).

This report records current local evidence only. It is not a G1 approval record.

## Passing local evidence

| Scope | Environment | Command/result |
|---|---|---|
| Complete local CTest | Humble AMD64 container | `ctest --test-dir build/g1-humble --output-on-failure`: 12/12 PASS (Coordinator integration 55.30 s, including AC-90 delay) |
| Complete local CTest | Jazzy AMD64 container | `ctest --test-dir build/g1-jazzy --output-on-failure`: 12/12 PASS (Coordinator integration 53.91 s, including AC-90 delay) |
| Pool/Queue pressure | Humble AMD64 Debug | `pool_queue_stress`: 1,000,000 messages, repeated three times, PASS |
| Pool/Queue pressure | Humble AMD64 ASan | `ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 pool_queue_stress_test`: PASS |
| Pool/Queue pressure | Jazzy AMD64 Debug | `pool_queue_stress_test`: PASS |
| Coordinator path | Humble AMD64 Debug | 1,000 String messages, dynamic GenericSubscription, MCAP Reader CDR deserialization: PASS |
| WriterAdapter per-write owner boundary | Humble/Jazzy AMD64 Debug | 20,000 1 KiB-class CDR messages; application owner expired after every synchronous write and complete MCAP readback matched: PASS |
| WriterAdapter owner ASan | Humble AMD64 ASan | Same 20,000-message per-write owner and MCAP readback test with `abort_on_error=1:detect_leaks=0`: PASS |
| WriterAdapter RSS | Humble AMD64 Debug | 300 s at 100 Hz (30,000 messages), per-second VmRSS regression slope 0.0126594 MiB/min, below 1 MiB/min diagnostic threshold; MCAP readback matched: PASS for this writer-path workload |
| Dynamic Topic lifecycle | Humble/Jazzy AMD64 Debug | add → remove → same-name re-add preserves TopicId and increments Topic Epoch; 1,000-message MCAP readback: PASS |
| External Topic control | Humble AMD64 Debug and ASan | Test-main-thread add/remove/re-add while the Executor spins, 1,000-message MCAP readback: PASS after wait-set notifications |
| AC-41 dynamic Topic stress | Humble/Jazzy AMD64 Debug | External-control thread performs 3,000 add/remove cycles while the Recorder Executor runs: PASS |
| Coordinator restart | Humble AMD64 Debug | Stop first task, start/stop second task in same Coordinator: PASS |
| Coordinator restart | Jazzy AMD64 Debug | 1,000-message task then same-instance Stop→Start: PASS |
| AC-91 normal generation transition | Humble/Jazzy AMD64 Debug | One Coordinator performs 100 consecutive empty Stop→Start cycles and is IDLE after every stop: PASS for normal transition; injected stale-message violation remains untested |
| Timed mode | Humble/Jazzy AMD64 Debug | 100 ms monotonic wall timer triggers stop and commits an empty-segment manifest: PASS |
| AC-48 zero-message cases | Humble/Jazzy AMD64 Debug CTest | Immediate stop, `allow_empty_topics=true`, all Topics removed, paused with no messages, and registered Topic with no publication each produce a readable zero-message MCAP segment plus manifest: PASS |
| Rolling segmentation | Humble/Jazzy AMD64 Debug | 1 s rotation produces `segment_000000` and `segment_000001`; each is independently opened with rosbag2 Reader: PASS |
| Snapshot buffer unit | Humble AMD64 Debug | Per-Topic message/byte and global-byte eviction, removed-epoch pruning and replay marker: PASS (`production_core`) |
| Rolling segmentation | Humble AMD64 ASan | Full Coordinator lifecycle with 1 s rotation and 1,000-message normal path: PASS (`detect_leaks=0`) |
| Rolling retention | Humble/Jazzy AMD64 Debug | 1 s retention deletes the first COMPLETE segment asynchronously after durable `DELETING`; later segment remains independently readable: PASS |
| Rolling retention while paused | Humble AMD64 Debug | After a segment becomes COMPLETE, user PAUSE leaves the rolling timer active; a later timer tick durably marks and asynchronously removes the expired segment: PASS |
| AC-90 delayed cleanup | Humble/Jazzy AMD64 Debug containers | Coordinator integration injects a 35-second segment deletion after retention scheduling; `status()` completes in under 200 ms while deletion is in flight, and both evidence containers exited 0: PASS for injected delay. CleanupWorker unit coverage also propagates an injected deletion failure and its error text; actual filesystem-error injection remains incomplete. |
| Disk protection | Humble/Jazzy AMD64 Debug | Forced `min_free_gb` critical sample causes system PAUSED while `resource.enabled=false`; direct user Resume is rejected: PASS |
| AC-89 disk safety transitions | Humble/Jazzy AMD64 Coordinator CTest | `start` creates the output root then samples disk; an unavailable root is rejected with `DISK_STATE_UNKNOWN` before Writer setup. Forced CRITICAL pauses and rejects user Resume. An injected NORMAL→CRITICAL→stable NORMAL sequence resumes only after the configured stability window: PASS. Repeated failure, stale and blocked sampling remain incomplete. |
| DELETING recovery | Humble AMD64 Debug | Recovery removes durable DELETING directory and atomically changes manifest to REMOVED: PASS |
| Conservative incomplete recovery | Humble AMD64 unit | WRITING becomes INCOMPLETE, PREPARED-only topic operation is written as UNCERTAIN in recovery.json, and no Segment is removed: PASS |
| UDS/CLI status path | Humble AMD64 Debug container | Recorder socket accepts two independent JSONL `status` connections; `./bag_cli status` succeeds twice against the running Recorder: PASS |
| UDS full status schema | Humble AMD64 Debug container | A real Recorder response to JSONL `status` contained the shared schema-v1 fields (`session_id`, instance ID, topic/process/disk/system snapshots and `updated_at_ns`) and then accepted `shutdown`: PASS |
| CLI managed lifecycle | Humble AMD64 Debug container | `bag_cli start --task-id managed-smoke` locates `build/g1-humble/ros2_bag_recorder`, starts a Session-owned Recorder, then start/status/stop complete: PASS |
| CLI graph topic start | Humble AMD64 Debug container | A live `std_msgs/msg/String` publisher was discovered by `bag_cli start --topics /cli_start_topic`; status showed TopicId 1/Epoch 1, then stop and same-process restart/stop completed: PASS with one Executor worker |
| CLI graph topic ASan | Humble AMD64 ASan container | Live publisher plus managed `start --topics` and stop completed with one Executor worker and `ASAN_OPTIONS=abort_on_error=1:detect_leaks=0`: PASS |
| CLI/Web protocol foundations | Humble AMD64 Python CTest | CLI JSONL status and existing-Recorder start requests; Web loopback bearer-token status endpoint and 401 rejection: PASS |
| SSE status failure | Humble/Jazzy AMD64 Python CTest | An established SSE stream emits a valid `error` event when status becomes stale, instead of attempting a second HTTP response: PASS |
| CLI PENDING contract | Humble/Jazzy AMD64 Python CTest | A temporary UDS returns PENDING; CLI exits 4 and prints request ID, operation ID and `operation-status --request-id` query: PASS |
| CLI PENDING contract | AMD64 Python CTest | A temporary UDS returns PENDING; CLI exits 4 and prints request ID, operation ID and `operation-status --request-id` query: PASS |
| Session/status smoke | Humble AMD64 Debug | Independent XDG runtime Session creates 0700 directory, PID/lock, 128-bit instance ID and 1 Hz atomic status.json; SIGINT removes PID: PASS |
| AC-21 Recorder SIGINT | Humble AMD64 Debug container | Current Recorder created Socket/PID, received SIGINT, exited 0, and Docker file inspection confirmed `recorder.pid` was removed: PASS |
| Process status telemetry | Humble AMD64 Debug | Running task status.json reports CPU/RSS/FD/thread and system-memory/CPU-count fields after ResourceMonitor sampling: PASS |
| Topic actual_hz and gap | Humble/Jazzy AMD64 Debug | Fixed 10 ms buckets over a 200 ms window report a live ~100 Hz topic, then report gap after two complete low-rate windows: PASS |
| Writer Topic counters | Humble/Jazzy AMD64 Debug | Writer-thread per-TopicId/Epoch snapshots report successful writes and zero write errors during MCAP readback integration: PASS |
| Removed Topic status | Humble/Jazzy AMD64 Debug | remove/re-add preserves TopicId, advances Epoch, and exposes a bounded REMOVED history with total count: PASS |
| IDLE process telemetry | Humble AMD64 Debug | Before any task starts, periodic ResourceMonitor sampling writes non-null process RSS and `sampling_enabled=true` in IDLE status.json: PASS |
| Status parser | Humble/Jazzy AMD64 Python CTest | Schema-v1 fresh document accepted and stale document rejected: PASS |
| AC-92 status lifecycle | Humble/Jazzy AMD64 CTest | Atomic writer rejects a missing parent path; parser rejects missing, corrupt and stale files; Web returns 503 for stale status; UI clears old state when `recorder_instance_id` changes: PASS for specified injected boundaries. |
| AC-54 implemented Topic capacity | Humble/Jazzy AMD64 CTest | A 128 KiB atomic status file carries 64 ACTIVE plus 32 REMOVED Topic snapshots and parses with exact counts: PASS for implemented Topic capacity. Pending-operation, secondary-error and cross-UDS/SSE maximum payload coverage remain incomplete. |
| Release packager | AMD64 local Python/shell | Synthetic hash-consistent four-platform inputs yield documented archive layout and SHA256SUMS; incomplete inputs are rejected: PASS (packager only, not a release) |
| Runtime dependencies | Humble/Jazzy AMD64 container | `tools/verify-runtime-deps.sh` confirms all ELF NEEDED entries resolve and none resolves from a private build directory: PASS |
| Coordinator path | Humble AMD64 ASan | Executor-scheduled dynamic Topic lifecycle and 1,000-message MCAP readback: PASS |
| Coordinator path | Jazzy AMD64 Debug | 10-message MCAP readback: PASS |

## Commands

```bash
docker run --rm --entrypoint bash --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -w /work ros2-bag-phase0:humble-amd64 \
  -lc 'source /opt/ros/humble/setup.bash &&
       cmake -S . -B build/g1-humble -DCMAKE_BUILD_TYPE=Debug \
         -DPython3_EXECUTABLE=/usr/bin/python3 &&
       cmake --build build/g1-humble --parallel 2 &&
       ctest --test-dir build/g1-humble --output-on-failure'
```

## Not complete

- G0 remains BLOCKED: no real Tech Lead/Project Owner approval. Model A now has
  bounded per-write owner validation on AMD64, but native ARM64 and a Humble
  full-stack TSan disposition are still open.
- Humble Fast DDS reproduces an ASan `__rmw_wait` segmentation fault when a
  dynamic GenericSubscription is removed while the Recorder uses four Executor
  workers. The latest reproduction has the concrete stack
  `rmw_fastrtps_shared_cpp::__rmw_wait → rmw_wait → rcl_wait →
  rclcpp::Executor::wait_for_work → MultiThreadedExecutor::run`; its faulting
  worker is created by the test Executor. The shipped development configuration
  therefore uses one `MultiThreadedExecutor` worker as containment. This is not
  multi-worker acceptance and needs upstream resolution or a formal architecture
  decision before G1/G3.
- Humble Pool/Queue TSan built but terminated before execution with
  `unexpected memory mapping`; it is not a pass.
- ARM64 native execution, remote CI and release artifacts are not run.
- Full recovery, UDS/CLI/Web/SSE and AC-01 through AC-94 remain incomplete.
  DELETING and conservative PREPARED/INCOMPLETE recovery are implemented;
  retained incomplete segments now have rosbag2 Reader metadata/message evidence,
  but raw MCAP Schema/Channel closure and cleanup timeout diagnostics are not.
- CLI managed-start and Web control relay code exist, but complete start/stop/
  pause/resume/topic-control end-to-end acceptance, browser UI, Cookie/CSRF
  and remote TLS are not complete.
- AC-41 passed in Humble/Jazzy AMD64 Debug after TopicManager moved subscription
  creation, removal and task cleanup to a Node-Executor timer boundary:
  `ROS2_BAG_TEST_TOPIC_STRESS_CYCLES=3000 coordinator_recording_test 10`.
  The prior direct-RMW path exited with signal 11; it is retained as a fixed
  regression, not counted as passing evidence for older implementations.

## Dynamic-entity execution boundary

The lifecycle test normally schedules add/remove/re-add and stop on the Recorder
node's Executor. A previous direct-control run failed in Humble Fast DDS
`rmw_fastrtps_shared_cpp::__rmw_wait`; it was not payload-lifetime evidence.
The production implementation dispatches subscription creation, removal and
task cleanup through one-shot Node-Executor timers. A 3,000-cycle Humble Debug
external-control regression now passes. Jazzy, TSan and ARM64 still need their
own evidence.


## Current-worktree targeted regression (2026-10-24)

| Scope | Environment | Command/result |
|---|---|---|
| Coordinator dynamic Topic → MCAP regression after current recovery/status/UDS changes | Humble AMD64 container, Fast DDS, `ROS_DOMAIN_ID=173` | `timeout 80s ./build/snapshot-humble/coordinator_recording_test 10`: exit 0. The test emitted expected zero-message MCAP metadata warnings while checking its empty-segment cases. |

An initial attempt with `ROS_DOMAIN_ID=243` was invalid for the Fast DDS port
mapping and is excluded from this result. This is a focused regression, not a
replacement for the complete CTest, long-duration, sanitizer, ARM64, or release
acceptance matrices.
