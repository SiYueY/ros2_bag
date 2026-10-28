# Next-generation C++ implementation baseline. Recorder implementation targets
# intentionally do not exist on this branch yet.
cmake_minimum_required(VERSION 3.16)
project(ros2_bag VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

find_package(rclcpp QUIET)
find_package(rosbag2_cpp QUIET)
find_package(OpenSSL REQUIRED)

if(rclcpp_FOUND AND rosbag2_cpp_FOUND)
  add_library(ros2_bag_recorder
    src/recorder/config.cpp
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
    src/recorder/schema.cpp)
  target_include_directories(ros2_bag_recorder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(ros2_bag_recorder PUBLIC cxx_std_17)
  target_link_libraries(ros2_bag_recorder PUBLIC rclcpp::rclcpp rosbag2_cpp::rosbag2_cpp OpenSSL::Crypto)
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
