// RecordingCoordinator implementation. The WriterAdapter is confined to writer_main.
#include "ros2_bag/recording_coordinator.hpp"

#include "ros2_bag/manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <utility>

#include <nlohmann/json.hpp>

namespace ros2_bag {
namespace {

std::string escape_json(const std::string& value) {
    std::string escaped;
    for (const char character : value) {
        if (character == '"' || character == '\\') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

std::string mode_name(RecordingMode mode) {
    switch (mode) {
        case RecordingMode::kNormal:
            return "normal";
        case RecordingMode::kTimed:
            return "timed";
        case RecordingMode::kRolling:
            return "rolling";
    }
    return "unknown";
}

std::uint64_t system_now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::string manifest_state_name(RecorderState state) {
    switch (state) {
        case RecorderState::kIdle:
            return "COMPLETED";
        case RecorderState::kRecording:
            return "RECORDING";
        case RecorderState::kPaused:
            return "PAUSED";
        case RecorderState::kStopping:
            return "STOPPING";
        case RecorderState::kFailed:
            return "FAILED";
    }
    return "FAILED";
}

}  // namespace

RecordingCoordinator::RecordingCoordinator(
    rclcpp::Node::SharedPtr node,
    RecorderConfig config,
    CleanupWorker::RemoveDirectory remove_directory,
    std::function<DiskSample()> disk_sampler,
    WriterAdapter::WriteOverride write_override,
    TopicManager::PersistEvent persist_event_override)
: node_(std::move(node)),
  config_(std::move(config)),
  disk_sampler_(std::move(disk_sampler)),
  persist_event_override_(std::move(persist_event_override)),
  writer_(std::move(write_override)),
  snapshot_buffer_(config_.snapshot.max_total_bytes) {
    if (!node_) throw std::invalid_argument("RecordingCoordinator requires a node");
    validate_recorder_config(&config_);
    process_started_at_ = std::chrono::steady_clock::now();
    cleanup_worker_ = std::make_unique<CleanupWorker>(
        config_.cleanup.queue_capacity,
        [this](const std::filesystem::path& path, bool success, const std::string& error) {
            complete_cleanup(path, success, error);
        },
        std::move(remove_directory));
    writer_thread_ = std::thread(&RecordingCoordinator::writer_main, this);
    resource_monitor_ = std::make_shared<ResourceMonitor>(
        config_.recording.output_root,
        config_.disk,
        config_.resource);
    if (!disk_sampler_) {
        resource_monitor_->start();
    }
    disk_timer_ = node_->create_wall_timer(
        std::chrono::seconds(config_.disk.check_interval_sec),
        [this]() { sample_disk(); });
}

RecordingCoordinator::~RecordingCoordinator() { shutdown(); }

bool RecordingCoordinator::call_writer(
    const std::function<bool(std::string*)>& operation,
    std::string* error,
    bool drain_data_before_operation) {
    const auto request = std::make_shared<WriterRequest>();
    request->operation = operation;
    request->drain_data_before_operation = drain_data_before_operation;
    std::unique_lock<std::mutex> lock(writer_mutex_);
    if (writer_shutdown_ || writer_requests_.size() >= config_.writer.control_queue_capacity) {
        if (error) *error = "writer control queue is unavailable";
        return false;
    }
    writer_requests_.push_back(request);
    writer_cv_.notify_one();
    request->completion.wait(lock, [&request]() { return request->done; });
    if (error) *error = request->error;
    return request->result;
}

bool RecordingCoordinator::persist_event(const TopicEvent& event, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int descriptor = ::open(event_log_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (descriptor < 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    const std::uint64_t sequence = ++event_sequence_;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    nlohmann::json details;
    try {
        details = nlohmann::json::parse(event.details_json);
    } catch (const nlohmann::json::exception&) {
        if (error != nullptr) {
            *error = "topic event details must be a JSON object";
        }
        ::close(descriptor);
        return false;
    }
    if (!details.is_object()) {
        if (error != nullptr) {
            *error = "topic event details must be a JSON object";
        }
        ::close(descriptor);
        return false;
    }
    const std::string line =
        "{\"schema_version\":1,\"event\":\"" + escape_json(event.event) +
        "\",\"event_seq\":" + std::to_string(sequence) + ",\"recording_id\":\"" +
        escape_json(recording_id_) + "\",\"operation_id\":\"" + escape_json(event.operation_id) +
        "\",\"topic_id\":" + std::to_string(event.topic_id) +
        ",\"topic_epoch\":" + std::to_string(event.topic_epoch) + ",\"name\":\"" +
        escape_json(event.name) + "\",\"type\":\"" + escape_json(event.type) +
        "\",\"error_code\":\"" + escape_json(event.error_code) + "\",\"error_message\":\"" +
        escape_json(event.error_message) + "\",\"details\":" + details.dump() +
        ",\"at_ns\":" + std::to_string(timestamp_ns) + "}\n";
    const ssize_t written = ::write(descriptor, line.data(), line.size());
    const int sync_result = ::fsync(descriptor);
    const int close_result = ::close(descriptor);
    if (written != static_cast<ssize_t>(line.size()) || sync_result != 0 || close_result != 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    return true;
}

bool RecordingCoordinator::start(const std::string& task_id, std::string* error) {
    if (config_.recording.mode == RecordingMode::kTimed) {
        if (error != nullptr) {
            *error = "timed mode requires a positive duration";
        }
        return false;
    }
    return start_impl(task_id, error);
}

bool RecordingCoordinator::start_impl(const std::string& task_id, std::string* error) {
    if (task_id.empty() || task_id.find('/') != std::string::npos ||
        task_id.find("..") != std::string::npos) {
        if (error) *error = "task_id must be a simple nonempty name";
        return false;
    }
    std::error_code output_root_error;
    std::filesystem::create_directories(config_.recording.output_root, output_root_error);
    if (output_root_error) {
        if (error != nullptr) {
            *error = "DISK_STATE_UNKNOWN: " + output_root_error.message();
        }
        return false;
    }
    const DiskSample initial_disk_sample =
        disk_sampler_ ? disk_sampler_() : resource_monitor_->sample();
    if (initial_disk_sample.state == DiskProtectionState::kUnknown) {
        if (error != nullptr) {
            *error = "DISK_STATE_UNKNOWN";
        }
        return false;
    }
    WriterOpenOptions options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        disk_sample_ = initial_disk_sample;
        if (state_ != RecorderState::kIdle) {
            if (error) *error = "recorder is not idle";
            return false;
        }
        task_directory_ = config_.recording.output_root / task_id;
        std::error_code filesystem_error;
        std::filesystem::create_directories(task_directory_, filesystem_error);
        if (filesystem_error) {
            if (error) *error = filesystem_error.message();
            return false;
        }
        event_log_ = task_directory_ / "topic_events.jsonl";
        paused_by_disk_ = false;
        paused_by_user_ = false;
        task_started_at_ = std::chrono::steady_clock::now();
        pause_started_at_.reset();
        paused_total_ = std::chrono::steady_clock::duration{};
        recording_id_ = task_id;
        event_sequence_ = 0;
        ingress_open_ = std::make_shared<std::atomic<bool>>(false);
        budget_ =
            std::make_shared<QueueBudget>(config_.queue.max_bytes, config_.queue.max_messages);
        queue_ = std::make_shared<MessageQueue>(
            config_.queue.max_messages,
            config_.topics.max_active_topics,
            0);
        pool_state_ = std::make_shared<PoolState>(config_.pool.pool);
        ++generation_;
        current_segment_id_ = 1;
        segments_.clear();
        cleanup_pending_.clear();
        secondary_errors_.clear();
        segments_.push_back({current_segment_id_, "segment_000000", "WRITING"});
        options.uri = (task_directory_ / "segment_000000").string();
        options.storage_id = config_.storage.storage_id;
        options.storage_config_uri = config_.storage.config_file.string();
    }
    std::string writer_error;
    if (!call_writer(
            [this, &options](std::string* result) {
                writer_.reset_statistics();
                snapshot_buffer_.clear();
                writer_failed_ = false;
                return writer_.open_segment(options, result);
            },
            &writer_error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        set_error_locked(writer_error);
        state_ = RecorderState::kFailed;
        if (error) *error = writer_error;
        return false;
    }
    if (!write_current_manifest("RECORDING", "", &writer_error)) {
        static_cast<void>(
            call_writer([this](std::string* result) { return writer_.close(result); }, nullptr));
        std::lock_guard<std::mutex> lock(mutex_);
        set_error_locked(writer_error);
        state_ = RecorderState::kFailed;
        if (error != nullptr) *error = writer_error;
        return false;
    }
    try {
        auto register_topic = [this](const TopicInfo& topic, std::string* result) {
            return call_writer(
                [this, &topic](std::string* writer_error) {
                    return writer_.register_topic(topic, writer_error);
                },
                result);
        };
        auto persist = [this](const TopicEvent& event, std::string* result) {
            if (persist_event_override_) {
                return persist_event_override_(event, result);
            }
            return persist_event(event, result);
        };
        const auto timeout = std::chrono::milliseconds(config_.topics.discovery_timeout_ms);
        topic_manager_ = std::make_shared<TopicManager>(
            node_,
            config_.topics.max_active_topics,
            config_.topics.max_distinct_topics,
            config_.topics.recent_removed_instances,
            timeout,
            config_.status.frequency_window_sec,
            config_.status.frequency_bucket_ms,
            std::move(register_topic),
            std::move(persist));
        topic_manager_->set_task_ingress(
            TaskIngress{generation_, queue_, budget_, pool_state_, ingress_open_});
    } catch (const std::exception& exception) {
        static_cast<void>(call_writer(
            [this](std::string* result) { return writer_.close(result); },
            &writer_error));
        std::lock_guard<std::mutex> lock(mutex_);
        set_error_locked(exception.what());
        state_ = RecorderState::kFailed;
        if (error) *error = exception.what();
        return false;
    }
    ingress_open_->store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = RecorderState::kRecording;
        if (config_.recording.mode == RecordingMode::kRolling) {
            rolling_timer_ = node_->create_wall_timer(
                std::chrono::seconds(config_.recording.segment_duration_sec),
                [this]() {
                    std::string ignored;
                    static_cast<void>(rotate_segment(&ignored));
                    schedule_retention_cleanup();
                });
        }
    }
    return true;
}

bool RecordingCoordinator::start_timed(
    const std::string& task_id,
    std::chrono::milliseconds timed_duration,
    std::string* error) {
    if (config_.recording.mode != RecordingMode::kTimed || timed_duration.count() <= 0) {
        if (error != nullptr) *error = "timed mode requires a positive duration";
        return false;
    }
    if (!start_impl(task_id, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    timed_stop_timer_ = node_->create_wall_timer(timed_duration, [this]() {
        std::string ignored;
        static_cast<void>(stop_on_executor(&ignored));
    });
    return true;
}

bool RecordingCoordinator::pause(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RecorderState::kRecording || !topic_manager_) {
        if (error) *error = "recorder is not recording";
        return false;
    }
    topic_manager_->set_ingress_open(false);
    paused_by_disk_ = false;
    paused_by_user_ = true;
    pause_started_at_ = std::chrono::steady_clock::now();
    state_ = RecorderState::kPaused;
    return true;
}

bool RecordingCoordinator::resume(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RecorderState::kPaused || !topic_manager_) {
        if (error) *error = "recorder is not paused";
        return false;
    }
    if (paused_by_disk_) {
        if (error) *error = "recorder is paused by disk protection";
        return false;
    }
    if (disk_sample_.state == DiskProtectionState::kUnknown) {
        if (error) *error = "DISK_STATE_UNKNOWN";
        return false;
    }
    if (disk_sample_.state == DiskProtectionState::kCritical) {
        if (error) *error = "DISK_FULL";
        return false;
    }
    topic_manager_->set_ingress_open(true);
    paused_by_user_ = false;
    if (pause_started_at_) {
        paused_total_ += std::chrono::steady_clock::now() - *pause_started_at_;
        pause_started_at_.reset();
    }
    state_ = RecorderState::kRecording;
    return true;
}

TopicOperationResult RecordingCoordinator::add_topic(
    TopicInfo topic,
    const std::string& operation_id) {
    std::shared_ptr<TopicManager> manager;
    bool armed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!topic_manager_ ||
            (state_ != RecorderState::kRecording && state_ != RecorderState::kPaused)) {
            return {false, kInvalidTopicId, 0, "INVALID_STATE", "recorder has no active task"};
        }
        manager = topic_manager_;
        armed = state_ == RecorderState::kPaused;
    }
    TopicOperationResult result = manager->add_topic(std::move(topic), operation_id, armed);
    if (!result.ok && result.error_code == "STORAGE_UNAVAILABLE") {
        fail_after_event_persistence_error(result.error_message);
    }
    return result;
}

TopicOperationResult RecordingCoordinator::add_topic_by_name(
    const std::string& name,
    const std::string& operation_id) {
    if (name.empty() || name.front() != '/') {
        return {false, kInvalidTopicId, 0, "INVALID_ARGUMENT", "topic name must be absolute"};
    }
    const auto graph_topics = node_->get_topic_names_and_types();
    const auto discovered = graph_topics.find(name);
    if (discovered == graph_topics.end()) {
        return {false, kInvalidTopicId, 0, "NOT_FOUND", "topic is not present in the ROS graph"};
    }
    if (discovered->second.size() != 1) {
        return {
            false,
            kInvalidTopicId,
            0,
            "INVALID_ARGUMENT",
            "topic must have exactly one discovered type"};
    }
    TopicInfo topic;
    topic.name = name;
    topic.type = discovered->second.front();
    return add_topic(std::move(topic), operation_id);
}

TopicOperationResult RecordingCoordinator::remove_topic(
    const std::string& name,
    const std::string& operation_id) {
    std::shared_ptr<TopicManager> manager;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        manager = topic_manager_;
    }
    if (!manager)
        return {false, kInvalidTopicId, 0, "INVALID_STATE", "recorder has no active task"};
    const TopicOperationResult result = manager->remove_topic(name, operation_id);
    if (!result.ok) {
        if (result.error_code == "STORAGE_UNAVAILABLE") {
            fail_after_event_persistence_error(result.error_message);
        }
        return result;
    }
    std::string writer_error;
    if (!call_writer(
            [this, &result](std::string*) {
                snapshot_buffer_.remove_topic(result.topic_id, result.topic_epoch);
                return true;
            },
            &writer_error,
            true)) {
        std::lock_guard<std::mutex> lock(mutex_);
        set_error_locked(writer_error);
        state_ = RecorderState::kFailed;
        return {false, result.topic_id, result.topic_epoch, "WRITER_UNAVAILABLE", writer_error};
    }
    return result;
}

void RecordingCoordinator::fail_after_event_persistence_error(const std::string& error) {
    std::string stop_error;
    static_cast<void>(stop(&stop_error));
    std::lock_guard<std::mutex> lock(mutex_);
    set_error_locked(error.empty() ? "topic event persistence failed" : error);
    state_ = RecorderState::kFailed;
}

bool RecordingCoordinator::stop(std::string* error) { return stop_impl(error, false); }

bool RecordingCoordinator::stop_on_executor(std::string* error) { return stop_impl(error, true); }

bool RecordingCoordinator::stop_impl(std::string* error, bool called_from_executor) {
    std::shared_ptr<TopicManager> manager;
    std::shared_ptr<std::atomic<bool>> open;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != RecorderState::kRecording && state_ != RecorderState::kPaused &&
            state_ != RecorderState::kFailed)
            return true;
        state_ = RecorderState::kStopping;
        if (timed_stop_timer_) {
            timed_stop_timer_->cancel();
            timed_stop_timer_.reset();
        }
        if (rolling_timer_) {
            rolling_timer_->cancel();
            rolling_timer_.reset();
        }
        paused_by_disk_ = false;
        paused_by_user_ = false;
        if (pause_started_at_) {
            paused_total_ += std::chrono::steady_clock::now() - *pause_started_at_;
            pause_started_at_.reset();
        }
        open = ingress_open_;
        manager = std::move(topic_manager_);
    }
    if (open) open->store(false);
    if (manager) {
        if (called_from_executor) {
            manager->clear_task_on_executor();
        } else {
            manager->clear_task();
        }
    }
    std::string writer_error;
    const bool closed = call_writer(
        [this](std::string* result) { return writer_.close(result); },
        &writer_error,
        true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!segments_.empty()) {
            segments_.back().state = closed ? "COMPLETE" : "INCOMPLETE";
            if (closed) {
                segments_.back().completed_at_ns = system_now_ns();
            }
        }
    }
    std::string manifest_error;
    const bool manifest_written = write_current_manifest(
        closed ? "COMPLETED" : "FAILED",
        closed ? "" : writer_error,
        &manifest_error);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closed) set_error_locked(writer_error);
    if (!manifest_written) set_error_locked(manifest_error);
    queue_.reset();
    budget_.reset();
    pool_state_.reset();
    ingress_open_.reset();
    state_ = closed && manifest_written ? RecorderState::kIdle : RecorderState::kFailed;
    if ((!closed || !manifest_written) && error) *error = closed ? manifest_error : writer_error;
    return closed && manifest_written;
}

