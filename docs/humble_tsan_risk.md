# Humble Fast DDS / TSan risk record

Document version: development risk log

Status: OPEN

The existing Phase 0 Humble AMD64 Executor TSan run reports Fast DDS 2.6 and
spdlog mutex diagnostics. The same diagnostic class was reproduced with a
minimal ROS node that does not use the ros2_bag Pool, queue or WriterAdapter.
This establishes that the current evidence does not attribute the report to
ros2_bag, but does not prove the dependency-stack warning harmless.

## Evidence

- `docs/evidence/phase0/humble-tsan-runtime.txt`: project Executor probe fails.
- `docs/evidence/phase0/tsan-ros-baseline.txt`: minimal ROS reproducer fails.
- `docs/evidence/phase0/tsan-core-final.txt` and
  `docs/evidence/phase0/tsan-payload.txt`: project-owned non-DDS scopes pass.
- `docs/evidence/phase0/jazzy-tsan-final.txt`: Jazzy Executor scope passes.

The paths above are references to historical local evidence, not files tracked
in Git. The 2026-10-25 Recorder-only cleanup preserved the original local
evidence externally before clearing docs/evidence/. A fresh checkout must
reproduce the diagnostics independently; historical results do not establish
current release acceptance.

## Impact and containment

The warning can mask project races in a full Humble DDS run. Recorder development therefore
uses focused ASan/TSan coverage for project-owned Pool, queue and control code,
keeps the full Humble test unsuppressed, and does not label it PASS.

Before any release decision, reproduce with a coherently instrumented Humble ROS/RMW/Fast DDS/
spdlog stack, compare supported transport settings, and either resolve,
demonstrably mitigate, or obtain explicit risk acceptance. Transport changes
must still prove discovery, receipt, throughput, latency, CPU and data
integrity.

Tech Lead approval: PENDING.

Project Owner approval: PENDING.
