// Writer-thread bounded repeat-on-segment payload ownership and replay buffer.
#ifndef ROS2_BAG_SNAPSHOT_BUFFER_HPP_
#define ROS2_BAG_SNAPSHOT_BUFFER_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <utility>
#include <vector>

#include "ros2_bag/recorded_message.hpp"
#include "ros2_bag/topic_manager.hpp"

namespace ros2_bag {

// Retains the latest successfully written messages selected by RepeatPolicy.
// Every method is restricted to the single Writer thread. Entries retain the
// original SerializedMessage owner and never make a full CDR payload copy.
class SnapshotBuffer {
public:
    explicit SnapshotBuffer(std::size_t max_total_bytes);

    SnapshotBuffer(const SnapshotBuffer&) = delete;
    SnapshotBuffer& operator=(const SnapshotBuffer&) = delete;

    // Adds a successfully written message if its Topic policy permits it.
    // Newer data replaces old data first for that Topic, then globally evicts
    // the oldest entries until the configured payload-byte budget is met.
    void add(const RecordedMessage& message);

    // Removes all cached messages for one removed Topic instance.
    void remove_topic(TopicId topic_id, std::uint64_t topic_epoch);

    // Discards entries whose TopicId/Epoch is not active in the new segment.
    void retain_active_topics(const std::vector<TopicSnapshot>& topics);

    // Calls write once for each retained entry in arrival order. Replayed
    // messages set is_replayed=true; write returns false to stop replay.
    bool replay(const std::function<bool(const RecordedMessage&)>& write) const;

    void clear() noexcept;
    std::size_t used_bytes() const noexcept;
    std::size_t message_count() const noexcept;

private:
    using TopicKey = std::pair<TopicId, std::uint64_t>;

    struct Entry {
        RecordedMessage message;
        TopicKey key;
    };

    using Entries = std::list<Entry>;

    struct TopicUsage {
        std::size_t used_bytes = 0;
        std::deque<Entries::iterator> entries;
    };

    void erase(Entries::iterator entry) noexcept;

    const std::size_t max_total_bytes_;
    std::size_t used_bytes_ = 0;
    Entries entries_;
    std::map<TopicKey, TopicUsage> topic_usage_;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_SNAPSHOT_BUFFER_HPP_