RecorderStatus RecordingCoordinator::status() const {
    RecorderStatus result;
    std::shared_ptr<QueueBudget> budget;
    std::shared_ptr<PoolState> pool_state;
    std::shared_ptr<TopicManager> topic_manager;
    std::shared_ptr<ResourceMonitor> resource_monitor;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        result.state = state_;
        result.recording_generation = generation_;
        result.task_directory = task_directory_;
        result.recording_id = recording_id_;
        result.mode = config_.recording.mode;
        result.last_error = last_error_;
        result.secondary_errors = secondary_errors_;
        result.disk = disk_sample_;
        result.paused_by_disk = paused_by_disk_;
        const auto now = std::chrono::steady_clock::now();
        result.uptime_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - process_started_at_)
                .count());
        if (task_started_at_) {
            const std::chrono::steady_clock::duration current_pause =
                pause_started_at_ ? now - *pause_started_at_
                                  : std::chrono::steady_clock::duration{};
            const std::chrono::steady_clock::duration wall = now - *task_started_at_;
            const std::chrono::steady_clock::duration paused = paused_total_ + current_pause;
            result.elapsed_wall_sec = std::chrono::duration<double>(wall).count();
            result.paused_total_sec = std::chrono::duration<double>(paused).count();
            result.elapsed_active_sec = std::chrono::duration<double>(wall - paused).count();
        }
        if (paused_by_user_) {
            result.paused_reason = "USER";
        } else if (paused_by_disk_) {
            result.paused_reason = disk_sample_.state == DiskProtectionState::kUnknown
                                       ? "SYSTEM_UNKNOWN"
                                       : "SYSTEM_DISK";
        }
        budget = budget_;
        pool_state = pool_state_;
        topic_manager = topic_manager_;
        resource_monitor = resource_monitor_;
    }
    if (budget) result.queue = budget->statistics();
    if (pool_state) result.pool = pool_state->statistics();
    if (topic_manager) {
        result.topics = topic_manager->snapshots();
        result.topics_removed = topic_manager->removed_snapshots();
        result.topics_removed_total = topic_manager->removed_total();
    }
    if (resource_monitor) result.process = resource_monitor->latest_process();
    const std::vector<WriterTopicStatistics> writer_statistics = writer_.statistics();
    for (TopicSnapshot& topic : result.topics) {
        for (const WriterTopicStatistics& writer_topic : writer_statistics) {
            if (writer_topic.topic_id == topic.topic_info.id &&
                writer_topic.topic_epoch == topic.topic_epoch) {
                topic.counters.written = writer_topic.written;
                topic.counters.write_errors = writer_topic.write_errors;
                break;
            }
        }
    }
    return result;
}

