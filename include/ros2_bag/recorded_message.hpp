// Formal Recorder payload wrapper. It owns no CDR bytes beyond the ROS owner.
#ifndef ROS2_BAG_RECORDED_MESSAGE_HPP_
#define ROS2_BAG_RECORDED_MESSAGE_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>

#include <rclcpp/serialized_message.hpp>

#include "ros2_bag/topic_info.hpp"

namespace ros2_bag {

enum class TimestampSource {
    kUnknown,
    kReceive,
    kSource,
};

// Task message passed from a GenericSubscription callback to the Writer. The
// shared SerializedMessage keeps CDR bytes alive without a Recorder-side copy.
struct RecordedMessage {
    std::uint64_t recording_generation = 0;
    TopicId topic_id = kInvalidTopicId;
    std::uint64_t topic_epoch = 0;
    std::shared_ptr<const TopicInfo> topic_info;
    std::shared_ptr<rclcpp::SerializedMessage> serialized_message;
    std::size_t payload_bytes = 0;
    std::int64_t recv_timestamp_ns = 0;
    std::int64_t send_timestamp_ns = 0;
    TimestampSource timestamp_source = TimestampSource::kUnknown;
    bool is_replayed = false;
};

// Restores the safe empty state before a Node or fallback allocation is reused.
void reset_recorded_message(RecordedMessage* message) noexcept;

}  // namespace ros2_bag

#endif  // ROS2_BAG_RECORDED_MESSAGE_HPP_
