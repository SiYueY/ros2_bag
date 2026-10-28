#ifndef ROS2_BAG_RECORDER_TASK_HPP_
#define ROS2_BAG_RECORDER_TASK_HPP_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ros2_bag/recorder/error.hpp"

namespace ros2_bag::recorder {

enum class RecorderState { Idle, Initializing, Recording, Paused, Stopping, Failed };
enum class TaskState { Initializing, Recording, Finalizing, Completed, Failed, Incomplete };
enum class TaskMode { Normal, Timed, Rolling };
enum class CompressionMode { None, Lz4 };
enum class TimestampPolicy { Receive };
enum class SegmentLifecycleState { Writing, Finalizing, Complete, Deleting, Removed, Incomplete };

struct StartOptions {
    std::string recording_id;
    TaskMode mode = TaskMode::Normal;
    std::optional<std::chrono::milliseconds> duration;
    std::optional<std::chrono::seconds> segment_duration;
    std::optional<std::chrono::seconds> retention_duration;
};

struct TaskConfig {
    TaskMode mode = TaskMode::Normal;
    std::optional<std::chrono::milliseconds> duration;
    std::optional<std::chrono::seconds> segment_duration;
    std::optional<std::chrono::seconds> retention_duration;
    CompressionMode compression = CompressionMode::Lz4;
    TimestampPolicy timestamp_policy = TimestampPolicy::Receive;
};

struct SegmentHistorySummary {
    std::uint64_t compacted_removed_count = 0;
    std::uint64_t compacted_removed_bytes = 0;
};

struct SegmentInfo {
    std::uint64_t segment_id = 0;
    SegmentLifecycleState state = SegmentLifecycleState::Writing;
    std::string relative_path;
    std::optional<std::int64_t> started_at_ns;
    std::optional<std::int64_t> completed_at_ns;
    std::optional<std::uint64_t> message_count;
    std::optional<std::uint64_t> size_bytes;
};

struct TaskInfo {
    std::string recording_id;
    std::uint64_t generation = 0;
    TaskState state = TaskState::Initializing;
    TaskConfig config;
    std::optional<std::int64_t> start_time_ns;
    std::optional<std::int64_t> terminal_time_ns;
    SegmentHistorySummary segment_history_summary;
    std::optional<Error> error;
};

struct TaskDetail {
    TaskInfo task;
    std::vector<SegmentInfo> segments;
    std::size_t segment_offset = 0;
    std::size_t detail_segment_count = 0;
    bool segment_has_more = false;
    std::uint64_t total_segment_count = 0;
    bool recovered = false;
    bool recovery_findings_available = false;
    std::uint32_t incomplete_segment_count = 0;
    std::uint32_t uncertain_topic_operation_count = 0;
};

struct TaskList {
    std::vector<TaskInfo> items;
    std::size_t scanned_entries = 0;
    bool scan_truncated = false;
    bool has_more = false;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_TASK_HPP_
