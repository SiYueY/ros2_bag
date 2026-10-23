#!/usr/bin/env bash
# Preserve sourced overlays and select the native platform executable.
set -euo pipefail
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_distro="${ROS_DISTRO:?source Humble or Jazzy before starting}"
case "$task_distro" in humble|jazzy) ;; *) echo 'Unsupported ROS_DISTRO' >&2; exit 2 ;; esac
case "$(uname -m)" in
    x86_64) task_arch=amd64 ;;
    aarch64) task_arch=arm64 ;;
    *) echo 'Unsupported architecture' >&2; exit 2 ;;
esac
if [[ "${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}" != rmw_fastrtps_cpp ]]; then
    echo 'Only rmw_fastrtps_cpp is supported' >&2
    exit 2
fi
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
exec "$task_root/$task_distro/$task_arch/ros2_bag_recorder" "$@"
