# Native baseline validation

`base-images.json` pins multi-architecture ROS base manifests and records both
AMD64/ARM64 child digests. `tools/ci-platform.sh DISTRO ARCH` refuses an
architecture mismatch; it validates the current dependency-free CMake/CTest
baseline in a native container.

```bash
bash tools/ci-platform.sh humble amd64
bash tools/ci-platform.sh jazzy amd64
```

This is development infrastructure only. No Recorder runtime, message fixture,
MCAP acceptance harness, or host-network DDS experiment exists on `next`.
