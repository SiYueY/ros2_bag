#ifndef ROS2_BAG_RECORDER_STATE_HPP_
#define ROS2_BAG_RECORDER_STATE_HPP_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ros2_bag/recorder/error.hpp"
#include "ros2_bag/recorder/task.hpp"
#include "ros2_bag/recorder/topic.hpp"

namespace ros2_bag::recorder {

enum class DiskPauseReason { None, Critical, Unknown, RecoveryPending };
enum class DiskSafetyState { Normal, Warning, Critical, Unknown };

struct PauseState {
    bool user_paused = false;
    DiskPauseReason disk = DiskPauseReason::None;

    bool paused() const noexcept;
};

struct QueueState {
    struct Messages {
        std::size_t used = 0;
        std::size_t max = 0;
    };

    struct Bytes {
        std::size_t used = 0;
        std::size_t max = 0;
    };

    Messages messages;
    Bytes bytes;
};

struct PoolState {
    std::size_t capacity = 0;
    std::size_t in_use = 0;
    std::uint64_t allocation_failures = 0;
};

enum class TopicRuntimeState { Adding, Armed, Active, Removing, Removed, Failed };

struct TopicState {
    enum class Compatibility { Unknown, Compatible, Incompatible };
    enum class Gap { Unknown, Normal, Detected };

    struct Publishers {
        std::size_t count = 0;
        Compatibility compatibility = Compatibility::Unknown;
        bool fresh = false;
    };

    struct Frequency {
        double actual_hz = 0.0;
        Gap gap = Gap::Unknown;
    };

    struct Messages {
        std::uint64_t received = 0;
        std::uint64_t written = 0;
        std::uint64_t dropped = 0;
    };

    TopicInfo info;
    TopicRuntimeState state = TopicRuntimeState::Adding;
    Publishers publishers;
    Frequency frequency;
    Messages messages;
};

struct SegmentState {
    std::uint64_t segment_id = 0;
    SegmentLifecycleState state = SegmentLifecycleState::Writing;
    std::uint64_t written_messages = 0;
    std::uint64_t written_bytes = 0;
    bool has_live_messages = false;
    bool has_replayed_messages = false;
};

struct CleanupState {
    struct Active {
        std::uint64_t segment_id = 0;
        std::chrono::milliseconds elapsed{0};
    };

    std::size_t queued = 0;
    std::size_t pending = 0;
    std::optional<Active> active;
    bool warning = false;
    std::uint64_t queue_full = 0;
    std::uint64_t removed = 0;
    std::optional<Error> last_error;
};

struct DropCounters {
    struct Admission {
        std::uint64_t stale_identity = 0;
        std::uint64_t not_accepting = 0;
        std::uint64_t timed_deadline = 0;
        std::uint64_t disk_unsafe = 0;
        std::uint64_t disk_snapshot_busy = 0;
    };

    struct Queue {
        std::uint64_t full = 0;
    };

    struct Pool {
        std::uint64_t exhausted = 0;
    };

    struct Writer {
        std::uint64_t write_failure = 0;
    };

    Admission admission;
    Queue queue;
    Pool pool;
    Writer writer;
};

struct DiskSample {
    DiskSafetyState state = DiskSafetyState::Unknown;
    std::int64_t sampled_at_ns = 0;
    std::uint64_t available_bytes = 0;
    double used_percent = 0.0;
    std::uint32_t consecutive_failures = 0;
};

struct ProcessSample {
    struct Cpu {
        double percent = 0.0;
    };

    struct Memory {
        std::uint64_t rss_bytes = 0;
    };

    struct Threads {
        std::uint32_t count = 0;
    };

    struct FileDescriptors {
        std::uint32_t count = 0;
    };

    std::int64_t sampled_at_ns = 0;
    Cpu cpu;
    Memory memory;
    Threads threads;
    FileDescriptors file_descriptors;
};

struct State {
    struct Task {
        std::string recording_id;
        std::uint64_t generation = 0;
        TaskConfig config;
    };

    struct Segments {
        std::optional<SegmentState> current;
        std::optional<SegmentState> last_completed;
        std::chrono::milliseconds rotation_lag{0};
    };

    RecorderState recorder_state = RecorderState::Idle;
    bool shutting_down = false;
    std::optional<Task> task;
    PauseState pause;
    QueueState queue;
    PoolState pool;
    DropCounters dropped;
    DiskSample disk;
    ProcessSample process;
    CleanupState cleanup;
    std::vector<TopicState> topics;
    std::vector<TopicInfo> topics_removed_recent;
    std::uint64_t topics_removed_total = 0;
    Segments segments;
    std::optional<Error> last_error;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_STATE_HPP_
