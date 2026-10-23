#!/usr/bin/env bash
# Native L2 custom recording and clean installed-PlotJuggler acceptance.
# Optional existing analysis image permits local reuse without another apt build.
set -euo pipefail
task_distro="${1:?usage: test-ac55-platform.sh humble|jazzy [EXISTING_ANALYSIS_IMAGE]}"
case "$task_distro" in
    humble|jazzy) ;;
    *) echo 'Unsupported ROS distro' >&2; exit 2 ;;
esac
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
case "$(uname -m)" in
    x86_64) task_arch=amd64 ;;
    aarch64) task_arch=arm64 ;;
    *) echo 'Unsupported native architecture' >&2; exit 2 ;;
esac
task_recording="ros2-bag-phase0:$task_distro-$task_arch"
task_analysis="${2:-ros2-bag-ci-analysis:$task_distro-$task_arch}"
task_output="$(mktemp -d "$task_root/build/ac55-$task_distro-$task_arch-XXXXXX")"
mkdir -p "$task_root/build/ci-evidence"
task_log="$task_root/build/ci-evidence/ac55-$task_distro-$task_arch.txt"
if [[ $# -lt 2 ]]; then
    task_base="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]]["image"])' \
        "$task_root/docker/base-images.json" "$task_distro")"
    docker build --build-arg "BASE_IMAGE=$task_base" --build-arg "ROS_DISTRO=$task_distro" \
        -f "$task_root/docker/Dockerfile.analysis" -t "$task_analysis" "$task_root" \
        > "$task_log" 2>&1
fi
{
    docker run --rm --entrypoint bash --user "$(id -u):$(id -g)" \
        -v "$task_output:/output" "$task_recording" -lc \
        'source /opt/ros/$ROS_DISTRO/setup.bash &&
         source /work/build/docker-native/message-install/share/ros2_bag_test_msgs/local_setup.bash &&
         python3 /work/tests/probes/write_custom_bag.py /output/input &&
         /work/build/docker-native/main/phase0_ros_probe rewrite-custom \
           /output/input /output/cpp /work/config/mcap.yaml &&
         cp /output/cpp/*.mcap /output/recording.mcap'
    python3 "$task_root/tools/inspect-mcap-schema.py" --expect-custom \
        "$task_output/recording.mcap"
    # Mount just one MCAP. No recording package, prefix or source tree is exposed.
    docker run --rm --network none --entrypoint bash \
        -v "$task_output/recording.mcap:/fixtures/recording.mcap:ro" "$task_analysis" -lc \
        'source /opt/ros/$ROS_DISTRO/setup.bash &&
         bash /analysis/verify-clean-analysis-env.sh && test ! -e /work &&
         timeout 45 /analysis/phase0_plotjuggler_probe \
           /opt/ros/$ROS_DISTRO /fixtures/recording.mcap'
} >> "$task_log" 2>&1
echo "PASS native AC-55 $task_distro/$task_arch: $task_log"
