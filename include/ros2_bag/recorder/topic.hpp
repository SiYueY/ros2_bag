#ifndef ROS2_BAG_RECORDER_TOPIC_HPP_
#define ROS2_BAG_RECORDER_TOPIC_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/error.hpp"

namespace ros2_bag::recorder {

using TopicId = std::uint64_t;
using OperationId = std::string;

enum class QosMode { Auto, Explicit };
enum class Reliability { Reliable, BestEffort };
enum class Durability { Volatile, TransientLocal };
enum class History { KeepLast, KeepAll };

struct QosOptions {
    QosMode mode = QosMode::Auto;
    std::optional<Reliability> reliability;
    std::optional<Durability> durability;
    std::optional<History> history;
    std::optional<std::uint32_t> depth;
};

struct TopicOptions {
    std::string name;
    std::string type;
    std::optional<QosOptions> qos;
    std::optional<double> expected_hz;
};

struct TopicInfo {
    TopicId topic_id = 0;
    std::uint64_t topic_epoch = 0;
    std::string name;
    std::string type;
    QosOptions qos;
    std::optional<std::string> type_hash;
    std::string schema_sha256;
    std::optional<double> expected_hz;
};

enum class TopicRecordability { Recordable, TypeConflict, TypeUnavailable, QosIncompatible };

struct DiscoveredTopic {
    std::string name;
    std::vector<std::string> types;
    std::size_t publisher_count = 0;
    TopicRecordability recordability = TopicRecordability::Recordable;
    std::vector<QosOptions> publisher_qos_profiles;
    bool publisher_qos_profiles_truncated = false;
};

Result<QosOptions, Error> normalize_qos(const std::optional<QosOptions>& options);
Result<void, Error> validate_topic_options(const TopicOptions& options);

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_TOPIC_HPP_
