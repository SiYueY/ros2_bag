// Phase 0 WriterAdapter ownership experiment. Only the descriptor is copied.
#ifndef ROS2_BAG_TESTS_SUPPORT_PAYLOAD_BINDING_HPP_
#define ROS2_BAG_TESTS_SUPPORT_PAYLOAD_BINDING_HPP_

#include <cstdint>
#include <memory>
#include <string>

#include <rclcpp/serialized_message.hpp>
#include <rosbag2_storage/serialized_bag_message.hpp>

namespace ros2_bag::phase0 {
// Bind the serialized owner into a bag descriptor without copying CDR bytes.
// Parameters: owning payload, receive time, topic. Returns an owning descriptor.
// Throws bad_alloc; caller serializes Writer access. Original payload is unchanged.
inline std::shared_ptr<rosbag2_storage::SerializedBagMessage> bind_payload(
    const std::shared_ptr<rclcpp::SerializedMessage>& owner,
    std::int64_t timestamp,
    const std::string& topic) {
    // Only the small rcutils descriptor is copied. Its bytes remain owned by
    // owner, captured by the descriptor deleter. No release() and no payload copy.
    const auto descriptor =
        std::make_shared<rcutils_uint8_array_t>(owner->get_rcl_serialized_message());
    const std::shared_ptr<rosbag2_storage::SerializedBagMessage> bag =
        std::make_shared<rosbag2_storage::SerializedBagMessage>();
    bag->serialized_data = std::shared_ptr<rcutils_uint8_array_t>(
        descriptor.get(),
        [owner, descriptor](rcutils_uint8_array_t*) {});
    bag->topic_name = topic;
#ifdef ROS2_BAG_JAZZY
    bag->recv_timestamp = timestamp;
    bag->send_timestamp = timestamp - 1;
#else
    bag->time_stamp = timestamp;
#endif
    return bag;
}

}  // namespace ros2_bag::phase0
#endif  // ROS2_BAG_TESTS_SUPPORT_PAYLOAD_BINDING_HPP_