void RecordingCoordinator::set_error_locked(const std::string& error) { last_error_ = error; }

void RecordingCoordinator::record_secondary_error_locked(const std::string& error) {
    constexpr std::size_t kMaximumSecondaryErrors = 8;
    if (error.empty()) {
        return;
    }
    secondary_errors_.push_back(error);
    if (secondary_errors_.size() > kMaximumSecondaryErrors) {
        secondary_errors_.erase(secondary_errors_.begin());
    }
}

void RecordingCoordinator::record_runtime_diagnostic(const std::string& diagnostic) {
    const std::lock_guard<std::mutex> lock(mutex_);
    record_secondary_error_locked(diagnostic);
}

bool RecordingCoordinator::write_current_manifest(
    const std::string& recording_state,
    const std::string& last_error,
    std::string* error) {
    RecordingManifest manifest;
    std::filesystem::path manifest_path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        manifest.recording_id = recording_id_;
        manifest.mode = mode_name(config_.recording.mode);
        manifest.recording_state = recording_state;
        manifest.last_error = last_error;
        manifest.secondary_errors = secondary_errors_;
        manifest.segments = segments_;
        manifest.cleanup_pending = cleanup_pending_;
        manifest_path = task_directory_ / "manifest.json";
    }
    return write_manifest_atomic(manifest_path, manifest, error);
}

