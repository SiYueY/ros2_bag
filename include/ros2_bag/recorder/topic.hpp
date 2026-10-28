#ifndef ROS2_BAG_RECORDER_TOPIC_HPP_
#define ROS2_BAG_RECORDER_TOPIC_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/error.hpp"

namespace ros2_bag::recorder {

using TopicId = std::uint64_t;
using OperationId = std::string;

inline constexpr std::uint32_t kMaxQosDepth = 10000;

enum class Reliability { Reliable, BestEffort };
enum class Durability { Volatile, TransientLocal };
enum class History { KeepLast, KeepAll };

struct QosOptions {
    struct Auto {};

    struct Explicit {
        Reliability reliability = Reliability::Reliable;
        Durability durability = Durability::Volatile;
        History history = History::KeepLast;
        std::uint32_t depth = 10;
    };

    using Mode = std::variant<Auto, Explicit>;

    Mode mode = Auto{};
};

struct QosSelection {
    QosOptions qos;
    std::size_t compatible_publishers = 0;
    bool all_compatible = true;
};

struct TopicOptions {
    std::string name;
    std::string type;
    QosOptions qos;
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
    struct Publishers {
        std::size_t count = 0;
        std::vector<QosOptions> profiles;
        bool truncated = false;
    };

    std::string name;
    std::vector<std::string> types;
    TopicRecordability recordability = TopicRecordability::Recordable;
    Publishers publishers;
};

Result<QosOptions, Error> normalize_qos(const QosOptions& options);
bool qos_compatible(const QosOptions& publisher, const QosOptions& subscription) noexcept;
Result<QosSelection, Error> select_qos(
    const QosOptions& options,
    const std::vector<QosOptions>& publisher_profiles,
    std::size_t publisher_count,
    bool profiles_truncated);
Result<void, Error> validate_topic_options(const TopicOptions& options);

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_TOPIC_HPP_
