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

    bool paused() const noexcept { return user_paused || disk != DiskPauseReason::None; }
};

struct QueueState {
    std::size_t used_messages = 0;
    std::size_t max_messages = 0;
    std::size_t used_bytes = 0;
    std::size_t max_bytes = 0;
};

struct PoolState {
    std::size_t capacity = 0;
    std::size_t in_use = 0;
    std::uint64_t allocation_failures = 0;
};

enum class TopicRuntimeState { Adding, Armed, Active, Removing, Removed, Failed };

struct TopicState {
    TopicInfo info;
    TopicRuntimeState state = TopicRuntimeState::Adding;
    std::size_t publisher_count = 0;
    std::optional<bool> qos_compatible;
    bool publisher_visibility_fresh = false;
    std::optional<double> actual_hz;
    std::optional<bool> gap;
    std::uint64_t received_messages = 0;
    std::uint64_t written_messages = 0;
    std::uint64_t dropped_messages = 0;
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
    std::size_t queued_count = 0;
    std::size_t pending_total = 0;
    std::optional<std::uint64_t> active_segment_id;
    std::optional<std::chrono::milliseconds> active_for;
    bool warning_active = false;
    std::uint64_t queue_full_count = 0;
    std::uint64_t total_removed = 0;
    std::optional<Error> last_error;
};

struct DropCounters {
    std::uint64_t stale_identity = 0;
    std::uint64_t not_accepting = 0;
    std::uint64_t timed_deadline = 0;
    std::uint64_t disk_unsafe = 0;
    std::uint64_t disk_snapshot_busy = 0;
    std::uint64_t queue_full = 0;
    std::uint64_t pool_exhausted = 0;
    std::uint64_t enqueue_failure = 0;
};

struct DiskSample {
    DiskSafetyState state = DiskSafetyState::Unknown;
    std::int64_t sampled_at_ns = 0;
    bool valid = false;
    std::uint64_t available_bytes = 0;
    double used_percent = 0.0;
    std::uint32_t consecutive_failures = 0;
};

struct ProcessSample {
    std::int64_t sampled_at_ns = 0;
    bool valid = false;
    std::optional<double> cpu_percent_single_core;
    std::optional<std::uint64_t> rss_bytes;
    std::optional<std::uint32_t> thread_count;
    std::optional<std::uint32_t> fd_count;
};

struct State {
    RecorderState recorder_state = RecorderState::Idle;
    bool shutting_down = false;
    std::optional<std::string> recording_id;
    std::optional<std::uint64_t> generation;
    std::optional<TaskConfig> task_config;
    PauseState pause;
    QueueState queue;
    PoolState pool;
    DropCounters dropped;
    std::optional<DiskSample> disk;
    std::optional<ProcessSample> process;
    CleanupState cleanup;
    std::vector<TopicState> topics;
    std::vector<TopicInfo> topics_removed_recent;
    std::uint64_t topics_removed_total = 0;
    std::optional<SegmentState> current_segment;
    std::optional<SegmentState> last_completed_segment;
    std::optional<std::chrono::milliseconds> rotation_lag;
    std::optional<Error> last_error;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_STATE_HPP_
