#ifndef ROS2_BAG_RECORDER_SNAPSHOT_HPP_
#define ROS2_BAG_RECORDER_SNAPSHOT_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ros2_bag/recorder/message.hpp"

namespace ros2_bag::recorder {

struct SnapshotConfig {
    std::size_t max_messages_per_topic = 0;
    std::size_t max_bytes_per_topic = 0;
    std::size_t max_total_bytes = 0;
    std::optional<std::chrono::steady_clock::duration> max_age;
};

class Snapshot {
public:
    explicit Snapshot(SnapshotConfig config);

    void push_written_live(const RecordedMessage& message, std::chrono::steady_clock::time_point now);
    std::vector<RecordedMessage> replay_for(std::uint64_t generation) const;
    void erase_topic(TopicId topic_id, std::uint64_t topic_epoch);
    std::size_t total_bytes() const noexcept { return total_bytes_; }

private:
    struct Entry {
        std::uint64_t sequence = 0;
        RecordedMessage message;
        std::chrono::steady_clock::time_point inserted_at;
    };

    using TopicEntries = std::deque<Entry>;

    void enforce_topic(TopicEntries& entries, std::chrono::steady_clock::time_point now);
    void enforce_total();
    void erase_entry(TopicEntries& entries, TopicEntries::iterator entry);

    SnapshotConfig config_;
    std::uint64_t next_sequence_ = 0;
    std::size_t total_bytes_ = 0;
    std::unordered_map<TopicId, TopicEntries> entries_;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_SNAPSHOT_HPP_
