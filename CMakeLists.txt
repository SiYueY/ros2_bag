# Recorder-only C++17 library/runtime. Research probes are built only for testing.
cmake_minimum_required(VERSION 3.16)
project(ros2_bag VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(ROS2_BAG_WITH_ROS "Build the ROS 2 Recorder runtime" ON)
option(ROS2_BAG_WITH_PLOTJUGGLER "Build the optional PlotJuggler test probe" OFF)
set(ROS2_BAG_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE ROS2_BAG_SANITIZER PROPERTY STRINGS none address thread)

if(ROS2_BAG_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(ROS2_BAG_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT ROS2_BAG_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unknown ROS2_BAG_SANITIZER")
endif()

foreach(dependency concurrentqueue json yaml-cpp)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency}/CMakeLists.txt")
    message(FATAL_ERROR "Initialize pinned submodules: git submodule update --init --recursive")
  endif()
endforeach()
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/json EXCLUDE_FROM_ALL)
add_subdirectory(third_party/yaml-cpp EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)
include(CTest)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  function(recorder_test_target target)
    target_include_directories(${target} PRIVATE tests/support third_party/concurrentqueue)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_link_libraries(${target} PRIVATE Threads::Threads)
  endfunction()

  add_executable(phase0_core_probe tests/probes/core_probe.cpp)
  recorder_test_target(phase0_core_probe)
  target_link_libraries(phase0_core_probe PRIVATE nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp)
  add_test(NAME phase0_core COMMAND phase0_core_probe)
  add_test(NAME phase0_tooling COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_tooling.py)
  set_tests_properties(phase0_core phase0_tooling PROPERTIES TIMEOUT 90)
endif()

if(ROS2_BAG_WITH_ROS)
  find_package(ament_cmake REQUIRED)
  find_package(rclcpp REQUIRED)
  find_package(rosbag2_cpp REQUIRED)
  find_package(rosbag2_storage REQUIRED)

  add_library(ros2_bag_core STATIC
    src/cleanup_worker.cpp
    src/config.cpp
    src/manifest.cpp
    src/recorded_message.cpp
    src/recorded_message_pool.cpp
    src/queue_budget.cpp
    src/recording_queue.cpp
    src/recording_coordinator.cpp
    src/recovery.cpp
    src/resource_monitor.cpp
    src/snapshot_buffer.cpp
    src/topic_manager.cpp
    src/writer_adapter.cpp
  )
  # Internal source-tree library; no stable installed C++ SDK is exported yet.
  target_include_directories(ros2_bag_core PUBLIC include third_party/concurrentqueue third_party/json/include)
  target_compile_options(ros2_bag_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  target_compile_definitions(ros2_bag_core PUBLIC ROS2_BAG_PRODUCTION_BUILD=1)
  target_link_libraries(ros2_bag_core PUBLIC Threads::Threads yaml-cpp::yaml-cpp)
  ament_target_dependencies(ros2_bag_core PUBLIC rclcpp rosbag2_cpp rosbag2_storage)
  if("$ENV{ROS_DISTRO}" STREQUAL "jazzy")
    target_compile_definitions(ros2_bag_core PUBLIC ROS2_BAG_JAZZY=1)
  endif()

  add_executable(ros2_bag_recorder src/main.cpp)
  target_compile_options(ros2_bag_recorder PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  target_link_libraries(ros2_bag_recorder PRIVATE ros2_bag_core)
  ament_target_dependencies(ros2_bag_recorder PUBLIC rclcpp)
  install(TARGETS ros2_bag_recorder DESTINATION lib/${PROJECT_NAME})
  install(DIRECTORY config/ DESTINATION share/${PROJECT_NAME}/config)

  if(BUILD_TESTING)
    find_package(std_msgs REQUIRED)

    add_executable(production_core_test tests/unit/production_core_test.cpp)
    recorder_test_target(production_core_test)
    target_link_libraries(production_core_test PRIVATE ros2_bag_core)
    add_test(NAME production_core COMMAND production_core_test)

    add_executable(coordinator_recording_test tests/integration/coordinator_recording_test.cpp)
    recorder_test_target(coordinator_recording_test)
    target_link_libraries(coordinator_recording_test PRIVATE ros2_bag_core)
    ament_target_dependencies(coordinator_recording_test PUBLIC rclcpp std_msgs)
    add_test(NAME coordinator_recording COMMAND coordinator_recording_test 10)
    set_tests_properties(coordinator_recording PROPERTIES TIMEOUT 90)

    add_executable(writer_adapter_lifetime_test tests/integration/writer_adapter_lifetime_test.cpp)
    recorder_test_target(writer_adapter_lifetime_test)
    target_link_libraries(writer_adapter_lifetime_test PRIVATE ros2_bag_core)
    ament_target_dependencies(writer_adapter_lifetime_test PUBLIC rclcpp std_msgs rosbag2_cpp rosbag2_storage)
    add_test(NAME writer_adapter_lifetime COMMAND writer_adapter_lifetime_test)
    set_tests_properties(writer_adapter_lifetime PROPERTIES TIMEOUT 90)

    add_executable(pool_queue_stress_test tests/integration/pool_queue_stress_test.cpp)
    recorder_test_target(pool_queue_stress_test)
    target_link_libraries(pool_queue_stress_test PRIVATE ros2_bag_core)
    add_test(NAME pool_queue_stress COMMAND pool_queue_stress_test)
    set_tests_properties(pool_queue_stress PROPERTIES TIMEOUT 60)

    add_executable(phase0_ros_allocator_reproducer tests/probes/ros_allocator_reproducer.cpp)
    recorder_test_target(phase0_ros_allocator_reproducer)
    ament_target_dependencies(phase0_ros_allocator_reproducer PUBLIC rclcpp)

    add_executable(phase0_payload_probe tests/probes/payload_probe.cpp)
    recorder_test_target(phase0_payload_probe)
    ament_target_dependencies(phase0_payload_probe PUBLIC rclcpp std_msgs rosbag2_storage)
    if("$ENV{ROS_DISTRO}" STREQUAL "jazzy")
      target_compile_definitions(phase0_payload_probe PRIVATE ROS2_BAG_JAZZY=1)
    endif()
    add_test(NAME phase0_payload COMMAND phase0_payload_probe)

    add_executable(phase0_ros_probe tests/probes/ros_probe.cpp)
    recorder_test_target(phase0_ros_probe)
    if("$ENV{ROS_DISTRO}" STREQUAL "jazzy")
      target_compile_definitions(phase0_ros_probe PRIVATE ROS2_BAG_JAZZY=1)
    endif()
    ament_target_dependencies(phase0_ros_probe PUBLIC rclcpp std_msgs rosbag2_cpp rosbag2_storage)
    add_test(NAME phase0_ros_ownership COMMAND phase0_ros_probe ownership)
    add_test(NAME phase0_ros_writer COMMAND phase0_ros_probe writer ${CMAKE_CURRENT_BINARY_DIR}/probe_bags ${CMAKE_CURRENT_SOURCE_DIR}/config/mcap.yaml)
    set_tests_properties(phase0_ros_ownership phase0_ros_writer PROPERTIES TIMEOUT 90)

    add_test(NAME recorder_instance_lock COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_recorder_instance.py
      $<TARGET_FILE:ros2_bag_recorder> ${CMAKE_CURRENT_SOURCE_DIR}/config/recorder.yaml)
    set_tests_properties(recorder_instance_lock PROPERTIES TIMEOUT 30)
  endif()
endif()

if(ROS2_BAG_WITH_PLOTJUGGLER)
  if(NOT BUILD_TESTING)
    message(FATAL_ERROR "ROS2_BAG_WITH_PLOTJUGGLER requires BUILD_TESTING=ON")
  endif()
  find_package(Qt5 REQUIRED COMPONENTS Widgets Xml)
  find_path(PLOTJUGGLER_INCLUDE_DIR PlotJuggler/dataloader_base.h PATHS /opt/ros/$ENV{ROS_DISTRO}/include REQUIRED)
  find_library(PLOTJUGGLER_BASE_LIBRARY plotjuggler_base PATHS /opt/ros/$ENV{ROS_DISTRO}/lib REQUIRED)
  add_executable(phase0_plotjuggler_probe tests/probes/plotjuggler_probe.cpp)
  recorder_test_target(phase0_plotjuggler_probe)
  target_include_directories(phase0_plotjuggler_probe SYSTEM PRIVATE ${PLOTJUGGLER_INCLUDE_DIR})
  target_link_libraries(phase0_plotjuggler_probe PRIVATE Qt5::Widgets Qt5::Xml ${PLOTJUGGLER_BASE_LIBRARY})
endif()

if(ROS2_BAG_WITH_ROS)
  ament_package()
endif()
