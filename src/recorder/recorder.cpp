#include "ros2_bag/recorder/recorder.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

#include "ros2_bag/recorder/manifest.hpp"

namespace ros2_bag::recorder {
namespace {

Error error(ErrorCode code, std::string message, Effects effects = Effects::None) {
    return {code, effects, std::move(message)};
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
                const auto deadline_ns = timed_deadline_steady_ns_.load(std::memory_order_acquire);
                if (deadline_ns == 0) {
                    queue_condition_.wait(lock, [this] { return stopping_worker_ || !queue_.empty(); });
                } else {
                    const auto deadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline_ns));
                    if (!queue_condition_.wait_until(lock, deadline, [this] { return stopping_worker_ || !queue_.empty(); })) {
                        lock.unlock();
                        stop_timed_task();
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

    void stop_timed_task() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!manifest_ || (state_.recorder_state != RecorderState::Recording && state_.recorder_state != RecorderState::Paused)) {
            timed_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        state_.recorder_state = RecorderState::Stopping;
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
        const auto committed = manifest_->commit(completed);
        if (!committed) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = committed.error();
            poisoned_ = true;
        } else {
            manifest_.reset();
            state_ = State{};
        }
        timed_deadline_steady_ns_.store(0, std::memory_order_release);
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
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                if (manifest_) {
                    ManifestData failed = manifest_->data();
                    if (failed.manifest_revision != std::numeric_limits<std::uint64_t>::max()) {
                        ++failed.manifest_revision;
                        failed.task.state = TaskState::Failed;
                        failed.task.error = error(ErrorCode::InternalError, "shutdown interrupted active task", Effects::ConfirmedPartial);
                        const auto committed = manifest_->commit(failed);
                        if (!committed) state_.last_error = committed.error();
                    }
                    manifest_.reset();
                }
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
    std::uint64_t next_generation_ = 1;
    bool poisoned_ = false;
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<std::int64_t> timed_deadline_steady_ns_{0};
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
            implementation->state_.recorder_state = RecorderState::Initializing;
            implementation->state_.recording_id = data.task.recording_id;
            implementation->state_.generation = data.task.generation;
            implementation->state_.task_config = data.task.config;
            ManifestData recording = data;
            recording.manifest_revision = 2;
            recording.task.state = TaskState::Recording;
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
            std::lock_guard<std::mutex> lock(implementation->state_mutex_);
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
            const auto committed = implementation->manifest_->commit(completed);
            if (!committed) {
                implementation->state_.recorder_state = RecorderState::Failed;
                implementation->state_.last_error = committed.error();
                implementation->poisoned_ = true;
                source.set_error(committed.error());
                return;
            }
            implementation->manifest_.reset();
            implementation->state_ = State{};
            implementation->timed_deadline_steady_ns_.store(0, std::memory_order_release);
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
            if (!implementation->state_.pause.user_paused && implementation->state_.pause.disk == DiskPauseReason::None) { source.set_error(error(ErrorCode::InvalidState, "no pause reason to resume")); return; }
            if (implementation->state_.pause.disk == DiskPauseReason::Critical) { source.set_error(error(ErrorCode::DiskCritical, "disk remains critical")); return; }
            if (implementation->state_.pause.disk == DiskPauseReason::Unknown) { source.set_error(error(ErrorCode::DiskUnknown, "disk safety is unknown")); return; }
            implementation->state_.pause = PauseState{};
            implementation->state_.recorder_state = RecorderState::Recording;
            source.set_value();
        })) source.set_error(error(ErrorCode::InvalidState, "shutdown requested or mutation queue full"));
    return completion;
}

Completion<TopicInfo> Recorder::add_topic(
    std::string_view, std::uint64_t, const TopicOptions&, const OperationId&) {
    return failed_completion<TopicInfo>(error(ErrorCode::Unavailable, "topic executor is not initialized"));
}

Completion<void> Recorder::remove_topic(
    std::string_view, std::uint64_t, TopicId, std::uint64_t, const OperationId&) {
    return failed_completion<void>(error(ErrorCode::Unavailable, "topic executor is not initialized"));
}

State Recorder::state() const {
    std::lock_guard<std::mutex> lock(implementation_->state_mutex_);
    State snapshot = implementation_->state_;
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
