# ros2_bag

`next` is the initialization branch for the next-generation C++ implementation.
It inherits the complete `develop` history, but intentionally contains no
legacy Recorder runtime, library, API, lifecycle, or compatibility wrapper.

The design documents in [`docs/`](docs/) remain the baseline for the new work,
including [`docs/result.md`](docs/result.md) and
[`docs/cpp_coding_guidelines.md`](docs/cpp_coding_guidelines.md). The prior
implementation remains available in `develop` only as reference material for
validated ROS 2, rosbag2, MCAP, Humble, and Jazzy behavior.

## Baseline checks

Initialize pinned third-party dependencies before running tooling checks:

```bash
git submodule update --init --recursive
bash tools/verify-submodules.sh
cmake -S . -B build/next
ctest --test-dir build/next --output-on-failure
python3 tools/format-cpp.py --check
```

There is deliberately no Recorder executable or C++ implementation target at
this stage. `config/mcap.yaml`, dependency locking, formatting, and general
tooling remain available for the forthcoming implementation.