void RecordingCoordinator::schedule_retention_cleanup() {
    if (config_.recording.mode != RecordingMode::kRolling || !cleanup_worker_) {
        return;
    }
    const std::uint64_t now = system_now_ns();
    const std::uint64_t retention_ns =
        static_cast<std::uint64_t>(config_.recording.retention_duration_sec) * 1000000000ULL;
    std::vector<std::string> scheduled;
    std::string recording_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (ManifestSegment& segment : segments_) {
            if (segment.state != "COMPLETE" || segment.completed_at_ns == 0 ||
                now - segment.completed_at_ns < retention_ns) {
                continue;
            }
            segment.state = "DELETING";
            cleanup_pending_.push_back(segment.path);
            scheduled.push_back(segment.path);
        }
        recording_state = manifest_state_name(state_);
    }
    if (scheduled.empty()) {
        return;
    }
    std::string ignored;
    if (!write_current_manifest(recording_state, "", &ignored)) {
        return;
    }
    for (const std::string& path : scheduled) {
        static_cast<void>(cleanup_worker_->enqueue(task_directory_ / path));
    }
}

void RecordingCoordinator::complete_cleanup(
    const std::filesystem::path& path,
    bool success,
    const std::string& error) {
    std::string state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string name = path.filename().string();
        for (ManifestSegment& segment : segments_) {
            if (segment.path == name) {
                segment.state = success ? "REMOVED" : "DELETING";
                break;
            }
        }
        cleanup_pending_.erase(
            std::remove(cleanup_pending_.begin(), cleanup_pending_.end(), name),
            cleanup_pending_.end());
        if (!success) {
            record_secondary_error_locked("cleanup " + name + ": " + error);
        }
        state = manifest_state_name(state_);
    }
    std::string ignored;
    static_cast<void>(write_current_manifest(state, "", &ignored));
}

