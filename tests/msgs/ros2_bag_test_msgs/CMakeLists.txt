# Independent interface fixture; never a production recorder dependency.
cmake_minimum_required(VERSION 3.16)
project(ros2_bag_test_msgs LANGUAGES C CXX)
find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(builtin_interfaces REQUIRED)
rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/MotorState.msg" "msg/JointState.msg" "msg/TestTimestamp.msg"
  "msg/RobotState.msg" "msg/MixedRobotState.msg"
  DEPENDENCIES builtin_interfaces)
ament_export_dependencies(rosidl_default_runtime)
ament_package()
