# Phase 0 native builds

`base-images.json` pins multi-architecture ROS base manifests and records both
AMD64/ARM64 child digests. `tools/ci-platform.sh DISTRO ARCH` refuses an architecture
mismatch; the Humble ARM64 job uses an ARM64 Ubuntu 24.04 Runner and a pinned
Humble Ubuntu 22.04 container. No QEMU performance claim is made.

Build and run locally:

```bash
bash tools/ci-platform.sh humble amd64
bash tools/ci-platform.sh jazzy amd64
```

The image builds the independent message fixture first, then the C++17 probes,
and runs CTest. `docker run --rm IMAGE` repeats the runtime probes. This is a
development/test image, not the V1.0 product. The recorder binary is a Phase 0
skeleton with `--phase0-info`, not a functioning recorder.

PR workflows have read-only permissions, no publishing, no robot access and no
host network/IPC sharing. The explicitly trusted local DDS experiment is run by
`tools/test-dds-containers.sh`; do not run it on unreviewed external PR code.

Actual GitHub Runner eligibility remains unverified until the owner pushes these
workflows and executes them. Creating these files is not CI PASS.
