# Next-generation baseline validation image. BASE_IMAGE is pinned by CI.
ARG BASE_IMAGE=ros:humble-ros-base
FROM ${BASE_IMAGE}
ARG BASE_IMAGE
ARG ROS_DISTRO=humble
ARG INSTALL_DEPS=1
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN if [[ "${INSTALL_DEPS}" == 1 ]]; then \
      apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake git python3 python3-dev; \
    fi
WORKDIR /work
COPY . /work
# Git verifies the actual gitlinks; local development may include uncommitted source files.
RUN git config --global --add safe.directory '*' && \
    cmake -S /work -B /work/build/next -DBUILD_TESTING=ON && \
    cmake --build /work/build/next --parallel 2 && \
    ctest --test-dir /work/build/next --output-on-failure
ENTRYPOINT []
CMD ["ctest", "--test-dir", "/work/build/next", "--output-on-failure"]