void RecordingCoordinator::record_cleanup_timeout() {
    if (!cleanup_worker_) {
        return;
    }
    const std::optional<std::filesystem::path> path = cleanup_worker_->take_timed_out_path(
        std::chrono::seconds(config_.cleanup.warning_timeout_sec));
    if (!path) {
        return;
    }
    std::string recording_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        record_secondary_error_locked("CLEANUP_TIMEOUT: " + path->filename().string());
        recording_state = manifest_state_name(state_);
    }
    std::string ignored;
    static_cast<void>(write_current_manifest(recording_state, "", &ignored));
}

bool RecordingCoordinator::rotate_segment(std::string* error) {
    std::shared_ptr<TopicManager> manager;
    std::shared_ptr<std::atomic<bool>> ingress_open;
    std::uint64_t next_segment_id = 0;
    std::filesystem::path next_segment_path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_.recording.mode != RecordingMode::kRolling ||
            state_ != RecorderState::kRecording || !topic_manager_) {
            return true;
        }
        manager = topic_manager_;
        ingress_open = ingress_open_;
        next_segment_id = current_segment_id_ + 1;
        std::ostringstream stream;
        stream << "segment_" << std::setfill('0') << std::setw(6) << next_segment_id - 1;
        next_segment_path = task_directory_ / stream.str();
    }
    if (!manager->close_ingress_and_drain()) {
        if (error != nullptr) {
            *error = "topic producer drain timed out during segment rotation";
        }
        return false;
    }
    const std::vector<TopicSnapshot> topics = manager->snapshots();
    WriterOpenOptions options;
    options.uri = next_segment_path.string();
    options.storage_id = config_.storage.storage_id;
    options.storage_config_uri = config_.storage.config_file.string();
    std::string writer_error;
    const bool rotated = call_writer(
        [this, &options, &topics](std::string* result) {
            if (!writer_.close(result) || !writer_.open_segment(options, result)) {
                return false;
            }
            for (const TopicSnapshot& topic : topics) {
                if (!writer_.register_topic(topic.topic_info, result)) {
                    return false;
                }
            }
            snapshot_buffer_.retain_active_topics(topics);
            return snapshot_buffer_.replay([this, result](const RecordedMessage& message) {
                return writer_.write(message, result);
            });
        },
        &writer_error,
        true);
    if (!rotated) {
        std::lock_guard<std::mutex> lock(mutex_);
        set_error_locked(writer_error);
        state_ = RecorderState::kFailed;
        if (error != nullptr) {
            *error = writer_error;
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!segments_.empty()) {
            segments_.back().state = "COMPLETE";
            segments_.back().completed_at_ns = system_now_ns();
        }
        current_segment_id_ = next_segment_id;
        segments_.push_back(
            {current_segment_id_, next_segment_path.filename().string(), "WRITING"});
    }
    std::string manifest_error;
    if (!write_current_manifest("RECORDING", "", &manifest_error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        set_error_locked(manifest_error);
        state_ = RecorderState::kFailed;
        if (error != nullptr) {
            *error = manifest_error;
        }
        return false;
    }
    schedule_retention_cleanup();
    if (ingress_open) {
        ingress_open->store(true);
    }
    manager->set_ingress_open(true);
    return true;
}

