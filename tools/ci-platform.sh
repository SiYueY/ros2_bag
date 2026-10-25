#!/usr/bin/env bash
# Build on the current native architecture; no transparent QEMU substitution.
set -euo pipefail
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_distro="${1:?usage: ci-platform.sh humble|jazzy amd64|arm64}"
task_arch="${2:?architecture required}"
case "$(uname -m)" in
    x86_64) task_native=amd64 ;;
    aarch64) task_native=arm64 ;;
    *) echo 'Unsupported native architecture' >&2; exit 2 ;;
esac
if [[ "$task_arch" != "$task_native" ]]; then
    echo "BLOCKED: requested $task_arch but Runner is $task_native" >&2
    exit 3
fi
task_image="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]]["image"])' \
    "$task_root/docker/base-images.json" "$task_distro")"
mkdir -p "$task_root/build/ci-evidence"
docker build --build-arg "BASE_IMAGE=$task_image" --build-arg "ROS_DISTRO=$task_distro" \
    -f "$task_root/docker/Dockerfile" -t "ros2-bag:$task_distro-$task_arch" "$task_root" \
    2>&1 | tee "$task_root/build/ci-evidence/$task_distro-$task_arch-build.txt"
docker image inspect "ros2-bag:$task_distro-$task_arch" \
    > "$task_root/build/ci-evidence/$task_distro-$task_arch-image.json"
docker run --rm "ros2-bag:$task_distro-$task_arch" \
    2>&1 | tee "$task_root/build/ci-evidence/$task_distro-$task_arch-tests.txt"

echo "PASS Recorder-only native validation: $task_distro/$task_arch"
