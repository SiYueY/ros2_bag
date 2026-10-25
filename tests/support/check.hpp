// Phase 0 test assertions. Failures remain active in Release builds.
#ifndef ROS2_BAG_TESTS_SUPPORT_CHECK_HPP_
#define ROS2_BAG_TESTS_SUPPORT_CHECK_HPP_

#include <stdexcept>
#include <string>

namespace ros2_bag::phase0 {
// Throws runtime_error on a false condition; no shared state is touched.
inline void require(bool condition, const std::string& description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}
}  // namespace ros2_bag::phase0
#endif  // ROS2_BAG_TESTS_SUPPORT_CHECK_HPP_