void RecordingCoordinator::sample_disk() {
    record_cleanup_timeout();
    std::shared_ptr<TopicManager> manager;
    bool close_ingress = false;
    bool open_ingress = false;
    std::shared_ptr<ResourceMonitor> monitor;
    if (!disk_sampler_) {
        std::lock_guard<std::mutex> lock(mutex_);
        monitor = resource_monitor_;
        if (monitor == nullptr) {
            return;
        }
    }
    const DiskSample sample = disk_sampler_ ? disk_sampler_() : monitor->latest();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        disk_sample_ = sample;
        manager = topic_manager_;
        if (!manager) {
            return;
        }
        const bool unsafe = disk_sample_.state == DiskProtectionState::kCritical ||
                            disk_sample_.state == DiskProtectionState::kUnknown;
        if (unsafe && state_ == RecorderState::kRecording) {
            paused_by_disk_ = true;
            paused_by_user_ = false;
            pause_started_at_ = std::chrono::steady_clock::now();
            disk_safe_since_ = std::chrono::steady_clock::time_point{};
            state_ = RecorderState::kPaused;
            close_ingress = true;
        } else if (
            !unsafe && state_ == RecorderState::kPaused && paused_by_disk_ &&
            config_.disk.auto_resume) {
            const std::uint64_t minimum_free =
                config_.disk.min_free_gb * 1000ULL * 1000ULL * 1000ULL;
            const bool safe = disk_sample_.used_percent <= config_.disk.resume_percent &&
                              disk_sample_.available_bytes >= minimum_free;
            if (!safe) {
                disk_safe_since_ = std::chrono::steady_clock::time_point{};
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (disk_safe_since_ == std::chrono::steady_clock::time_point{}) {
                    disk_safe_since_ = now;
                }
                if (now - disk_safe_since_ >=
                    std::chrono::seconds(config_.disk.resume_stable_sec)) {
                    paused_by_disk_ = false;
                    paused_by_user_ = false;
                    if (pause_started_at_) {
                        paused_total_ += now - *pause_started_at_;
                        pause_started_at_.reset();
                    }
                    state_ = RecorderState::kRecording;
                    open_ingress = true;
                }
            }
        }
    }
    if (close_ingress) {
        manager->set_ingress_open(false);
    }
    if (open_ingress) {
        manager->set_ingress_open(true);
    }
}

