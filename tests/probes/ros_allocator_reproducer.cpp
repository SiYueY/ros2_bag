// Phase 0 diagnostic baseline: constructs a ROS node without any probe pool,
// queue or payload adapter. Used to isolate sanitizer reports in ROS/RMW.
#include <exception>
#include <iostream>
#include <memory>

#include <rclcpp/rclcpp.hpp>

int main(
    int argc,
    char** argv
) {
    try {
        rclcpp::init(
            argc,
            argv
        );
        {
            const std::shared_ptr<rclcpp::Node> node =
                std::make_shared<rclcpp::Node>("phase0_ros_baseline");
        }
        rclcpp::shutdown();
        std::cout << "PASS minimal ROS node baseline\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
