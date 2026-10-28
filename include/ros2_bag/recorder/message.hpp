#ifndef ROS2_BAG_RECORDER_MESSAGE_HPP_
#define ROS2_BAG_RECORDER_MESSAGE_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>

#include <rclcpp/serialized_message.hpp>

#include "ros2_bag/recorder/topic.hpp"

namespace ros2_bag::recorder {

struct RecordedMessage {
    std::uint64_t generation = 0;
    TopicId topic_id = 0;
    std::uint64_t topic_epoch = 0;
    std::shared_ptr<rclcpp::SerializedMessage> message;
    std::size_t payload_bytes = 0;
    std::int64_t receive_timestamp_ns = 0;
    bool replayed = false;

    // Keeps the frozen topic identity alive after Remove while queued data drains.
    std::shared_ptr<const TopicInfo> topic;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_MESSAGE_HPP_