bool RecordingCoordinator::drain_writer_queue(std::string* error) {
    std::shared_ptr<MessageQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue = queue_;
    }
    QueuedMessage message;
    while (queue && queue->try_dequeue(message)) {
        if (writer_failed_) {
            message = QueuedMessage{};
            continue;
        }
        if (message.message && !write_message(*message.message, error)) {
            return false;
        }
        message = QueuedMessage{};
    }
    return true;
}

void RecordingCoordinator::fail_recording_from_writer(const std::string& error) {
    std::shared_ptr<std::atomic<bool>> ingress_open;
    std::shared_ptr<TopicManager> manager;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_failed_ = true;
        set_error_locked(error);
        state_ = RecorderState::kFailed;
        ingress_open = ingress_open_;
        manager = topic_manager_;
    }
    if (ingress_open) {
        ingress_open->store(false);
    }
    if (manager) {
        manager->set_ingress_open(false);
    }
}

bool RecordingCoordinator::write_message(const RecordedMessage& message, std::string* error) {
    std::shared_ptr<TopicManager> manager;
    bool stale_generation = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stale_generation = message.recording_generation != generation_;
        manager = topic_manager_;
    }
    if (stale_generation) {
        if (manager) {
            manager->record_generation_violation(message.topic_id, message.topic_epoch);
        }
        constexpr char kGenerationViolation[] = "GENERATION_VIOLATION";
        fail_recording_from_writer(kGenerationViolation);
        if (error != nullptr) {
            *error = kGenerationViolation;
        }
        return false;
    }
    if (!writer_.write(message, error)) {
        fail_recording_from_writer(error != nullptr ? *error : "WriterAdapter write failed");
        return false;
    }
    if (!message.is_replayed) {
        snapshot_buffer_.add(message);
    }
    return true;
}

