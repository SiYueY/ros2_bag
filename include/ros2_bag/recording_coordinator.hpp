// Task lifecycle coordinator: serializes control and owns the Writer thread.
#ifndef ROS2_BAG_RECORDING_COORDINATOR_HPP_
#define ROS2_BAG_RECORDING_COORDINATOR_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "ros2_bag/config.hpp"
#include "ros2_bag/cleanup_worker.hpp"
#include "ros2_bag/manifest.hpp"
#include "ros2_bag/resource_monitor.hpp"
#include "ros2_bag/snapshot_buffer.hpp"
#include "ros2_bag/topic_manager.hpp"
#include "ros2_bag/writer_adapter.hpp"

namespace ros2_bag {

enum class RecorderState { kIdle, kRecording, kPaused, kStopping, kFailed };

struct RecorderStatus {
    RecorderState state = RecorderState::kIdle;
    std::uint64_t recording_generation = 0;
    std::filesystem::path task_directory;
    std::string recording_id;
    RecordingMode mode = RecordingMode::kNormal;
    std::string last_error;
    std::vector<std::string> secondary_errors;
    QueueBudgetStatistics queue;
    PoolStatistics pool;
    DiskSample disk;
    ProcessSample process;
    bool paused_by_disk = false;
    std::string paused_reason;
    std::optional<std::uint64_t> uptime_ns;
    std::optional<double> elapsed_wall_sec;
    std::optional<double> elapsed_active_sec;
    std::optional<double> paused_total_sec;
    std::vector<TopicSnapshot> topics;
    std::vector<RemovedTopicSnapshot> topics_removed;
    std::uint64_t topics_removed_total = 0;
};

// Coordinates one Recorder process. Public methods are thread-safe and block
// only for bounded Writer-control work. Callbacks never call WriterAdapter.
class RecordingCoordinator {
public:
    RecordingCoordinator(
        rclcpp::Node::SharedPtr node,
        RecorderConfig config,
        CleanupWorker::RemoveDirectory remove_directory = CleanupWorker::RemoveDirectory(),
        std::function<DiskSample()> disk_sampler = std::function<DiskSample()>(),
        WriterAdapter::WriteOverride write_override = WriterAdapter::WriteOverride(),
        TopicManager::PersistEvent persist_event_override = TopicManager::PersistEvent());
    ~RecordingCoordinator();
    RecordingCoordinator(const RecordingCoordinator&) = delete;
    RecordingCoordinator& operator=(const RecordingCoordinator&) = delete;

    bool start(const std::string& task_id, std::string* error);
    // Starts a timed task. timed_duration uses ROS steady wall time and includes
    // paused intervals. It is valid only when recording.mode is kTimed.
    bool start_timed(
        const std::string& task_id,
        std::chrono::milliseconds timed_duration,
        std::string* error);
    bool stop(std::string* error);
    bool pause(std::string* error);
    bool resume(std::string* error);
    TopicOperationResult add_topic(TopicInfo topic, const std::string& operation_id);
    // Resolves one uniquely typed topic from the current ROS graph and adds it
    // to the active task. It returns NOT_FOUND or INVALID_ARGUMENT when graph
    // discovery cannot provide exactly one type. Thread-safe.
    TopicOperationResult add_topic_by_name(
        const std::string& name,
        const std::string& operation_id);
    TopicOperationResult remove_topic(const std::string& name, const std::string& operation_id);
    RecorderStatus status() const;
    // Records a bounded non-fatal runtime diagnostic. Thread-safe and safe
    // while no recording task is active.
    void record_runtime_diagnostic(const std::string& diagnostic);
    void shutdown();

private:
    struct WriterRequest {
        std::function<bool(std::string*)> operation;
        std::string error;
        bool done = false;
        bool result = false;
        bool drain_data_before_operation = false;
        std::condition_variable completion;
    };
    bool call_writer(
        const std::function<bool(std::string*)>& operation,
        std::string* error,
        bool drain_data_before_operation = false);
    bool start_impl(const std::string& task_id, std::string* error);
    bool stop_impl(std::string* error, bool called_from_executor);
    bool stop_on_executor(std::string* error);
    bool persist_event(const TopicEvent& event, std::string* error);
    void writer_main();
    void fail_recording_from_writer(const std::string& error);
    bool write_message(const RecordedMessage& message, std::string* error);
    bool drain_writer_queue(std::string* error);
    void fail_after_event_persistence_error(const std::string& error);
    bool rotate_segment(std::string* error);
    void sample_disk();
    bool write_current_manifest(
        const std::string& recording_state,
        const std::string& last_error,
        std::string* error);
    void schedule_retention_cleanup();
    void record_cleanup_timeout();
    void
    complete_cleanup(const std::filesystem::path& path, bool success, const std::string& error);
    void set_error_locked(const std::string& error);
    void record_secondary_error_locked(const std::string& error);

    rclcpp::Node::SharedPtr node_;
    RecorderConfig config_;
    mutable std::mutex mutex_;
    RecorderState state_ = RecorderState::kIdle;
    std::uint64_t generation_ = 0;
    std::uint64_t event_sequence_ = 0;
    std::string recording_id_;
    std::filesystem::path task_directory_;
    std::string last_error_;
    std::vector<std::string> secondary_errors_;
    std::shared_ptr<std::atomic<bool>> ingress_open_;
    std::shared_ptr<QueueBudget> budget_;
    std::shared_ptr<MessageQueue> queue_;
    std::shared_ptr<PoolState> pool_state_;
    std::shared_ptr<TopicManager> topic_manager_;
    rclcpp::TimerBase::SharedPtr timed_stop_timer_;
    rclcpp::TimerBase::SharedPtr rolling_timer_;
    rclcpp::TimerBase::SharedPtr disk_timer_;
    std::vector<ManifestSegment> segments_;
    std::vector<std::string> cleanup_pending_;
    std::uint64_t current_segment_id_ = 0;
    std::unique_ptr<CleanupWorker> cleanup_worker_;
    std::shared_ptr<ResourceMonitor> resource_monitor_;
    const std::function<DiskSample()> disk_sampler_;
    const TopicManager::PersistEvent persist_event_override_;
    DiskSample disk_sample_;
    bool paused_by_disk_ = false;
    bool paused_by_user_ = false;
    std::chrono::steady_clock::time_point process_started_at_;
    std::optional<std::chrono::steady_clock::time_point> task_started_at_;
    std::optional<std::chrono::steady_clock::time_point> pause_started_at_;
    std::chrono::steady_clock::duration paused_total_{};
    std::chrono::steady_clock::time_point disk_safe_since_{};
    mutable std::mutex writer_mutex_;
    std::condition_variable writer_cv_;
    std::deque<std::shared_ptr<WriterRequest>> writer_requests_;
    std::thread writer_thread_;
    bool writer_shutdown_ = false;
    bool writer_failed_ = false;
    WriterAdapter writer_;
    SnapshotBuffer snapshot_buffer_;
    std::filesystem::path event_log_;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_RECORDING_COORDINATOR_HPP_
