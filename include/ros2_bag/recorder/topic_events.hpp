#ifndef ROS2_BAG_RECORDER_TOPIC_EVENTS_HPP_
#define ROS2_BAG_RECORDER_TOPIC_EVENTS_HPP_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/error.hpp"
#include "ros2_bag/recorder/topic.hpp"

namespace ros2_bag::recorder {

enum class TopicEventType {
    AddPrepared,
    TopicAdded,
    AddFailed,
    RemovePrepared,
    TopicRemoved,
    RemoveFailed,
};

enum class TopicOperationState { ConfirmedSuccess, ConfirmedFailure, Uncertain, NotFound };

struct TopicEvent {
    std::uint64_t event_seq = 0;
    std::string recording_id;
    std::uint64_t generation = 0;
    OperationId operation_id;
    TopicId topic_id = 0;
    std::uint64_t topic_epoch = 0;
    TopicEventType type = TopicEventType::AddPrepared;
    std::int64_t timestamp_ns = 0;
    std::optional<Error> error;
};

class TopicEvents {
public:
    static Result<TopicEvents, Error> open(
        const std::filesystem::path& task_directory,
        std::string recording_id,
        std::uint64_t generation);

    Result<std::uint64_t, Error> append(TopicEvent event);
    Result<TopicOperationState, Error> operation_state(const OperationId& operation_id) const;
    std::uint64_t uncertainty_watermark() const noexcept;

private:
    TopicEvents(
        std::filesystem::path path,
        std::string recording_id,
        std::uint64_t generation,
        std::uint64_t next_sequence);

    static bool terminal_matches(TopicEventType prepared, TopicEventType terminal) noexcept;
    static Result<std::vector<TopicEvent>, Error> read_events(const std::filesystem::path& path);
    static Result<void, Error> validate_chain(const std::vector<TopicEvent>& events);

    std::filesystem::path path_;
    std::string recording_id_;
    std::uint64_t generation_ = 0;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t uncertainty_watermark_ = 0;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_TOPIC_EVENTS_HPP_
