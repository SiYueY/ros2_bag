#ifndef ROS2_BAG_RECORDER_TASK_HPP_
#define ROS2_BAG_RECORDER_TASK_HPP_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
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
    struct Normal {};

    struct Timed {
        std::chrono::milliseconds duration{0};
    };

    struct Rolling {
        std::optional<std::chrono::seconds> segment_duration;
        std::optional<std::chrono::seconds> retention_duration;
    };

    using Mode = std::variant<Normal, Timed, Rolling>;

    std::string recording_id;
    Mode mode = Normal{};
};

struct TaskConfig {
    struct Timed {
        std::chrono::milliseconds duration{0};
    };

    struct Rolling {
        std::chrono::seconds segment_duration{0};
        std::chrono::seconds retention_duration{0};
    };

    TaskMode mode = TaskMode::Normal;
    Timed timed;
    Rolling rolling;
    CompressionMode compression = CompressionMode::Lz4;
    TimestampPolicy timestamp_policy = TimestampPolicy::Receive;
};

struct SegmentHistorySummary {
    std::uint64_t removed_count = 0;
    std::uint64_t removed_bytes = 0;
};

struct SegmentInfo {
    std::uint64_t segment_id = 0;
    SegmentLifecycleState state = SegmentLifecycleState::Writing;
    std::string relative_path;
    std::int64_t started_at_ns = 0;
    std::int64_t completed_at_ns = 0;
    std::optional<std::uint64_t> message_count;
    std::optional<std::uint64_t> size_bytes;
};

struct TaskInfo {
    std::string recording_id;
    std::uint64_t generation = 0;
    TaskState state = TaskState::Initializing;
    TaskConfig config;
    std::int64_t start_time_ns = 0;
    std::int64_t terminal_time_ns = 0;
    SegmentHistorySummary segment_history;
    std::optional<Error> error;
};

struct TaskDetail {
    struct Segments {
        std::vector<SegmentInfo> items;
        std::size_t offset = 0;
        std::size_t available = 0;
        std::uint64_t total = 0;
        bool has_more = false;
    };

    struct Recovery {
        bool recovered = false;
        bool has_findings = false;
        bool manifest_revision_exhausted = false;
        std::uint32_t incomplete_segments = 0;
        std::uint32_t uncertain_operations = 0;
    };

    TaskInfo task;
    Segments segments;
    Recovery recovery;
};

struct TaskList {
    std::vector<TaskInfo> items;
    std::size_t scanned_entries = 0;
    bool scan_truncated = false;
    bool has_more = false;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_TASK_HPP_
