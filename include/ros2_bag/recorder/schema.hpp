#ifndef ROS2_BAG_RECORDER_SCHEMA_HPP_
#define ROS2_BAG_RECORDER_SCHEMA_HPP_

#include <string>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/error.hpp"

namespace ros2_bag::recorder {

struct ResolvedSchema {
    std::string root_type;
    std::string encoding = "ros2msg";
    std::string definition;
    std::string sha256;
};

std::string schema_sha256(const std::string& definition);
Result<void, Error> validate_type_support(const std::string& type);
Result<ResolvedSchema, Error> resolve_schema(const std::string& type);

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_SCHEMA_HPP_
