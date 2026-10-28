#ifndef ROS2_BAG_RECORDER_RECOVERY_HPP_
#define ROS2_BAG_RECORDER_RECOVERY_HPP_

#include <filesystem>
#include <string>
#include <vector>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/error.hpp"

namespace ros2_bag::recorder {

struct RecoveryFinding {
    std::string recording_id;
    std::string code;
    std::string message;
};

struct RecoveryReport {
    std::vector<RecoveryFinding> findings;
};

class Recovery {
public:
    static Result<RecoveryReport, Error> run(const std::filesystem::path& output_root);
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_RECOVERY_HPP_