void RecordingCoordinator::shutdown() {
    static_cast<void>(stop(nullptr));
    if (resource_monitor_) {
        resource_monitor_->stop();
    }
    if (cleanup_worker_) {
        cleanup_worker_->shutdown();
    }
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        if (writer_shutdown_) return;
        writer_shutdown_ = true;
    }
    writer_cv_.notify_one();
    if (writer_thread_.joinable()) writer_thread_.join();
}

void RecordingCoordinator::writer_main() {
    for (;;) {
        std::shared_ptr<WriterRequest> request;
        {
            std::unique_lock<std::mutex> lock(writer_mutex_);
            writer_cv_.wait_for(
                lock,
                std::chrono::milliseconds(config_.queue.wait_timeout_ms),
                [this]() { return writer_shutdown_ || !writer_requests_.empty(); });
            if (writer_shutdown_ && writer_requests_.empty()) return;
            if (!writer_requests_.empty()) {
                request = writer_requests_.front();
                writer_requests_.pop_front();
            }
        }
        if (request) {
            std::string error;
            bool result = true;
            if (request->drain_data_before_operation) {
                result = drain_writer_queue(&error);
            }
            if (result) {
                result = request->operation(&error);
            }
            {
                std::lock_guard<std::mutex> lock(writer_mutex_);
                request->result = result;
                request->error = error;
                request->done = true;
            }
            request->completion.notify_one();
        }
        std::shared_ptr<MessageQueue> queue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue = queue_;
        }
        if (!queue || !writer_.is_open()) continue;
        QueuedMessage message;
        for (std::size_t index = 0; index < config_.queue.batch_size && queue->try_dequeue(message);
             ++index) {
            if (writer_failed_) {
                message = QueuedMessage{};
                continue;
            }
            std::string error;
            if (message.message) {
                static_cast<void>(write_message(*message.message, &error));
            }
            message = QueuedMessage{};
        }
    }
}

}  // namespace ros2_bag
