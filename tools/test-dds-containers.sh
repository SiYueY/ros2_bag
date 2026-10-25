#!/usr/bin/env bash
# Trusted local test: the named container must already use host network and IPC.
# Receives all 100 unique sequences with reliable/transient-local QoS per case.
set -euo pipefail
if [[ $# -ne 5 ]]; then
    echo 'usage: test-dds-containers.sh CONTAINER HOST_PROBE CONTAINER_PROBE LABEL DOMAIN_ID' >&2
    exit 2
fi
task_container="$1"
task_host_probe="$2"
task_container_probe="$3"
task_label="$4"
task_domain="$5"
if [[ ! "$task_domain" =~ ^[0-9]+$ ]] || (( task_domain < 1 || task_domain > 230 )); then
    echo 'DOMAIN_ID must be an integer from 1 to 230; next domain is reserved too' >&2
    exit 2
fi
task_cross_domain=$((task_domain + 1))
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_logs="$task_root/build/local-dds-evidence"
task_network="$(docker inspect --format '{{.HostConfig.NetworkMode}}' "$task_container")"
task_ipc="$(docker inspect --format '{{.HostConfig.IpcMode}}' "$task_container")"
if [[ "$task_network" != host || "$task_ipc" != host ]]; then
    echo 'BLOCKED: this experiment requires an explicitly trusted host-network/IPC container' >&2
    exit 3
fi
mkdir -p "$task_logs" "$task_root/build/host-dds-logs"
# Both participants inside one container.
docker exec --user "$(id -u):$(id -g)" -e "ROS_DOMAIN_ID=$task_domain" -e "ROS_LOG_DIR=/tmp/phase0-dds-logs-$(id -u)" \
    "$task_container" bash -lc 'source /opt/ros/$ROS_DISTRO/setup.bash; exec "$@"' -- \
    "$task_container_probe" subscribe > "$task_logs/$task_label-container-subscriber.txt" 2>&1 &
task_subscriber=$!
docker exec --user "$(id -u):$(id -g)" -e "ROS_DOMAIN_ID=$task_domain" -e "ROS_LOG_DIR=/tmp/phase0-dds-logs-$(id -u)" \
    "$task_container" bash -lc 'source /opt/ros/$ROS_DISTRO/setup.bash; exec "$@"' -- \
    "$task_container_probe" publish > "$task_logs/$task_label-container-publisher.txt" 2>&1
wait "$task_subscriber"
# Publisher on host, GenericSubscription inside the container.
docker exec --user "$(id -u):$(id -g)" -e "ROS_DOMAIN_ID=$task_cross_domain" -e "ROS_LOG_DIR=/tmp/phase0-dds-logs-$(id -u)" \
    "$task_container" bash -lc 'source /opt/ros/$ROS_DISTRO/setup.bash; exec "$@"' -- \
    "$task_container_probe" subscribe > "$task_logs/$task_label-cross-subscriber.txt" 2>&1 &
task_subscriber=$!
ROS_DOMAIN_ID="$task_cross_domain" RMW_IMPLEMENTATION=rmw_fastrtps_cpp \
    ROS_LOG_DIR="$task_root/build/host-dds-logs" \
    "$task_host_probe" publish > "$task_logs/$task_label-host-publisher.txt" 2>&1
wait "$task_subscriber"
echo "PASS DDS $task_label: container-local and host-to-container sequence/QoS tests"
