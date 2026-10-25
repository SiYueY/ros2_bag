// Immutable task-local topic identity and supported ROS QoS configuration.
#ifndef ROS2_BAG_TOPIC_INFO_HPP_
#define ROS2_BAG_TOPIC_INFO_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ros2_bag {

using TopicId = std::uint64_t;

constexpr TopicId kInvalidTopicId = 0;

enum class Reliability {
    kBestEffort,
    kReliable,
};

enum class Durability {
    kVolatile,
    kTransientLocal,
};

enum class History {
    kKeepLast,
    kKeepAll,
};

// Supported QoS fields. Unsupported ROS QoS options are rejected by configuration
// validation instead of being silently ignored.
struct TopicQos {
    Reliability reliability = Reliability::kBestEffort;
    Durability durability = Durability::kVolatile;
    History history = History::kKeepLast;
    std::size_t depth = 10;
};

// Bounds the repeat-on-segment snapshot held by the Writer thread.
struct RepeatPolicy {
    bool enabled = false;
    std::size_t max_messages = 64;
    std::size_t max_bytes = 4194304;
};

// Immutable identity used by one recording generation. It is safe to share across
// callback, coordinator and Writer threads after construction.
struct TopicInfo {
    TopicId id = kInvalidTopicId;
    std::string name;
    std::string type;
    std::string serialization_format = "cdr";
    TopicQos qos;
    std::optional<std::string> type_hash;
    std::optional<double> expected_hz;
    bool qos_is_explicit = false;
    RepeatPolicy repeat_policy;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_TOPIC_INFO_HPP_
