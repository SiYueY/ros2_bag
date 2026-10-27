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
  add_test(
    NAME tooling_dependency_metadata
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_tooling.py)
  set_tests_properties(tooling_dependency_metadata PROPERTIES TIMEOUT 90)
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
