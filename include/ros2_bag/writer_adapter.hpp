// Single-threaded rosbag2/MCAP adapter for formal RecordedMessage ownership.
#ifndef ROS2_BAG_WRITER_ADAPTER_HPP_
#define ROS2_BAG_WRITER_ADAPTER_HPP_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rosbag2_cpp/writer.hpp>

#include "ros2_bag/recorded_message.hpp"

namespace ros2_bag {

struct WriterOpenOptions {
    std::string uri;
    std::string storage_id = "mcap";
    std::string storage_config_uri;
};

struct WriterTopicStatistics {
    TopicId topic_id = kInvalidTopicId;
    std::uint64_t topic_epoch = 0;
    std::uint64_t written = 0;
    std::uint64_t write_errors = 0;
};

// Owns a rosbag2 Writer for exactly one Writer thread. No method is thread-safe;
// RecordingCoordinator and TopicManager must send requests through that thread.
class WriterAdapter {
public:
    // write_override is a deterministic test seam. Production construction
    // leaves it empty and always calls rosbag2_cpp::Writer synchronously.
    using WriteOverride = std::function<bool(const RecordedMessage&, std::string*)>;

    explicit WriterAdapter(WriteOverride write_override = WriteOverride());
    WriterAdapter(const WriterAdapter&) = delete;
    WriterAdapter& operator=(const WriterAdapter&) = delete;

    // Opens one independently readable segment. Returns false with error text on
    // a recoverable rosbag2 failure. Calling while open is an error.
    bool open_segment(const WriterOpenOptions& options, std::string* error) noexcept;

    // Registers TopicMetadata and, on Jazzy, the complete MessageDefinition.
    // Registration is idempotent for the same topic name within one segment.
    bool register_topic(const TopicInfo& topic_info, std::string* error) noexcept;

    // Borrows CDR bytes from message.serialized_message during the synchronous
    // rosbag2 call. It does not create a Recorder-side full-payload copy.
    bool write(const RecordedMessage& message, std::string* error) noexcept;

    // Closes an opened segment. It is safe to call when already closed.
    bool close(std::string* error) noexcept;

    bool is_open() const noexcept;

    // Clears task-local write counters before opening a new recording task.
    // RecordingCoordinator calls it only on the Writer thread.
    void reset_statistics() noexcept;

    // Returns a consistent copy of Writer-thread counters. This method is
    // thread-safe and intentionally does not expose rosbag2 Writer state.
    std::vector<WriterTopicStatistics> statistics() const noexcept;

private:
    bool set_error(const std::string& message, std::string* error) noexcept;

    rosbag2_cpp::Writer writer_;
    const WriteOverride write_override_;
    bool open_ = false;
    std::set<std::string> registered_topics_;
    mutable std::mutex statistics_mutex_;
    std::map<std::pair<TopicId, std::uint64_t>, WriterTopicStatistics> statistics_;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_WRITER_ADAPTER_HPP_
