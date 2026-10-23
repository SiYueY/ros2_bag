# Phase 0 deployment boundary

Only build/test tooling and a recorder identity skeleton exist. Run
`BUILD_MAIN_DIR/ros2_bag_recorder --phase0-info` to obtain a machine-readable
`functional_recorder=false` marker. Recording, UDS, CLI and Web are not available.

Supported target combinations remain Humble/Ubuntu 22.04 and Jazzy/Ubuntu 24.04,
each on AMD64/ARM64. Current validation scope is recorded per platform in
`phase0_validation_report.md`; an untested architecture is not supported by
inference. Fast DDS (`rmw_fastrtps_cpp`) and the MCAP storage plugin are required.
Preserve the user's already sourced ROS overlay.

The preview packaging command is deliberately explicit:

```bash
bash tools/package.sh --phase0-preview build/humble-amd64/main humble build/package-preview
cd build/package-preview
sha256sum --check SHA256SUMS
tar -xJf ros2_bag.tar.xz
ROS_DISTRO=humble ./ros2_bag/ros2_bag recorder --phase0-info
```

The preview has a `PHASE0_ONLY.json` marker, one native platform, licenses and
dependency/build metadata. It must not be advertised as the four-platform V1.0
product. Production packaging without the preview switch fails closed. The
packager refuses to overwrite an existing preview directory.

No pip dependencies are needed by project tooling (except ROS-provided Python
bindings used by the fixture writer). Docker is a developer/test dependency,
not a user runtime requirement. Disk use of downloaded ROS images can be large;
the exact host capacity at validation is in the evidence directory.
