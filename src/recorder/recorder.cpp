#include "ros2_bag/recorder/recorder.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <sstream>

#include <rclcpp/qos.hpp>
#include <rclcpp/generic_subscription.hpp>

#include "ros2_bag/recorder/manifest.hpp"
#include "ros2_bag/recorder/admission.hpp"
#include "ros2_bag/recorder/cleanup.hpp"
#include "ros2_bag/recorder/message_pool.hpp"
#include "ros2_bag/recorder/message_queue.hpp"
#include "ros2_bag/recorder/resource_monitor.hpp"
#include "ros2_bag/recorder/schema.hpp"
#include "ros2_bag/recorder/snapshot.hpp"
#include "ros2_bag/recorder/writer.hpp"

namespace ros2_bag::recorder {
namespace {

Error error(ErrorCode code, std::string message, Effects effects = Effects::None) {
    return {code, effects, std::move(message)};
}

std::int64_t wall_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

rclcpp::QoS make_qos(const QosOptions& options) {
    rclcpp::QoS qos = *options.history == History::KeepLast
        ? rclcpp::QoS(rclcpp::KeepLast(*options.depth))
        : rclcpp::QoS(rclcpp::KeepAll());
    if (*options.reliability == Reliability::Reliable) qos.reliable();
    else qos.best_effort();
    if (*options.durability == Durability::TransientLocal) qos.transient_local();
    else qos.durability_volatile();
    return qos;
}

template <class T>
Result<T, Error> completed_result(const Completion<T>& completion, std::chrono::milliseconds timeout) {
    if (!completion.wait_for(timeout)) {
        return Result<T, Error>(unexpect, error(ErrorCode::StorageError, "writer operation timed out", Effects::Possible));
    }
    const auto result = completion.result();
    if (!result) return Result<T, Error>(unexpect, error(ErrorCode::InternalError, "writer completion disappeared"));
    return *result;
}

bool valid_recording_id(std::string_view recording_id) {
    if (recording_id.empty() || recording_id.size() > 64) return false;
    const auto initial = static_cast<unsigned char>(recording_id.front());
    if (!std::isalnum(initial)) return false;
    return std::all_of(recording_id.begin() + 1, recording_id.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
    });
}

template <class T>
Completion<T> failed_completion(Error value) {
    CompletionSource<T> source;
    source.set_error(std::move(value));
    return source.completion();
}

}  // namespace

class Recorder::Implementation {
public:
    struct Ingress {
        std::mutex mutex;
        std::condition_variable condition;
        std::shared_ptr<MessageQueue> queue;
        bool open = false;
        std::size_t producers = 0;
    };

    struct ActiveTopic {
        TopicInfo info;
        std::shared_ptr<rclcpp::GenericSubscription> subscription;
        std::shared_ptr<Ingress> ingress;
        std::shared_ptr<AdmissionGate> admission;
        std::shared_ptr<const TopicInfo> frozen_info;
    };

