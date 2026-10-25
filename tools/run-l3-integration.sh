#!/usr/bin/env bash
# Run the bounded L3 Recorder integration subset from a verified native image.
set -euo pipefail

task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_distro="${1:?usage: run-l3-integration.sh humble|jazzy amd64|arm64}"
task_arch="${2:?architecture required}"
case "$task_distro" in
    humble|jazzy) ;;
    *) echo "unsupported ROS distro: $task_distro" >&2; exit 2 ;;
esac
case "$task_arch" in
    amd64|arm64) ;;
    *) echo "unsupported architecture: $task_arch" >&2; exit 2 ;;
esac

task_image="ros2-bag:$task_distro-$task_arch"
if ! docker image inspect "$task_image" >/dev/null 2>&1; then
    echo "missing native image: $task_image; run tools/ci-platform.sh first" >&2
    exit 2
fi

task_evidence="$task_root/build/ci-evidence/l3-$task_distro-$task_arch.txt"
mkdir -p "$(dirname "$task_evidence")"
docker run --rm "$task_image" bash -lc "
    set -eo pipefail
    source /opt/ros/\$ROS_DISTRO/setup.bash
    export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
    export ROS_DOMAIN_ID=\${ROS_DOMAIN_ID:-231}
    ctest --test-dir /work/build/docker-native/main --output-on-failure \\
      -R '^(production_core|coordinator_recording|writer_adapter_lifetime|pool_queue_stress)$'
" 2>&1 | tee "$task_evidence"

echo "PASS L3 integration $task_distro/$task_arch: $task_evidence"
