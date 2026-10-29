# Unified C++17 build with Recorder implementation and verification targets.
cmake_minimum_required(VERSION 3.16)
project(ros2_bag VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

find_package(rclcpp QUIET)
find_package(rosbag2_storage QUIET)
find_package(rosbag2_storage_mcap QUIET)
find_package(ament_index_cpp QUIET)
find_package(mcap_vendor QUIET)
find_package(OpenSSL REQUIRED)

if(rclcpp_FOUND AND rosbag2_storage_FOUND AND rosbag2_storage_mcap_FOUND AND ament_index_cpp_FOUND AND mcap_vendor_FOUND)
  add_library(ros2_bag_recorder
    src/recorder/config.cpp
    src/recorder/completion.cpp
    src/recorder/error.cpp
    src/recorder/message_pool.cpp
    src/recorder/message_queue.cpp
    src/recorder/resource_monitor.cpp
    src/recorder/snapshot.cpp
    src/recorder/manifest.cpp
    src/recorder/topic_events.cpp
    src/recorder/recorder.cpp
    src/recorder/recovery.cpp
    src/recorder/cleanup.cpp
    src/recorder/writer.cpp
    src/recorder/topic.cpp
    src/recorder/admission.cpp
    src/recorder/schema.cpp
    src/recorder/state.cpp)
  target_include_directories(
    ros2_bag_recorder
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/concurrentqueue
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include)
  target_compile_features(ros2_bag_recorder PUBLIC cxx_std_17)
  target_link_libraries(ros2_bag_recorder PUBLIC rclcpp::rclcpp rosbag2_storage::rosbag2_storage ament_index_cpp::ament_index_cpp mcap_vendor::mcap OpenSSL::Crypto)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ros2_bag_recorder PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_executable(result_test tests/unit/result_test.cpp)
  target_include_directories(result_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(result_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  add_test(NAME result_unit COMMAND result_test)

  if(TARGET ros2_bag_recorder)
    add_executable(recorder_foundation_test tests/unit/recorder/foundation_test.cpp)
    target_link_libraries(recorder_foundation_test PRIVATE ros2_bag_recorder)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(recorder_foundation_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME recorder_foundation_unit COMMAND recorder_foundation_test)

    add_executable(recorder_correctness_test tests/unit/recorder/correctness_test.cpp)
    target_link_libraries(recorder_correctness_test PRIVATE ros2_bag_recorder)
    target_compile_options(recorder_correctness_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_options(recorder_correctness_test PRIVATE
      -Wl,--wrap=fsync -Wl,--wrap=statvfs
      -Wl,--wrap=_ZN8ros2_bag8recorder6Writer4openERKNSt10filesystem7__cxx114pathE
      -Wl,--wrap=_ZN8ros2_bag8recorder6Writer14register_topicERKNS0_9TopicInfoE
      -Wl,--wrap=_ZN8ros2_bag8recorder6Writer11write_batchERKSt6vectorINS0_15RecordedMessageESaIS3_EE
      -Wl,--wrap=_ZN8ros2_bag8recorder6Writer5closeEv
      -Wl,--wrap=_ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE)
    add_test(NAME recorder_correctness_unit COMMAND recorder_correctness_test)
    set_tests_properties(recorder_correctness_unit PROPERTIES TIMEOUT 90)

    add_executable(
      recorder_data_path_benchmark
      tests/performance/recorder_data_path_benchmark.cpp)
    target_link_libraries(recorder_data_path_benchmark PRIVATE ros2_bag_recorder)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(recorder_data_path_benchmark PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
  add_test(
    NAME result_compile_fail
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_result_compile_fail.py
      --compiler
      ${CMAKE_CXX_COMPILER}
      --include
      ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(
    NAME tooling_dependency_metadata
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_tooling.py)

  set_tests_properties(
    result_unit
    result_compile_fail
    tooling_dependency_metadata
    PROPERTIES TIMEOUT 90)
endif()

# Keep this an ament package when configured from a sourced ROS environment,
# while allowing dependency-free CMake configuration during early development.
find_package(ament_cmake QUIET)
if(ament_cmake_FOUND)
  install(DIRECTORY config/ DESTINATION share/${PROJECT_NAME}/config)
  ament_package()
else()
  message(STATUS "ament_cmake not found: configuring next-generation baseline only")
endif()
