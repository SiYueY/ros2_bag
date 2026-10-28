# Next-generation C++ implementation baseline. Recorder implementation targets
# intentionally do not exist on this branch yet.
cmake_minimum_required(VERSION 3.16)
project(ros2_bag VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_executable(result_test tests/unit/result_test.cpp)
  target_include_directories(result_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(result_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  add_test(NAME result_unit COMMAND result_test)
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