    Implementation(RecorderConfig config, rclcpp::Node::SharedPtr node)
    : config_(std::move(config)), node_(std::move(node)), shutdown_completion_(shutdown_source_.completion()) {
        const auto validated = validate_config(config_);
        if (!validated) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = validated.error();
            poisoned_ = true;
        }
        worker_ = std::thread([this] { run(); });
    }

    ~Implementation() {
        request_shutdown();
        if (worker_.joinable()) worker_.join();
    }

    template <class Function>
    bool submit(Function&& function) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= config_.operation_queue_capacity || stopping_worker_) return false;
        queue_.emplace_back(std::forward<Function>(function));
        queue_condition_.notify_one();
        return true;
    }

    void run() {
        while (true) {
            std::function<void()> operation;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                const auto timed_deadline = timed_deadline_steady_ns_.load(std::memory_order_acquire);
                const auto rolling_deadline = rolling_deadline_steady_ns_.load(std::memory_order_acquire);
                const auto retention_deadline = retention_deadline_steady_ns_.load(std::memory_order_acquire);
                auto deadline_ns = timed_deadline;
                if (deadline_ns == 0 || (rolling_deadline != 0 && rolling_deadline < deadline_ns)) {
                    deadline_ns = rolling_deadline;
                }
                if (deadline_ns == 0 || (retention_deadline != 0 && retention_deadline < deadline_ns)) {
                    deadline_ns = retention_deadline;
                }
                if (deadline_ns == 0) {
                    queue_condition_.wait(lock, [this] { return stopping_worker_ || !queue_.empty(); });
                } else {
                    const auto deadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline_ns));
                    if (!queue_condition_.wait_until(lock, deadline, [this] { return stopping_worker_ || !queue_.empty(); })) {
                        lock.unlock();
                        if (timed_deadline != 0 && deadline_ns == timed_deadline) {
                            stop_timed_task();
                        } else if (rolling_deadline != 0 && deadline_ns == rolling_deadline) {
                            rotate_rolling_segment();
                        } else {
                            run_retention();
                        }
                        continue;
                    }
                }
                if (queue_.empty() && stopping_worker_) return;
                operation = std::move(queue_.front());
                queue_.pop_front();
            }
            operation();
        }
    }

    void schedule_retention_locked() {
        retention_deadline_steady_ns_.store(0, std::memory_order_release);
        if (!manifest_ || !cleanup_ || !state_.task_config || state_.task_config->mode != TaskMode::Rolling ||
            !state_.task_config->retention_duration) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        ManifestData updated = manifest_->data();
        std::vector<DeleteWork> work;
        std::optional<std::chrono::steady_clock::time_point> next_due;
        for (auto& segment : updated.segments) {
            const auto completed = completed_segment_steady_.find(segment.segment_id);
            if (segment.state != SegmentLifecycleState::Complete || completed == completed_segment_steady_.end()) {
                continue;
            }
            const auto due = completed->second + *state_.task_config->retention_duration;
            if (now < due) {
                if (!next_due || due < *next_due) next_due = due;
                continue;
            }
            segment.state = SegmentLifecycleState::Deleting;
            work.push_back(DeleteWork{updated.task.recording_id, segment.segment_id,
                                      manifest_->task_directory() / segment.relative_path,
                                      updated.manifest_revision + 1});
        }
        if (!work.empty() && updated.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = error(ErrorCode::StorageError, "manifest revision exhausted");
            poisoned_ = true;
            return;
        }
        if (!work.empty()) {
            ++updated.manifest_revision;
            const auto committed = manifest_->commit(std::move(updated));
            if (!committed) {
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = committed.error();
                poisoned_ = true;
                return;
            }
        }
        for (auto& item : work) {
            if (!cleanup_->enqueue(std::move(item))) {
                state_.cleanup.queue_full_count++;
                state_.cleanup.last_error = error(ErrorCode::ResourceExhausted, "cleanup queue is full");
            }
        }
        if (next_due) {
            retention_deadline_steady_ns_.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(next_due->time_since_epoch()).count(),
                std::memory_order_release);
            queue_condition_.notify_one();
        }
    }

    void run_retention() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        schedule_retention_locked();
    }

    void rotate_rolling_segment() {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (!manifest_ || state_.recorder_state != RecorderState::Recording || !state_.task_config ||
            state_.task_config->mode != TaskMode::Rolling || !state_.current_segment) {
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        state_.recorder_state = RecorderState::Stopping;
        for (auto& entry : active_topics_) {
            entry.second.admission->close();
            std::unique_lock<std::mutex> ingress_lock(entry.second.ingress->mutex);
            entry.second.ingress->open = false;
            entry.second.ingress->condition.wait(
                ingress_lock, [&entry] { return entry.second.ingress->producers == 0; });
        }
        const auto previous = *state_.current_segment;
        const auto generation = *state_.generation;
        const auto writer = writer_;
        std::vector<TopicInfo> topics;
        topics.reserve(active_topics_.size());
        for (const auto& entry : active_topics_) topics.push_back(entry.second.info);
        lock.unlock();
        stop_dispatch_thread(false);
        const auto closed = completed_result(writer->close(), std::chrono::seconds(30));
        if (!closed) {
            lock.lock();
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = closed.error();
            poisoned_ = true;
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        lock.lock();
        ManifestData complete = manifest_->data();
        if (complete.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = error(ErrorCode::StorageError, "manifest revision exhausted");
            poisoned_ = true;
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        for (auto& segment : complete.segments) {
            if (segment.segment_id == previous.segment_id && segment.state == SegmentLifecycleState::Writing) {
                segment.state = SegmentLifecycleState::Complete;
                segment.completed_at_ns = wall_time_ns();
                segment.message_count = previous.written_messages;
                segment.size_bytes = previous.written_bytes;
                completed_segment_steady_[segment.segment_id] = std::chrono::steady_clock::now();
                break;
            }
        }
        ++complete.manifest_revision;
        const auto committed_complete = manifest_->commit(std::move(complete));
        if (!committed_complete) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = committed_complete.error();
            poisoned_ = true;
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        schedule_retention_locked();
        if (state_.recorder_state == RecorderState::Failed) {
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        const auto next_segment_id = previous.segment_id + 1;
        if (next_segment_id == 0 || manifest_->data().manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = error(ErrorCode::StorageError, "segment or manifest revision exhausted");
            poisoned_ = true;
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        std::ostringstream name;
        name << "segment_" << std::setw(6) << std::setfill('0') << next_segment_id;
        ManifestData opening = manifest_->data();
        SegmentInfo next;
        next.segment_id = next_segment_id;
        next.state = SegmentLifecycleState::Writing;
        next.relative_path = name.str();
        next.started_at_ns = wall_time_ns();
        opening.segments.push_back(next);
        ++opening.manifest_revision;
        const auto committed_opening = manifest_->commit(std::move(opening));
        if (!committed_opening) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = committed_opening.error();
            poisoned_ = true;
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        const auto segment_directory = manifest_->task_directory() / next.relative_path;
        lock.unlock();
        const auto opened = completed_result(writer->open(segment_directory.string()), std::chrono::seconds(30));
        if (!opened) {
            lock.lock();
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = opened.error();
            poisoned_ = true;
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        for (const auto& topic : topics) {
            const auto registered = completed_result(writer->register_topic(topic), std::chrono::seconds(30));
            if (!registered) {
                lock.lock();
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = registered.error();
                poisoned_ = true;
                rolling_deadline_steady_ns_.store(0, std::memory_order_release);
                return;
            }
        }
        std::vector<RecordedMessage> replay;
        {
            std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
            if (snapshot_) replay = snapshot_->replay_for(generation);
        }
        for (auto& message : replay) {
            if (!message.topic) continue;
            const auto replayed = completed_result(writer->write(message, message.topic->name, message.topic->type), std::chrono::seconds(30));
            if (!replayed) {
                lock.lock();
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = replayed.error();
                poisoned_ = true;
                rolling_deadline_steady_ns_.store(0, std::memory_order_release);
                return;
            }
        }
        lock.lock();
        message_queue_ = std::make_shared<MessageQueue>(config_.message_queue_max_messages, config_.message_queue_max_bytes);
        for (auto& entry : active_topics_) {
            std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
            entry.second.ingress->queue = message_queue_;
        }
        start_dispatch_thread(writer);
        SegmentState current;
        current.segment_id = next_segment_id;
        current.state = SegmentLifecycleState::Writing;
        current.has_replayed_messages = !replay.empty();
        current.written_messages = replay.size();
        for (const auto& message : replay) current.written_bytes += message.payload_bytes;
        state_.current_segment = current;
        state_.last_completed_segment = previous;
        state_.recorder_state = RecorderState::Recording;
        for (auto& entry : active_topics_) {
            {
                std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                entry.second.ingress->open = true;
            }
            entry.second.admission->open(generation, entry.second.info.topic_epoch);
        }
        rolling_deadline_steady_ns_.store(0, std::memory_order_release);
    }

    void stop_timed_task() {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (!manifest_ || (state_.recorder_state != RecorderState::Recording && state_.recorder_state != RecorderState::Paused)) {
            timed_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        state_.recorder_state = RecorderState::Stopping;
        for (auto& entry : active_topics_) {
            entry.second.admission->close();
            std::unique_lock<std::mutex> ingress_lock(entry.second.ingress->mutex);
            entry.second.ingress->open = false;
            entry.second.ingress->condition.wait(
                ingress_lock, [&entry] { return entry.second.ingress->producers == 0; });
        }
        active_topics_.clear();
        state_.topics.clear();
        const auto writer = writer_;
        const bool writer_open = writer_open_;
        lock.unlock();
        resource_monitor_.reset();
        stop_dispatch_thread();
        if (writer_open) {
            const auto closed = completed_result(writer->close(), std::chrono::seconds(30));
            if (!closed) {
                lock.lock();
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = closed.error();
                poisoned_ = true;
                timed_deadline_steady_ns_.store(0, std::memory_order_release);
                return;
            }
        }
        lock.lock();
        writer_open_ = false;
        ManifestData completed = manifest_->data();
        if (completed.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = error(ErrorCode::StorageError, "manifest revision exhausted");
            poisoned_ = true;
            timed_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        ++completed.manifest_revision;
        completed.task.state = TaskState::Completed;
        completed.task.terminal_time_ns = wall_time_ns();
        for (auto& segment : completed.segments) {
            if (segment.state == SegmentLifecycleState::Writing) {
                segment.state = SegmentLifecycleState::Complete;
                segment.completed_at_ns = completed.task.terminal_time_ns;
            }
        }
        const auto committed = manifest_->commit(completed);
        if (!committed) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = committed.error();
            poisoned_ = true;
        } else {
            manifest_.reset();
            topic_events_.reset();
            writer_.reset();
            snapshot_.reset();
            message_queue_.reset();
            message_pool_.reset();
            cleanup_.reset();
            state_ = State{};
        }
        timed_deadline_steady_ns_.store(0, std::memory_order_release);
        rolling_deadline_steady_ns_.store(0, std::memory_order_release);
    }

    void request_shutdown() {
        bool expected = false;
        if (!shutdown_requested_.compare_exchange_strong(expected, true)) return;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_.shutting_down = true;
        }
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.emplace_back([this] {
            std::shared_ptr<Writer> writer;
            bool writer_open = false;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                for (auto& entry : active_topics_) {
                    entry.second.admission->close();
                    std::unique_lock<std::mutex> ingress_lock(entry.second.ingress->mutex);
                    entry.second.ingress->open = false;
                    entry.second.ingress->condition.wait(
                        ingress_lock, [&entry] { return entry.second.ingress->producers == 0; });
                }
                active_topics_.clear();
                state_.topics.clear();
                writer = writer_;
                writer_open = writer_open_;
            }
            resource_monitor_.reset();
            stop_dispatch_thread();
            std::optional<Error> close_error;
            if (writer && writer_open) {
                const auto closed = completed_result(writer->close(), std::chrono::seconds(30));
                if (!closed) close_error = closed.error();
            }
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                if (manifest_) {
                    ManifestData failed = manifest_->data();
                    if (failed.manifest_revision != std::numeric_limits<std::uint64_t>::max()) {
                        ++failed.manifest_revision;
                        failed.task.state = TaskState::Failed;
                        failed.task.terminal_time_ns = wall_time_ns();
                        failed.task.error = close_error ? *close_error :
                            error(ErrorCode::InternalError, "shutdown interrupted active task", Effects::ConfirmedPartial);
                        const auto committed = manifest_->commit(failed);
                        if (!committed) state_.last_error = committed.error();
                    }
                    manifest_.reset();
                }
                topic_events_.reset();
                writer_.reset();
                snapshot_.reset();
                message_queue_.reset();
                message_pool_.reset();
                cleanup_.reset();
                writer_open_ = false;
                state_ = State{};
                state_.shutting_down = true;
            }
            shutdown_source_.set_value();
            std::lock_guard<std::mutex> queue_lock(queue_mutex_);
            stopping_worker_ = true;
            queue_condition_.notify_all();
        });
        queue_condition_.notify_one();
    }

    RecorderConfig config_;
    rclcpp::Node::SharedPtr node_;
    mutable std::mutex state_mutex_;
    State state_;
    std::optional<Manifest> manifest_;
    std::optional<TopicEvents> topic_events_;
    std::shared_ptr<Writer> writer_;
    std::shared_ptr<MessagePool> message_pool_;
    std::shared_ptr<MessageQueue> message_queue_;
    std::unique_ptr<Snapshot> snapshot_;
    std::mutex snapshot_mutex_;
    std::unique_ptr<ResourceMonitor> resource_monitor_;
    std::unique_ptr<Cleanup> cleanup_;
    std::thread dispatch_thread_;
    std::atomic<bool> dispatch_stopping_{false};
    DiskAdmissionState disk_admission_;
    bool writer_open_ = false;
    std::unordered_map<TopicId, ActiveTopic> active_topics_;
    std::unordered_map<std::string, std::uint64_t> topic_epochs_;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> completed_segment_steady_;
    std::uint64_t next_topic_id_ = 1;
    std::atomic<std::uint64_t> pool_exhausted_{0};
    std::atomic<std::uint64_t> queue_full_{0};
    std::atomic<std::uint64_t> writer_enqueue_failure_{0};
    std::optional<std::chrono::steady_clock::time_point> disk_safe_since_;

    void start_dispatch_thread(std::shared_ptr<Writer> writer) {
        dispatch_stopping_.store(false, std::memory_order_release);
        dispatch_thread_ = std::thread([this, writer = std::move(writer)] {
            while (true) {
                const auto message = message_queue_->pop_for(std::chrono::milliseconds(50));
                if (!message) {
                    if (message_queue_->closed()) return;
                    continue;
                }
                if (!message->topic) continue;
                const auto written = writer->write(*message, message->topic->name, message->topic->type);
                if (!written.wait_for(std::chrono::seconds(30))) {
                    writer_enqueue_failure_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const auto result = written.result();
                if (!result || !*result) {
                    writer_enqueue_failure_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (!message->replayed) {
                    std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
                    if (snapshot_) snapshot_->push_written_live(*message, std::chrono::steady_clock::now());
                }
                const auto generation = message->generation;
                const auto topic_id = message->topic_id;
                const auto payload_bytes = message->payload_bytes;
                const bool live = !message->replayed;
                (void)submit([this, generation, topic_id, payload_bytes, live] {
                    std::lock_guard<std::mutex> state_lock(state_mutex_);
                    if (!state_.generation || *state_.generation != generation) return;
                    for (auto& topic : state_.topics) {
                        if (topic.info.topic_id == topic_id) {
                            ++topic.written_messages;
                            break;
                        }
                    }
                    if (!state_.current_segment) return;
                    ++state_.current_segment->written_messages;
                    state_.current_segment->written_bytes += payload_bytes;
                    if (live && !state_.current_segment->has_live_messages) {
                        state_.current_segment->has_live_messages = true;
                        if (state_.task_config && state_.task_config->mode == TaskMode::Rolling) {
                            const auto deadline = std::chrono::steady_clock::now() + *state_.task_config->segment_duration;
                            rolling_deadline_steady_ns_.store(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count(),
                                std::memory_order_release);
                            queue_condition_.notify_one();
                        }
                    }
                });
            }
        });
    }

    void stop_dispatch_thread(bool stop_pool = true) {
        if (message_queue_) message_queue_->close();
        if (dispatch_thread_.joinable()) dispatch_thread_.join();
        dispatch_stopping_.store(true, std::memory_order_release);
        if (stop_pool && message_pool_) message_pool_->begin_stop();
    }
    std::uint64_t next_generation_ = 1;
    bool poisoned_ = false;
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<std::int64_t> timed_deadline_steady_ns_{0};
    std::atomic<std::int64_t> rolling_deadline_steady_ns_{0};
    std::atomic<std::int64_t> retention_deadline_steady_ns_{0};
    CompletionSource<void> shutdown_source_;
    Completion<void> shutdown_completion_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::deque<std::function<void()>> queue_;
    bool stopping_worker_ = false;
    std::thread worker_;
};

Recorder::Recorder(RecorderConfig config) : Recorder(std::move(config), nullptr) {}
Recorder::Recorder(RecorderConfig config, rclcpp::Node::SharedPtr node)
: implementation_(std::make_unique<Implementation>(std::move(config), std::move(node))) {}
Recorder::~Recorder() = default;

Completion<TaskInfo> Recorder::start(const StartOptions& options) {
    if (implementation_->shutdown_requested_.load()) return failed_completion<TaskInfo>(error(ErrorCode::InvalidState, "shutdown requested"));
    CompletionSource<TaskInfo> source;
    const auto completion = source.completion();
    if (!implementation_->submit([implementation = implementation_.get(), options, source] {
            std::lock_guard<std::mutex> lock(implementation->state_mutex_);
            if (implementation->shutdown_requested_.load() || implementation->poisoned_ ||
                implementation->state_.recorder_state != RecorderState::Idle) {
                source.set_error(error(ErrorCode::InvalidState, "recorder is not idle"));
                return;
            }
            implementation->completed_segment_steady_.clear();
            if (!valid_recording_id(options.recording_id)) {
                source.set_error(error(ErrorCode::InvalidArgument, "invalid recording_id"));
                return;
            }
            const auto config = make_task_config(implementation->config_, options);
            if (!config) {
                source.set_error(config.error());
                return;
            }
            const auto directory = implementation->config_.output_root / options.recording_id;
            if (std::filesystem::exists(directory)) {
                source.set_error(error(ErrorCode::RecordingExists, "recording directory already exists"));
                return;
            }
            ManifestData data;
            data.manifest_revision = 1;
            data.task.recording_id = options.recording_id;
            data.task.generation = implementation->next_generation_++;
            data.task.state = TaskState::Initializing;
            data.task.config = config.value();
            const auto initial = Manifest::create(directory, data);
            if (!initial) {
                if (initial.error().effects == Effects::Possible) implementation->poisoned_ = true;
                source.set_error(initial.error());
                return;
            }
            implementation->manifest_ = initial.value();
            const auto events = TopicEvents::open(directory, data.task.recording_id, data.task.generation);
            if (!events) {
                ManifestData failed = data;
                failed.manifest_revision = 2;
                failed.task.state = TaskState::Failed;
                failed.task.terminal_time_ns = wall_time_ns();
                failed.task.error = events.error();
                const auto persisted = implementation->manifest_->commit(std::move(failed));
                implementation->state_.recorder_state = RecorderState::Failed;
                implementation->state_.last_error = persisted ? events.error() : persisted.error();
                implementation->poisoned_ = true;
                source.set_error(events.error());
                return;
            }
            implementation->topic_events_ = events.value();
            implementation->writer_ = std::make_shared<Writer>(implementation->config_.writer_control_queue_capacity);
            const auto pool_capacity = message_pool_capacity(implementation->config_);
            if (!pool_capacity) {
                implementation->state_.recorder_state = RecorderState::Failed;
                implementation->state_.last_error = pool_capacity.error();
                implementation->poisoned_ = true;
                source.set_error(pool_capacity.error());
                return;
            }
            try {
                implementation->message_pool_ = std::make_shared<MessagePool>(pool_capacity.value());
                implementation->message_queue_ = std::make_shared<MessageQueue>(
                    implementation->config_.message_queue_max_messages,
                    implementation->config_.message_queue_max_bytes);
                if (config.value().mode == TaskMode::Rolling) {
                    SnapshotConfig snapshot_config;
                    snapshot_config.max_messages_per_topic = implementation->config_.snapshot_max_messages_per_topic;
                    snapshot_config.max_bytes_per_topic = implementation->config_.snapshot_max_bytes_per_topic;
                    snapshot_config.max_total_bytes = implementation->config_.snapshot_max_total_bytes;
                    snapshot_config.max_age = config.value().retention_duration;
                    implementation->snapshot_ = std::make_unique<Snapshot>(snapshot_config);
                }
                implementation->pool_exhausted_.store(0, std::memory_order_relaxed);
                implementation->queue_full_.store(0, std::memory_order_relaxed);
                implementation->writer_enqueue_failure_.store(0, std::memory_order_relaxed);
                implementation->start_dispatch_thread(implementation->writer_);
                implementation->cleanup_ = std::make_unique<Cleanup>(
                    implementation->config_.cleanup_queue_capacity, implementation->config_.cleanup_warning,
                    [implementation](DeleteResult result) {
                        (void)implementation->submit([implementation, result = std::move(result)] {
                            std::lock_guard<std::mutex> state_lock(implementation->state_mutex_);
                            if (!implementation->manifest_) return;
                            if (result.error) {
                                implementation->state_.cleanup.last_error = result.error;
                                return;
                            }
                            ManifestData updated = implementation->manifest_->data();
                            if (updated.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
                                implementation->state_.last_error = error(ErrorCode::StorageError, "manifest revision exhausted");
                                implementation->state_.recorder_state = RecorderState::Failed;
                                implementation->poisoned_ = true;
                                return;
                            }
                            bool found = false;
                            for (auto& segment : updated.segments) {
                                if (segment.segment_id == result.work.segment_id &&
                                    segment.state == SegmentLifecycleState::Deleting) {
                                    segment.state = SegmentLifecycleState::Removed;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) return;
                            ++updated.manifest_revision;
                            const auto committed = implementation->manifest_->commit(std::move(updated));
                            if (!committed) {
                                implementation->state_.last_error = committed.error();
                                implementation->state_.recorder_state = RecorderState::Failed;
                                implementation->poisoned_ = true;
                            } else {
                                implementation->completed_segment_steady_.erase(result.work.segment_id);
                            }
                        });
                    });
                DiskMonitorConfig disk_config;
                disk_config.path = implementation->config_.output_root;
                disk_config.max_age = implementation->config_.disk_max_age;
                disk_config.failure_threshold = implementation->config_.disk_consecutive_failures;
                disk_config.warning_percent = implementation->config_.disk_warning_percent;
                disk_config.pause_percent = implementation->config_.disk_pause_percent;
                disk_config.min_free_bytes = implementation->config_.disk_min_free_bytes;
                implementation->resource_monitor_ = std::make_unique<ResourceMonitor>(
                    disk_config, implementation->disk_admission_, [implementation](DiskSample sample) {
                        (void)implementation->submit([implementation, sample] {
                            std::lock_guard<std::mutex> state_lock(implementation->state_mutex_);
                            implementation->state_.disk = sample;
                            if (sample.state == DiskSafetyState::Critical || sample.state == DiskSafetyState::Unknown) {
                                implementation->disk_safe_since_.reset();
                                implementation->state_.pause.disk = sample.state == DiskSafetyState::Critical
                                    ? DiskPauseReason::Critical : DiskPauseReason::Unknown;
                                if (implementation->state_.recorder_state != RecorderState::Recording) return;
                                implementation->state_.recorder_state = RecorderState::Paused;
                                for (auto& entry : implementation->active_topics_) {
                                    entry.second.admission->close();
                                    std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                                    entry.second.ingress->open = false;
                                }
                                return;
                            }
                            if (implementation->state_.pause.disk == DiskPauseReason::None) return;
                            if (!implementation->disk_safe_since_) {
                                implementation->disk_safe_since_ = std::chrono::steady_clock::now();
                            }
                            if (std::chrono::steady_clock::now() - *implementation->disk_safe_since_ <
                                implementation->config_.disk_stable) {
                                return;
                            }
                            if (!implementation->config_.disk_auto_resume) {
                                implementation->state_.pause.disk = DiskPauseReason::RecoveryPending;
                                return;
                            }
                            implementation->state_.pause.disk = DiskPauseReason::None;
                            if (implementation->state_.pause.user_paused) return;
                            if (implementation->state_.recorder_state != RecorderState::Paused) return;
                            implementation->state_.recorder_state = RecorderState::Recording;
                            if (!implementation->state_.generation) return;
                            for (auto& entry : implementation->active_topics_) {
                                {
                                    std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                                    entry.second.ingress->open = true;
                                }
                                entry.second.admission->open(*implementation->state_.generation, entry.second.info.topic_epoch);
                            }
                        });
                    });
            } catch (const std::exception& exception) {
                implementation->state_.recorder_state = RecorderState::Failed;
                implementation->state_.last_error = error(ErrorCode::ResourceExhausted, exception.what());
                implementation->poisoned_ = true;
                source.set_error(*implementation->state_.last_error);
                return;
            }
            implementation->state_.recorder_state = RecorderState::Initializing;
            implementation->state_.recording_id = data.task.recording_id;
            implementation->state_.generation = data.task.generation;
            implementation->state_.task_config = data.task.config;
            ManifestData recording = data;
            recording.manifest_revision = 2;
            recording.task.state = TaskState::Recording;
            recording.task.start_time_ns = wall_time_ns();
            const auto committed = implementation->manifest_->commit(recording);
            if (!committed) {
                implementation->state_.recorder_state = RecorderState::Failed;
                implementation->state_.last_error = committed.error();
                implementation->poisoned_ = true;
                source.set_error(error(ErrorCode::StorageError, "recording manifest transition failed", Effects::ConfirmedPartial));
                return;
            }
            implementation->state_.recorder_state = RecorderState::Recording;
            if (recording.task.config.mode == TaskMode::Timed) {
                const auto deadline = std::chrono::steady_clock::now() + *recording.task.config.duration;
                implementation->timed_deadline_steady_ns_.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count(),
                    std::memory_order_release);
                implementation->queue_condition_.notify_one();
            }
            source.set_value(recording.task);
        })) {
        source.set_error(error(ErrorCode::ResourceExhausted, "recorder mutation queue is full"));
    }
    return completion;
}

Completion<void> Recorder::stop(std::string_view recording_id, std::uint64_t generation) {
    if (implementation_->shutdown_requested_.load()) return failed_completion<void>(error(ErrorCode::InvalidState, "shutdown requested"));
    CompletionSource<void> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (!implementation_->submit([implementation = implementation_.get(), id, generation, source] {
            std::unique_lock<std::mutex> lock(implementation->state_mutex_);
            if (!implementation->state_.recording_id || *implementation->state_.recording_id != id ||
                !implementation->state_.generation || *implementation->state_.generation != generation) {
                source.set_error(error(ErrorCode::StaleRecording, "stale recording identity"));
                return;
            }
            if (implementation->state_.recorder_state != RecorderState::Recording &&
                implementation->state_.recorder_state != RecorderState::Paused) {
                source.set_error(error(ErrorCode::InvalidState, "recording cannot be stopped in this state"));
                return;
            }
            implementation->state_.recorder_state = RecorderState::Stopping;
            for (auto& entry : implementation->active_topics_) {
                entry.second.admission->close();
                std::unique_lock<std::mutex> ingress_lock(entry.second.ingress->mutex);
                entry.second.ingress->open = false;
                entry.second.ingress->condition.wait(
                    ingress_lock, [&entry] { return entry.second.ingress->producers == 0; });
            }
            implementation->active_topics_.clear();
            implementation->state_.topics.clear();
            const auto writer = implementation->writer_;
            const bool writer_open = implementation->writer_open_;
            lock.unlock();
            implementation->resource_monitor_.reset();
            implementation->stop_dispatch_thread();
            if (writer_open) {
                const auto closed = completed_result(writer->close(), std::chrono::seconds(30));
                if (!closed) {
                    lock.lock();
                    implementation->state_.recorder_state = RecorderState::Failed;
                    implementation->state_.last_error = closed.error();
                    implementation->poisoned_ = true;
                    source.set_error(closed.error());
                    return;
                }
            }
            lock.lock();
            implementation->writer_open_ = false;
            ManifestData completed = implementation->manifest_->data();
            if (completed.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
                implementation->state_.recorder_state = RecorderState::Failed;
                implementation->state_.last_error = error(ErrorCode::StorageError, "manifest revision exhausted");
                implementation->poisoned_ = true;
                source.set_error(*implementation->state_.last_error);
                return;
            }
            completed.manifest_revision++;
            completed.task.state = TaskState::Completed;
            completed.task.terminal_time_ns = wall_time_ns();
            for (auto& segment : completed.segments) {
                if (segment.state == SegmentLifecycleState::Writing) {
                    segment.state = SegmentLifecycleState::Complete;
                    segment.completed_at_ns = completed.task.terminal_time_ns;
                }
            }
            const auto committed = implementation->manifest_->commit(completed);
            if (!committed) {
                implementation->state_.recorder_state = RecorderState::Failed;
                implementation->state_.last_error = committed.error();
                implementation->poisoned_ = true;
                source.set_error(committed.error());
                return;
            }
            implementation->manifest_.reset();
            implementation->topic_events_.reset();
            implementation->writer_.reset();
            implementation->snapshot_.reset();
            implementation->message_queue_.reset();
            implementation->message_pool_.reset();
            implementation->cleanup_.reset();
            implementation->state_ = State{};
            implementation->timed_deadline_steady_ns_.store(0, std::memory_order_release);
            implementation->rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            source.set_value();
        })) source.set_error(error(ErrorCode::ResourceExhausted, "recorder mutation queue is full"));
    return completion;
}

Completion<void> Recorder::pause(std::string_view recording_id, std::uint64_t generation) {
    CompletionSource<void> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (implementation_->shutdown_requested_.load() || !implementation_->submit([implementation = implementation_.get(), id, generation, source] {
            std::lock_guard<std::mutex> lock(implementation->state_mutex_);
            if (!implementation->state_.recording_id || *implementation->state_.recording_id != id || !implementation->state_.generation || *implementation->state_.generation != generation) { source.set_error(error(ErrorCode::StaleRecording, "stale recording identity")); return; }
            if (implementation->state_.recorder_state != RecorderState::Recording && implementation->state_.recorder_state != RecorderState::Paused) { source.set_error(error(ErrorCode::InvalidState, "recording cannot be paused")); return; }
            if (implementation->state_.pause.user_paused) { source.set_error(error(ErrorCode::AlreadyPaused, "already user paused")); return; }
            implementation->state_.pause.user_paused = true;
            for (auto& entry : implementation->active_topics_) {
                entry.second.admission->close();
                std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                entry.second.ingress->open = false;
            }
            implementation->state_.recorder_state = RecorderState::Paused;
            source.set_value();
        })) source.set_error(error(ErrorCode::InvalidState, "shutdown requested or mutation queue full"));
    return completion;
}

Completion<void> Recorder::resume(std::string_view recording_id, std::uint64_t generation) {
    CompletionSource<void> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (implementation_->shutdown_requested_.load() || !implementation_->submit([implementation = implementation_.get(), id, generation, source] {
            std::lock_guard<std::mutex> lock(implementation->state_mutex_);
            if (!implementation->state_.recording_id || *implementation->state_.recording_id != id || !implementation->state_.generation || *implementation->state_.generation != generation) { source.set_error(error(ErrorCode::StaleRecording, "stale recording identity")); return; }
            if (implementation->state_.recorder_state != RecorderState::Paused) { source.set_error(error(ErrorCode::InvalidState, "recording is not paused")); return; }
            const bool cleared_user_pause = implementation->state_.pause.user_paused;
            if (!cleared_user_pause && implementation->state_.pause.disk == DiskPauseReason::None) { source.set_error(error(ErrorCode::InvalidState, "no pause reason to resume")); return; }
            implementation->state_.pause.user_paused = false;
            const auto disk_effects = cleared_user_pause ? Effects::ConfirmedPartial : Effects::None;
            if (implementation->state_.pause.disk == DiskPauseReason::Critical) { source.set_error(error(ErrorCode::DiskCritical, "disk remains critical", disk_effects)); return; }
            if (implementation->state_.pause.disk == DiskPauseReason::Unknown) { source.set_error(error(ErrorCode::DiskUnknown, "disk safety is unknown", disk_effects)); return; }
            if (implementation->state_.pause.disk == DiskPauseReason::RecoveryPending) {
                const auto safe = implementation->state_.disk && implementation->state_.disk->valid &&
                    (implementation->state_.disk->state == DiskSafetyState::Normal ||
                     implementation->state_.disk->state == DiskSafetyState::Warning) &&
                    implementation->disk_safe_since_ &&
                    std::chrono::steady_clock::now() - *implementation->disk_safe_since_ >= implementation->config_.disk_stable;
                if (!safe) {
                    const auto code = implementation->state_.disk &&
                        implementation->state_.disk->state == DiskSafetyState::Critical
                        ? ErrorCode::DiskCritical : ErrorCode::DiskUnknown;
                    source.set_error(error(code, "disk recovery is not yet confirmed", disk_effects));
                    return;
                }
                implementation->state_.pause.disk = DiskPauseReason::None;
            }
            for (auto& entry : implementation->active_topics_) {
                {
                    std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                    entry.second.ingress->open = true;
                }
                entry.second.admission->open(generation, entry.second.info.topic_epoch);
            }
            implementation->state_.recorder_state = RecorderState::Recording;
            source.set_value();
        })) source.set_error(error(ErrorCode::InvalidState, "shutdown requested or mutation queue full"));
    return completion;
}

Completion<TopicInfo> Recorder::add_topic(
    std::string_view recording_id, std::uint64_t generation, const TopicOptions& options, const OperationId& operation_id) {
    if (implementation_->shutdown_requested_.load()) {
        return failed_completion<TopicInfo>(error(ErrorCode::InvalidState, "shutdown requested"));
    }
    CompletionSource<TopicInfo> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (!implementation_->submit([implementation = implementation_.get(), id, generation, options, operation_id, source] {
            std::unique_lock<std::mutex> lock(implementation->state_mutex_);
            if (!implementation->state_.recording_id || *implementation->state_.recording_id != id ||
                !implementation->state_.generation || *implementation->state_.generation != generation) {
                source.set_error(error(ErrorCode::StaleRecording, "stale recording identity"));
                return;
            }
            if (implementation->state_.recorder_state != RecorderState::Recording || !implementation->node_) {
                source.set_error(error(implementation->node_ ? ErrorCode::InvalidState : ErrorCode::Unavailable,
                                       implementation->node_ ? "topics can only be added while recording" : "ROS node is not available"));
                return;
            }
            if (operation_id.empty()) {
                source.set_error(error(ErrorCode::InvalidArgument, "operation_id must not be empty"));
                return;
            }
            if (implementation->active_topics_.size() >= implementation->config_.max_active_topics) {
                source.set_error(error(ErrorCode::ResourceExhausted, "active topic limit reached"));
                return;
            }
            const auto valid = validate_topic_options(options);
            if (!valid) {
                source.set_error(valid.error());
                return;
            }
            const auto qos = normalize_qos(options.qos);
            if (!qos) {
                source.set_error(qos.error());
                return;
            }
            for (const auto& item : implementation->active_topics_) {
                if (item.second.info.name == options.name && item.second.info.type != options.type) {
                    source.set_error(error(ErrorCode::TopicTypeConflict, "active topic name has a different type"));
                    return;
                }
                if (item.second.info.name == options.name && item.second.info.type == options.type) {
                    source.set_error(error(ErrorCode::InvalidState, "topic is already active"));
                    return;
                }
            }
            if (!implementation->topic_events_ || implementation->topic_events_->uncertainty_watermark()) {
                source.set_error(error(ErrorCode::EventCommitFailed, "topic event chain is uncertain", Effects::Possible));
                return;
            }
            const auto existing_operation = implementation->topic_events_->operation_state(operation_id);
            if (!existing_operation) {
                source.set_error(existing_operation.error());
                return;
            }
            if (existing_operation.value() != TopicOperationState::NotFound) {
                source.set_error(error(ErrorCode::InvalidArgument, "operation_id has already been used"));
                return;
            }
            const auto schema = resolve_schema(options.type);
            if (!schema) {
                source.set_error(schema.error());
                return;
            }
            if (implementation->next_topic_id_ == std::numeric_limits<TopicId>::max()) {
                source.set_error(error(ErrorCode::ResourceExhausted, "topic id space exhausted"));
                return;
            }
            const TopicId topic_id = implementation->next_topic_id_++;
            auto& epoch = implementation->topic_epochs_[options.name];
            if (epoch == std::numeric_limits<std::uint64_t>::max()) {
                source.set_error(error(ErrorCode::ResourceExhausted, "topic epoch space exhausted"));
                return;
            }
            ++epoch;
            TopicInfo info;
            info.topic_id = topic_id;
            info.topic_epoch = epoch;
            info.name = options.name;
            info.type = options.type;
            info.qos = qos.value();
            // ROS Humble does not expose a portable definition resolver.  The type-support
            // preflight above is still mandatory; schema capture is upgraded separately.
            info.schema_sha256 = schema->sha256;
            info.expected_hz = options.expected_hz;

            TopicEvent prepared;
            prepared.operation_id = operation_id;
            prepared.topic_id = info.topic_id;
            prepared.topic_epoch = info.topic_epoch;
            prepared.type = TopicEventType::AddPrepared;
            prepared.timestamp_ns = wall_time_ns();
            const auto appended = implementation->topic_events_->append(std::move(prepared));
            if (!appended) {
                source.set_error(appended.error());
                return;
            }

            const auto directory = implementation->manifest_->task_directory() / "segment_000001";
            auto writer = implementation->writer_;
            lock.unlock();
            if (!implementation->writer_open_) {
                ManifestData opening = implementation->manifest_->data();
                if (opening.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
                    lock.lock();
                    implementation->state_.recorder_state = RecorderState::Failed;
                    implementation->state_.last_error = error(ErrorCode::StorageError, "manifest revision exhausted");
                    implementation->poisoned_ = true;
                    source.set_error(*implementation->state_.last_error);
                    return;
                }
                SegmentInfo segment;
                segment.segment_id = 1;
                segment.state = SegmentLifecycleState::Writing;
                segment.relative_path = "segment_000001";
                segment.started_at_ns = wall_time_ns();
                opening.segments.push_back(segment);
                ++opening.manifest_revision;
                const auto persisted_segment = implementation->manifest_->commit(std::move(opening));
                if (!persisted_segment) {
                    lock.lock();
                    implementation->state_.recorder_state = RecorderState::Failed;
                    implementation->state_.last_error = persisted_segment.error();
                    implementation->poisoned_ = true;
                    source.set_error(persisted_segment.error());
                    return;
                }
                const auto opened = completed_result(writer->open(directory.string()), std::chrono::seconds(30));
                if (!opened) {
                    lock.lock();
                    TopicEvent failed;
                    failed.operation_id = operation_id;
                    failed.topic_id = info.topic_id;
                    failed.topic_epoch = info.topic_epoch;
                    failed.type = TopicEventType::AddFailed;
                    failed.timestamp_ns = wall_time_ns();
                    failed.error = opened.error();
                    const auto failure_event = implementation->topic_events_->append(std::move(failed));
                    source.set_error(failure_event ? opened.error() : failure_event.error());
                    return;
                }
                implementation->writer_open_ = true;
                lock.lock();
                SegmentState state_segment;
                state_segment.segment_id = 1;
                state_segment.state = SegmentLifecycleState::Writing;
                implementation->state_.current_segment = state_segment;
                lock.unlock();
            }
            const auto registered = completed_result(writer->register_topic(info), std::chrono::seconds(30));
            if (!registered) {
                lock.lock();
                TopicEvent failed;
                failed.operation_id = operation_id;
                failed.topic_id = info.topic_id;
                failed.topic_epoch = info.topic_epoch;
                failed.type = TopicEventType::AddFailed;
                failed.timestamp_ns = wall_time_ns();
                failed.error = registered.error();
                const auto failure_event = implementation->topic_events_->append(std::move(failed));
                source.set_error(failure_event ? registered.error() : failure_event.error());
                return;
            }

            auto ingress = std::make_shared<Implementation::Ingress>();
            auto admission = std::make_shared<AdmissionGate>();
            auto frozen_info = std::make_shared<TopicInfo>(info);
            std::shared_ptr<rclcpp::GenericSubscription> subscription;
            try {
                ingress->queue = implementation->message_queue_;
                subscription = implementation->node_->create_generic_subscription(
                    info.name, info.type, make_qos(info.qos),
                    [ingress, admission, pool = implementation->message_pool_,
                     disk = &implementation->disk_admission_, frozen_info, pool_exhausted = &implementation->pool_exhausted_,
                     queue_full = &implementation->queue_full_, generation, topic_id = info.topic_id,
                     topic_epoch = info.topic_epoch](std::shared_ptr<rclcpp::SerializedMessage> message) {
                        if (admission->admit(generation, topic_epoch, *disk) != AdmissionResult::Accepted) return;
                        std::shared_ptr<MessageQueue> queue;
                        {
                            std::lock_guard<std::mutex> ingress_lock(ingress->mutex);
                            if (!ingress->open) return;
                            ++ingress->producers;
                            queue = ingress->queue;
                        }
                        if (!queue) {
                            std::lock_guard<std::mutex> ingress_lock(ingress->mutex);
                            --ingress->producers;
                            ingress->condition.notify_all();
                            return;
                        }
                        auto owned = pool->acquire();
                        if (!owned) {
                            pool_exhausted->fetch_add(1, std::memory_order_relaxed);
                            std::lock_guard<std::mutex> ingress_lock(ingress->mutex);
                            --ingress->producers;
                            ingress->condition.notify_all();
                            return;
                        }
                        RecordedMessage recorded;
                        recorded.generation = generation;
                        recorded.topic_id = topic_id;
                        recorded.topic_epoch = topic_epoch;
                        recorded.payload_bytes = message->size();
                        recorded.receive_timestamp_ns = wall_time_ns();
                        recorded.topic = frozen_info;
                        try {
                            *owned = *message;
                        } catch (...) {
                            std::lock_guard<std::mutex> ingress_lock(ingress->mutex);
                            --ingress->producers;
                            ingress->condition.notify_all();
                            return;
                        }
                        recorded.message = std::move(owned);
                        if (!queue->try_push(std::move(recorded))) {
                            queue_full->fetch_add(1, std::memory_order_relaxed);
                        }
                        {
                            std::lock_guard<std::mutex> ingress_lock(ingress->mutex);
                            --ingress->producers;
                        }
                        ingress->condition.notify_all();
                    });
            } catch (const std::exception& exception) {
                lock.lock();
                const auto subscription_error = error(ErrorCode::Unavailable, exception.what(), Effects::ConfirmedPartial);
                TopicEvent failed;
                failed.operation_id = operation_id;
                failed.topic_id = info.topic_id;
                failed.topic_epoch = info.topic_epoch;
                failed.type = TopicEventType::AddFailed;
                failed.timestamp_ns = wall_time_ns();
                failed.error = subscription_error;
                const auto failure_event = implementation->topic_events_->append(std::move(failed));
                source.set_error(failure_event ? subscription_error : failure_event.error());
                return;
            }
            lock.lock();
            TopicEvent added;
            added.operation_id = operation_id;
            added.topic_id = info.topic_id;
            added.topic_epoch = info.topic_epoch;
            added.type = TopicEventType::TopicAdded;
            added.timestamp_ns = wall_time_ns();
            const auto added_event = implementation->topic_events_->append(std::move(added));
            if (!added_event) {
                source.set_error(added_event.error());
                return;
            }
            {
                std::lock_guard<std::mutex> ingress_lock(ingress->mutex);
                ingress->open = true;
            }
            admission->open(generation, info.topic_epoch);
            implementation->active_topics_.emplace(
                info.topic_id, Implementation::ActiveTopic{info, std::move(subscription), ingress, admission, frozen_info});
            TopicState state_topic;
            state_topic.info = info;
            state_topic.state = TopicRuntimeState::Active;
            state_topic.publisher_count = implementation->node_->count_publishers(info.name);
            state_topic.publisher_visibility_fresh = true;
            implementation->state_.topics.push_back(std::move(state_topic));
            source.set_value(std::move(info));
        })) {
        source.set_error(error(ErrorCode::ResourceExhausted, "recorder mutation queue is full"));
    }
    return completion;
}

Completion<void> Recorder::remove_topic(
    std::string_view recording_id, std::uint64_t generation, TopicId topic_id, std::uint64_t topic_epoch,
    const OperationId& operation_id) {
    if (implementation_->shutdown_requested_.load()) return failed_completion<void>(error(ErrorCode::InvalidState, "shutdown requested"));
    CompletionSource<void> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (!implementation_->submit([implementation = implementation_.get(), id, generation, topic_id, topic_epoch, operation_id, source] {
            std::unique_lock<std::mutex> lock(implementation->state_mutex_);
            if (!implementation->state_.recording_id || *implementation->state_.recording_id != id ||
                !implementation->state_.generation || *implementation->state_.generation != generation) {
                source.set_error(error(ErrorCode::StaleRecording, "stale recording identity"));
                return;
            }
            if (implementation->state_.recorder_state != RecorderState::Recording &&
                implementation->state_.recorder_state != RecorderState::Paused) {
                source.set_error(error(ErrorCode::InvalidState, "topics can only be removed from an active task"));
                return;
            }
            if (operation_id.empty()) {
                source.set_error(error(ErrorCode::InvalidArgument, "operation_id must not be empty"));
                return;
            }
            const auto found = implementation->active_topics_.find(topic_id);
            if (found == implementation->active_topics_.end() || found->second.info.topic_epoch != topic_epoch) {
                source.set_error(error(ErrorCode::StaleTopicEpoch, "stale topic identity"));
                return;
            }
            if (!implementation->topic_events_ || implementation->topic_events_->uncertainty_watermark()) {
                source.set_error(error(ErrorCode::EventCommitFailed, "topic event chain is uncertain", Effects::Possible));
                return;
            }
            const auto existing_operation = implementation->topic_events_->operation_state(operation_id);
            if (!existing_operation) {
                source.set_error(existing_operation.error());
                return;
            }
            if (existing_operation.value() != TopicOperationState::NotFound) {
                source.set_error(error(ErrorCode::InvalidArgument, "operation_id has already been used"));
                return;
            }
            TopicEvent prepared;
            prepared.operation_id = operation_id;
            prepared.topic_id = topic_id;
            prepared.topic_epoch = topic_epoch;
            prepared.type = TopicEventType::RemovePrepared;
            prepared.timestamp_ns = wall_time_ns();
            const auto appended = implementation->topic_events_->append(std::move(prepared));
            if (!appended) {
                source.set_error(appended.error());
                return;
            }
            auto active = std::move(found->second);
            implementation->active_topics_.erase(found);
            active.admission->close();
            {
                std::unique_lock<std::mutex> ingress_lock(active.ingress->mutex);
                active.ingress->open = false;
                active.ingress->condition.wait(ingress_lock, [&active] { return active.ingress->producers == 0; });
            }
            active.subscription.reset();
            {
                std::lock_guard<std::mutex> snapshot_lock(implementation->snapshot_mutex_);
                if (implementation->snapshot_) implementation->snapshot_->erase_topic(topic_id, topic_epoch);
            }
            TopicEvent removed;
            removed.operation_id = operation_id;
            removed.topic_id = topic_id;
            removed.topic_epoch = topic_epoch;
            removed.type = TopicEventType::TopicRemoved;
            removed.timestamp_ns = wall_time_ns();
            const auto removed_event = implementation->topic_events_->append(std::move(removed));
            if (!removed_event) {
                source.set_error(removed_event.error());
                return;
            }
            implementation->state_.topics.erase(
                std::remove_if(implementation->state_.topics.begin(), implementation->state_.topics.end(),
                    [topic_id](const TopicState& topic) { return topic.info.topic_id == topic_id; }),
                implementation->state_.topics.end());
            implementation->state_.topics_removed_recent.push_back(active.info);
            ++implementation->state_.topics_removed_total;
            const auto capacity = implementation->config_.recent_removed_instances;
            if (implementation->state_.topics_removed_recent.size() > capacity) {
                implementation->state_.topics_removed_recent.erase(implementation->state_.topics_removed_recent.begin());
            }
            source.set_value();
        })) {
        source.set_error(error(ErrorCode::ResourceExhausted, "recorder mutation queue is full"));
    }
    return completion;
}

State Recorder::state() const {
    std::lock_guard<std::mutex> lock(implementation_->state_mutex_);
    State snapshot = implementation_->state_;
    if (implementation_->message_queue_) snapshot.queue = implementation_->message_queue_->state();
    if (implementation_->message_pool_) snapshot.pool = implementation_->message_pool_->state();
    if (implementation_->cleanup_) snapshot.cleanup = implementation_->cleanup_->state();
    snapshot.dropped.pool_exhausted = implementation_->pool_exhausted_.load(std::memory_order_relaxed);
    snapshot.dropped.queue_full = implementation_->queue_full_.load(std::memory_order_relaxed);
    snapshot.dropped.enqueue_failure = implementation_->writer_enqueue_failure_.load(std::memory_order_relaxed);
    for (const auto& entry : implementation_->active_topics_) {
        const auto dropped = entry.second.admission->counters();
        snapshot.dropped.stale_identity += dropped.stale_identity;
        snapshot.dropped.not_accepting += dropped.not_accepting;
        snapshot.dropped.timed_deadline += dropped.timed_deadline;
        snapshot.dropped.disk_unsafe += dropped.disk_unsafe;
        snapshot.dropped.disk_snapshot_busy += dropped.disk_snapshot_busy;
    }
    snapshot.shutting_down = implementation_->shutdown_requested_.load();
    return snapshot;
}

Result<std::vector<DiscoveredTopic>, Error> Recorder::discover_topics() const {
    if (!implementation_->node_) {
        return Result<std::vector<DiscoveredTopic>, Error>(
            unexpect, error(ErrorCode::Unavailable, "ROS node is not available"));
    }
    try {
        const auto graph = implementation_->node_->get_topic_names_and_types();
        std::vector<DiscoveredTopic> discovered;
        discovered.reserve(std::min(graph.size(), implementation_->config_.discovery_max_results));
        for (const auto& entry : graph) {
            if (discovered.size() == implementation_->config_.discovery_max_results) break;
            DiscoveredTopic topic;
            topic.name = entry.first;
            topic.types = entry.second;
            std::sort(topic.types.begin(), topic.types.end());
            topic.types.erase(std::unique(topic.types.begin(), topic.types.end()), topic.types.end());
            topic.publisher_count = implementation_->node_->count_publishers(topic.name);
            if (topic.types.size() > 1) topic.recordability = TopicRecordability::TypeConflict;
            discovered.push_back(std::move(topic));
        }
        std::sort(discovered.begin(), discovered.end(), [](const DiscoveredTopic& left, const DiscoveredTopic& right) {
            return left.name < right.name;
        });
        return Result<std::vector<DiscoveredTopic>, Error>(std::move(discovered));
    } catch (const std::exception& exception) {
        return Result<std::vector<DiscoveredTopic>, Error>(
            unexpect, error(ErrorCode::Unavailable, exception.what()));
    }
}

Result<TaskDetail, Error> Recorder::get_task(
    std::string_view recording_id, std::size_t segment_offset, std::size_t segment_limit) const {
    if (segment_limit == 0 || segment_limit > implementation_->config_.max_segment_detail_limit) {
        return Result<TaskDetail, Error>(unexpect, error(ErrorCode::InvalidArgument, "invalid segment limit"));
    }
    if (!valid_recording_id(recording_id)) {
        return Result<TaskDetail, Error>(unexpect, error(ErrorCode::InvalidArgument, "invalid recording_id"));
    }
    const auto directory = implementation_->config_.output_root / std::string(recording_id);
    if (!std::filesystem::exists(directory)) {
        return Result<TaskDetail, Error>(unexpect, error(ErrorCode::NotFound, "recording does not exist"));
    }
    const auto manifest = Manifest::load(directory);
    if (!manifest) return Result<TaskDetail, Error>(unexpect, manifest.error());
    TaskDetail detail;
    detail.task = manifest->data().task;
    detail.segment_offset = segment_offset;
    detail.detail_segment_count = manifest->data().segments.size();
    detail.total_segment_count = detail.detail_segment_count + detail.task.segment_history_summary.compacted_removed_count;
    if (segment_offset < detail.detail_segment_count) {
        const auto end = std::min(detail.detail_segment_count, segment_offset + segment_limit);
        detail.segments.insert(detail.segments.end(), manifest->data().segments.begin() + segment_offset,
                               manifest->data().segments.begin() + end);
        detail.segment_has_more = end < detail.detail_segment_count;
    }
    return Result<TaskDetail, Error>(std::move(detail));
}

Result<TaskList, Error> Recorder::list_tasks(std::size_t offset, std::size_t limit) const {
    if (limit == 0) return Result<TaskList, Error>(unexpect, error(ErrorCode::InvalidArgument, "limit must be positive"));
    TaskList list;
    std::error_code filesystem_error;
    std::vector<TaskInfo> candidates;
    if (!std::filesystem::exists(implementation_->config_.output_root, filesystem_error)) return Result<TaskList, Error>(std::move(list));
    for (const auto& entry : std::filesystem::directory_iterator(implementation_->config_.output_root, filesystem_error)) {
        if (filesystem_error) return Result<TaskList, Error>(unexpect, error(ErrorCode::StorageError, filesystem_error.message()));
        if (!entry.is_directory()) continue;
        if (list.scanned_entries == implementation_->config_.max_task_scan_entries) { list.scan_truncated = true; break; }
        ++list.scanned_entries;
        const auto manifest = Manifest::load(entry.path());
        if (manifest) candidates.push_back(manifest->data().task);
    }
    std::sort(candidates.begin(), candidates.end(), [](const TaskInfo& left, const TaskInfo& right) {
        if (left.start_time_ns && right.start_time_ns && *left.start_time_ns != *right.start_time_ns) return *left.start_time_ns > *right.start_time_ns;
        if (left.start_time_ns.has_value() != right.start_time_ns.has_value()) return left.start_time_ns.has_value();
        return left.recording_id < right.recording_id;
    });
    if (offset < candidates.size()) {
        const auto end = std::min(candidates.size(), offset + limit);
        list.items.insert(list.items.end(), candidates.begin() + offset, candidates.begin() + end);
        list.has_more = end < candidates.size();
    }
    return Result<TaskList, Error>(std::move(list));
}

Result<TopicOperationState, Error> Recorder::topic_operation_state(
    std::string_view recording_id, const OperationId& operation_id) const {
    if (!valid_recording_id(recording_id)) return Result<TopicOperationState, Error>(unexpect, error(ErrorCode::InvalidArgument, "invalid recording_id"));
    const auto directory = implementation_->config_.output_root / std::string(recording_id);
    if (!std::filesystem::exists(directory)) {
        return Result<TopicOperationState, Error>(unexpect, error(ErrorCode::NotFound, "recording does not exist"));
    }
    const auto manifest = Manifest::load(directory);
    if (!manifest) return Result<TopicOperationState, Error>(unexpect, manifest.error());
    const auto events = TopicEvents::open(directory, manifest->data().task.recording_id, manifest->data().task.generation);
    if (!events) return Result<TopicOperationState, Error>(unexpect, events.error());
    return events->operation_state(operation_id);
}

Completion<void> Recorder::shutdown() {
    implementation_->request_shutdown();
    return implementation_->shutdown_completion_;
}

}  // namespace ros2_bag::recorder
