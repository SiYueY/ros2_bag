# Recorder development native build/test image. BASE_IMAGE is pinned by CI.
ARG BASE_IMAGE=ros:humble-ros-base
FROM ${BASE_IMAGE}
ARG BASE_IMAGE
ARG ROS_DISTRO=humble
ARG INSTALL_DEPS=1
ENV RMW_IMPLEMENTATION=rmw_fastrtps_cpp
ENV ROS_LOG_DIR=/tmp/ros2-bag-logs
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN if [[ "${INSTALL_DEPS}" == 1 ]]; then \
      apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake git python3 python3-dev \
        ros-${ROS_DISTRO}-rclcpp ros-${ROS_DISTRO}-std-msgs \
        ros-${ROS_DISTRO}-rosbag2-cpp ros-${ROS_DISTRO}-rosbag2-py \
        ros-${ROS_DISTRO}-rosbag2-storage-mcap ros-${ROS_DISTRO}-rmw-fastrtps-cpp \
        ros-${ROS_DISTRO}-rosidl-default-generators; \
    fi
WORKDIR /work
COPY . /work
# Git verifies the actual gitlinks; local development may include uncommitted source files.
RUN git config --global --add safe.directory '*' && \
    source /opt/ros/${ROS_DISTRO}/setup.bash && \
    ROS2_BAG_BASE_IMAGE=${BASE_IMAGE} bash tools/run-phase0.sh docker-native none
ENTRYPOINT []
CMD ["bash", "-lc", "source /opt/ros/$ROS_DISTRO/setup.bash && ctest --test-dir /work/build/docker-native/main --output-on-failure"]
