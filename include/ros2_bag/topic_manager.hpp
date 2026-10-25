// Dynamic GenericSubscription lifecycle and task-scoped recording ingress.
#ifndef ROS2_BAG_TOPIC_MANAGER_HPP_
#define ROS2_BAG_TOPIC_MANAGER_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "ros2_bag/recording_queue.hpp"
#include "ros2_bag/topic_info.hpp"

namespace ros2_bag {

enum class TopicRuntimeState {
    kAbsent,
    kAdding,
    kArmed,
    kActive,
    kRemoving,
    kRemoved,
    kFailed,
};

struct TopicCounters {
    std::uint64_t received = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t dropped_capacity = 0;
    std::uint64_t dropped_enqueue_failure = 0;
    std::uint64_t dropped_allocation_failure = 0;
    std::uint64_t dropped_paused = 0;
    std::uint64_t dropped_stale_generation = 0;
    std::uint64_t written = 0;
    std::uint64_t write_errors = 0;
};

struct TopicSnapshot {
    TopicInfo topic_info;
    std::uint64_t topic_epoch = 0;
    TopicRuntimeState state = TopicRuntimeState::kAbsent;
    std::size_t matched_publishers = 0;
    std::optional<double> actual_hz;
    std::optional<bool> gap;
    TopicCounters counters;
    std::string last_operation_error;
};

struct RemovedTopicSnapshot {
    TopicInfo topic_info;
    std::uint64_t topic_epoch = 0;
    std::uint64_t removed_at_ns = 0;
    std::string final_state;
};

struct TopicEvent {
    std::string event;
    std::string operation_id;
    TopicId topic_id = kInvalidTopicId;
    std::uint64_t topic_epoch = 0;
    std::string name;
    std::string type;
    std::string error_code;
    std::string error_message;
    // Serialized JSON object with diagnostic context for this durable event.
    std::string details_json = "{}";
};

struct TaskIngress {
    std::uint64_t recording_generation = 0;
    std::shared_ptr<MessageQueue> queue;
    std::shared_ptr<QueueBudget> budget;
    std::shared_ptr<PoolState> pool_state;
    std::shared_ptr<std::atomic<bool>> open;
};

struct TopicOperationResult {
    bool ok = false;
    TopicId topic_id = kInvalidTopicId;
    std::uint64_t topic_epoch = 0;
    std::string error_code;
    std::string error_message;
};

// Owns task subscriptions. Public operations are serialized internally; callbacks
// do not wait under manager_mutex_. Writer registration and event persistence are
// supplied by RecordingCoordinator and must be synchronous at their boundaries.
class TopicManager {
public:
    using RegisterTopic = std::function<bool(const TopicInfo&, std::string*)>;
    using PersistEvent = std::function<bool(const TopicEvent&, std::string*)>;

    TopicManager(
        rclcpp::Node::SharedPtr node,
        std::size_t max_active_topics,
        std::size_t max_distinct_topics,
        std::size_t recent_removed_instances,
        std::chrono::milliseconds producer_drain_timeout,
        double frequency_window_sec,
        std::size_t frequency_bucket_ms,
        RegisterTopic register_topic,
        PersistEvent persist_event);

    TopicManager(const TopicManager&) = delete;
    TopicManager& operator=(const TopicManager&) = delete;

    // Installs the current task ingress before topics can be armed or activated.
    // The caller must have removed all previous task subscriptions.
    void set_task_ingress(TaskIngress ingress);

    // Creates one Writer channel then a mutually exclusive GenericSubscription.
    // In paused mode it creates ARMED state without opening callback ingress.
    TopicOperationResult
    add_topic(TopicInfo topic_info, const std::string& operation_id, bool armed);

    // Closes ingress, drains current callback work, removes a subscription, then
    // records a durable terminal event. Existing accepted queue messages remain.
    TopicOperationResult remove_topic(const std::string& name, const std::string& operation_id);

    // Opens or closes every active callback ingress without touching subscriptions.
    void set_ingress_open(bool open);

    // Closes ingress and waits for active callback producers. It does not remove
    // subscriptions or drain the Writer queue.
    bool close_ingress_and_drain();

    // Removes all subscriptions after ingress is closed and producers have drained.
    void clear_task();

    // Performs clear_task() from the Node Executor callback that owns the
    // subscriptions. It must not be called by external control threads.
    void clear_task_on_executor();

    std::vector<TopicSnapshot> snapshots() const;
    std::vector<RemovedTopicSnapshot> removed_snapshots() const;
    std::uint64_t removed_total() const noexcept;

    // Records a Writer-side generation violation against the matching current
    // topic instance. It is safe to call from the Writer thread.
    void record_generation_violation(TopicId topic_id, std::uint64_t topic_epoch);

private:
    struct TopicRuntime;
    struct ExecutorCall;

    static rclcpp::QoS make_qos(const TopicQos& qos);
    TopicQos adapt_qos(const TopicInfo& topic_info) const;
    void process_message(
        const std::shared_ptr<TopicRuntime>& runtime,
        std::shared_ptr<rclcpp::SerializedMessage> serialized);
    bool persist(const TopicEvent& event, std::string* error);
    TopicOperationResult
    add_topic_on_executor(TopicInfo topic_info, const std::string& operation_id, bool armed);
    TopicOperationResult remove_topic_on_executor(
        const std::string& name,
        const std::string& operation_id);
    void record_arrival(const std::shared_ptr<TopicRuntime>& runtime);
    void update_frequency_snapshot(
        const std::shared_ptr<TopicRuntime>& runtime,
        TopicSnapshot* snapshot) const;

    rclcpp::Node::SharedPtr node_;
    const std::size_t max_active_topics_;
    const std::size_t max_distinct_topics_;
    const std::size_t recent_removed_instances_;
    const std::chrono::milliseconds producer_drain_timeout_;
    const double frequency_window_sec_;
    const std::size_t frequency_bucket_ms_;
    const std::size_t frequency_window_slots_;
    RegisterTopic register_topic_;
    PersistEvent persist_event_;

    mutable std::mutex manager_mutex_;
    TaskIngress ingress_;
    TopicId next_topic_id_ = 1;
    std::size_t distinct_topics_ = 0;
    std::map<std::string, TopicId> historical_ids_;
    std::map<std::string, std::uint64_t> historical_epochs_;
    std::map<std::string, std::shared_ptr<TopicRuntime>> topics_;
    std::vector<RemovedTopicSnapshot> removed_topics_;
    std::uint64_t removed_total_ = 0;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_TOPIC_MANAGER_HPP_
