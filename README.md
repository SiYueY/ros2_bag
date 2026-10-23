# ros2_bag — Phase 0

This checkout implements capability probes for design v4.3.3. It does **not**
implement the Phase 1 recorder or Phase 2 CLI/Web. No V1.0 release is provided.

Read [the validation report](docs/phase0_validation_report.md),
[the data model decision](docs/recorded_message_data_model.md), and
[implementation notes](docs/implementation_notes.md) before using the probes.

```bash
git submodule update --init --recursive
bash tools/verify-submodules.sh
source /opt/ros/humble/setup.bash  # or Jazzy, on its supported Ubuntu
bash tools/run-phase0.sh humble-amd64
```

The main package never links the fixture message package. The script builds the
fixture in a separate prefix and sources it only for tests. CMake also supports a
non-ROS experiment build:

```bash
cmake -S . -B build/core -DROS2_BAG_WITH_ROS=OFF
cmake --build build/core --parallel 2
ctest --test-dir build/core --output-on-failure
```

Use independent build directories with `-DROS2_BAG_SANITIZER=address` or `thread`.
ASan and TSan are never combined. Read the report for the Humble prebuilt-runtime
diagnostics and the process-local ASLR requirement observed with GCC TSan.

`python3 tools/format-cpp.py --check` applies the existing `.clang-format` plus
the requested stricter one-argument-per-line formatting; it never formats vendors.
See [Docker instructions](docker/README.md) for native four-platform CI jobs and
the isolated PlotJuggler harness. Workflows are local files until the owner elects
to publish them; their existence does not establish that GitHub CI ran.

Local validation logs, generated manifests and MCAP bags live under
`docs/evidence/`, which is ignored by Git. They remain available in the validation
workspace; regenerate them when validating a fresh checkout. The reports describe
the recorded experiments and retain the outstanding Phase 0 gates.
