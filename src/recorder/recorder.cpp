#include "ros2_bag/recorder/recorder.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <system_error>
#include <new>
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

template <class T>
Result<T, Error> completed_result(
    const Completion<T>& completion, std::chrono::milliseconds timeout) {
    // A timed wait is diagnostic only. A native Writer operation that has started
    // owns Recorder resources until it really completes; synthesizing an Error
    // here would let Stop/Shutdown report Done while that work is still live.
    while (!completion.wait_for(timeout)) {
    }
    const auto result = completion.result();
    if (!result) {
        return Result<T, Error>(
            unexpect, error(ErrorCode::InternalError, "writer completion disappeared"));
    }
    return *result;
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

        struct Producer {
            explicit Producer(Ingress& ingress) : ingress(ingress) {}
            ~Producer() {
                std::lock_guard<std::mutex> lock(ingress.mutex);
                --ingress.producers;
                ingress.condition.notify_all();
            }
            Producer(const Producer&) = delete;
            Producer& operator=(const Producer&) = delete;
            Ingress& ingress;  // borrowed from the callback's shared Ingress owner
        };
    };

    struct ActiveTopic {
        struct Counters {
            struct FrequencyBucket {
                std::int64_t bucket_id = std::numeric_limits<std::int64_t>::min();
                std::uint64_t count = 0;
            };

            struct FrequencySample {
                double actual_hz = 0.0;
                TopicState::Gap gap = TopicState::Gap::Unknown;
            };

            static constexpr std::int64_t kFrequencyWindowNs = 1000000000LL;
            static constexpr std::int64_t kFrequencyBucketNs = 10000000LL;
            static constexpr std::size_t kFrequencyBucketCount = 202;

            std::atomic<std::uint64_t> received{0};
            std::shared_ptr<std::atomic<std::uint64_t>> written{
                std::make_shared<std::atomic<std::uint64_t>>(0)};
            std::atomic<std::uint64_t> dropped{0};
            std::atomic<std::size_t> publisher_count{0};
            std::atomic<std::int64_t> publisher_observed_steady_ns{0};
            std::atomic<TopicState::Compatibility> qos_compatible{
                TopicState::Compatibility::Unknown};
            mutable std::mutex frequency_mutex;
            std::array<FrequencyBucket, kFrequencyBucketCount> frequency_buckets{};
            std::int64_t first_arrival_bucket = std::numeric_limits<std::int64_t>::min();

            void record_arrival(std::int64_t steady_ns) {
                const auto bucket_id = steady_ns / kFrequencyBucketNs;
                std::lock_guard<std::mutex> lock(frequency_mutex);
                auto& bucket = frequency_buckets[static_cast<std::size_t>(
                    bucket_id % static_cast<std::int64_t>(kFrequencyBucketCount))];
                if (bucket.bucket_id != bucket_id) {
                    bucket.bucket_id = bucket_id;
                    bucket.count = 0;
                }
                if (bucket.count != std::numeric_limits<std::uint64_t>::max()) {
                    ++bucket.count;
                }
                if (first_arrival_bucket == std::numeric_limits<std::int64_t>::min()) {
                    first_arrival_bucket = bucket_id;
                }
            }

            FrequencySample frequency(
                std::int64_t steady_ns, std::optional<double> expected_hz,
                bool monitoring_valid) const {
                FrequencySample sample;
                const auto bucket_id = steady_ns / kFrequencyBucketNs;
                const auto buckets_per_window =
                    static_cast<std::int64_t>(kFrequencyWindowNs / kFrequencyBucketNs);
                const auto current_start = bucket_id - buckets_per_window + 1;
                std::lock_guard<std::mutex> lock(frequency_mutex);
                if (first_arrival_bucket > current_start) {
                    return sample;
                }
                const auto count_window = [this](std::int64_t start, std::int64_t end) {
                    std::uint64_t total = 0;
                    for (auto index = start; index <= end; ++index) {
                        const auto& bucket = frequency_buckets[static_cast<std::size_t>(
                            index % static_cast<std::int64_t>(kFrequencyBucketCount))];
                        if (bucket.bucket_id != index) {
                            continue;
                        }
                        if (total > std::numeric_limits<std::uint64_t>::max() - bucket.count) {
                            return std::numeric_limits<std::uint64_t>::max();
                        }
                        total += bucket.count;
                    }
                    return total;
                };
                const auto current_count = count_window(current_start, bucket_id);
                sample.actual_hz = static_cast<double>(current_count);
                if (!expected_hz || !monitoring_valid) {
                    return sample;
                }
                const auto previous_start = current_start - buckets_per_window;
                if (first_arrival_bucket > previous_start) {
                    sample.gap = TopicState::Gap::Normal;
                    return sample;
                }
                const auto previous_count = count_window(previous_start, current_start - 1);
                sample.gap = TopicState::Gap::Normal;
                if (current_count < *expected_hz * 0.5 && previous_count < *expected_hz * 0.5) {
                    sample.gap = TopicState::Gap::Detected;
                }
                return sample;
            }
        };

        TopicInfo info;
        std::shared_ptr<rclcpp::GenericSubscription> subscription;
        std::shared_ptr<Ingress> ingress;
        std::shared_ptr<AdmissionGate> admission;
        std::shared_ptr<const TopicInfo> frozen_info;
        std::shared_ptr<Counters> counters;
    };

    struct PublisherVisibility {
        TopicId topic_id = 0;
        std::size_t publisher_count = 0;
        std::int64_t observed_steady_ns = 0;
        TopicState::Compatibility compatibility = TopicState::Compatibility::Unknown;
    };

    struct ProcessBaseline {
        std::int64_t cpu_ns = 0;
        std::chrono::steady_clock::time_point sampled_at;
    };

    static constexpr std::size_t kMaxRecordingIdLength = 64;

    struct PublisherQosSnapshot {
        std::size_t count = 0;
        std::vector<QosOptions> profiles;
        bool truncated = false;
    };

    struct TopicSignature {
        std::string type;
        std::string serialization_format;
        std::string schema_encoding;
        std::string schema_sha256;

        bool matches(const TopicSignature& other) const noexcept {
            if (type != other.type) {
                return false;
            }
            if (serialization_format != other.serialization_format) {
                return false;
            }
            if (schema_encoding != other.schema_encoding) {
                return false;
            }
            return schema_sha256 == other.schema_sha256;
        }
    };

    struct RecoveryEvidence {
        bool present = false;
        bool findings_available = false;
    };

    static bool valid_recording_id(std::string_view recording_id) {
        if (recording_id.empty() || recording_id.size() > kMaxRecordingIdLength) {
            return false;
        }
        const auto initial = static_cast<unsigned char>(recording_id.front());
        const auto ascii_alnum = [](unsigned char character) {
            return (character >= 'A' && character <= 'Z') ||
                   (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        };
        if (!ascii_alnum(initial)) {
            return false;
        }
        return std::all_of(
            recording_id.begin() + 1, recording_id.end(), [ascii_alnum](unsigned char character) {
                return ascii_alnum(character) || character == '_' || character == '-';
            });
    }

    static rclcpp::QoS make_qos(const QosOptions& options) {
        const auto& explicit_qos = std::get<QosOptions::Explicit>(options.mode);
        rclcpp::QoS qos{rclcpp::KeepAll()};
        if (explicit_qos.history == History::KeepLast) {
            qos = rclcpp::QoS(rclcpp::KeepLast(explicit_qos.depth));
        }
        if (explicit_qos.reliability == Reliability::Reliable) {
            qos.reliable();
        } else {
            qos.best_effort();
        }
        if (explicit_qos.durability == Durability::TransientLocal) {
            qos.transient_local();
        } else {
            qos.durability_volatile();
        }
        return qos;
    }

    PublisherQosSnapshot publisher_qos_snapshot(
        const std::string& topic, std::size_t maximum_profiles) const {
        PublisherQosSnapshot snapshot;
        if (!node_) {
            return snapshot;
        }

        const auto endpoints = node_->get_publishers_info_by_topic(topic);
        snapshot.count = endpoints.size();
        snapshot.profiles.reserve(std::min(maximum_profiles, endpoints.size()));
        for (const auto& endpoint : endpoints) {
            if (snapshot.profiles.size() == maximum_profiles) {
                snapshot.truncated = true;
                break;
            }

            const auto endpoint_qos = endpoint.qos_profile();
            const auto& profile = endpoint_qos.get_rmw_qos_profile();
            QosOptions::Explicit explicit_qos;

            switch (profile.reliability) {
                case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
                    explicit_qos.reliability = Reliability::Reliable;
                    break;
                case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
                    explicit_qos.reliability = Reliability::BestEffort;
                    break;
                default:
                    snapshot.truncated = true;
                    continue;
            }

            switch (profile.durability) {
                case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL:
                    explicit_qos.durability = Durability::TransientLocal;
                    break;
                case RMW_QOS_POLICY_DURABILITY_VOLATILE:
                    explicit_qos.durability = Durability::Volatile;
                    break;
                default:
                    snapshot.truncated = true;
                    continue;
            }

            switch (profile.history) {
                case RMW_QOS_POLICY_HISTORY_KEEP_LAST:
                    if (profile.depth == 0 || profile.depth > kMaxQosDepth) {
                        snapshot.truncated = true;
                        continue;
                    }
                    explicit_qos.history = History::KeepLast;
                    explicit_qos.depth = static_cast<std::uint32_t>(profile.depth);
                    break;
                case RMW_QOS_POLICY_HISTORY_KEEP_ALL:
                    explicit_qos.history = History::KeepAll;
                    explicit_qos.depth = 0;
                    break;
                default:
                    snapshot.truncated = true;
                    continue;
            }

            QosOptions qos;
            qos.mode = explicit_qos;
            snapshot.profiles.push_back(std::move(qos));
        }
        return snapshot;
    }

    static Result<RecoveryEvidence, Error> read_recovery_evidence(
        const std::filesystem::path& directory, const std::string& recording_id) {
        const auto path = directory / "recovery.json";
        std::error_code filesystem_error;
        const auto status = std::filesystem::symlink_status(path, filesystem_error);
        if (filesystem_error == std::errc::no_such_file_or_directory ||
            !std::filesystem::exists(status)) {
            return Result<RecoveryEvidence, Error>(RecoveryEvidence{});
        }
        if (filesystem_error || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status)) {
            return Result<RecoveryEvidence, Error>(
                unexpect,
                error(ErrorCode::StorageError, "recovery evidence is not a safe regular file"));
        }

        std::ifstream input(path);
        if (!input) {
            return Result<RecoveryEvidence, Error>(
                unexpect, error(ErrorCode::StorageError, "cannot open recovery evidence"));
        }

        try {
            const auto document = nlohmann::json::parse(input);
            if (document.at("schema_version").get<int>() != 1 ||
                document.at("recording_id").get<std::string>() != recording_id ||
                !document.at("findings").is_array()) {
                return Result<RecoveryEvidence, Error>(
                    unexpect, error(ErrorCode::StorageError, "invalid recovery evidence"));
            }
            for (const auto& finding : document.at("findings")) {
                if (!finding.contains("code") || !finding.at("code").is_string() ||
                    !finding.contains("message") || !finding.at("message").is_string()) {
                    return Result<RecoveryEvidence, Error>(
                        unexpect, error(ErrorCode::StorageError, "invalid recovery finding"));
                }
            }
            return Result<RecoveryEvidence, Error>(
                RecoveryEvidence{true, !document.at("findings").empty()});
        } catch (const std::exception& exception) {
            return Result<RecoveryEvidence, Error>(
                unexpect, error(ErrorCode::StorageError, exception.what()));
        }
    }

    static bool disk_state_is_safe(DiskSafetyState state) noexcept {
        return state == DiskSafetyState::Normal || state == DiskSafetyState::Warning;
    }

    static bool disk_snapshot_is_fresh_and_safe(
        const std::optional<DiskAdmissionSnapshot>& snapshot, std::int64_t now_steady_ns) noexcept {
        if (!snapshot) {
            return false;
        }
        if (now_steady_ns > snapshot->valid_until_steady_ns) {
            return false;
        }
        return disk_state_is_safe(snapshot->state);
    }

    DiskMonitorConfig disk_config() const {
        DiskMonitorConfig result;
        result.path = config_.output_root;
        result.max_age = config_.disk.max_age;
        result.failure_threshold = config_.disk.failure_threshold;
        result.warning_percent = config_.disk.warning_percent;
        result.pause_percent = config_.disk.pause_percent;
        result.min_free_bytes = config_.disk.min_free_bytes;
        return result;
    }

    bool disk_recovery_confirmed(const DiskSample& sample, bool snapshot_fresh_and_safe) const {
        if (sample.state == DiskSafetyState::Unknown) {
            return false;
        }
        if (!disk_state_is_safe(sample.state)) {
            return false;
        }
        if (!snapshot_fresh_and_safe) {
            return false;
        }
        if (disk_safe_since_ == std::chrono::steady_clock::time_point{}) {
            return false;
        }
        return std::chrono::steady_clock::now() - disk_safe_since_ >= config_.disk.stable;
    }

    Implementation(RecorderConfig config, rclcpp::Node::SharedPtr node)
    : config_(std::move(config)),
      node_(std::move(node)),
      shutdown_completion_(shutdown_source_.completion()),
      cleanup_results_(config_.cleanup.queue_capacity) {
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
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    template <class Function>
    bool submit(Function&& function) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= config_.operation_queue_capacity || stopping_worker_) {
            return false;
        }
        queue_.emplace_back(std::forward<Function>(function));
        queue_condition_.notify_one();
        return true;
    }

    enum class SystemEvent : std::size_t { WriterFailure, FirstLive, DiskSample };

    // Each source has one outstanding/coalescible result. External admission
    // cannot consume these reserved slots, and publication never waits on mutation.
    template <class Function>
    bool submit_critical(SystemEvent kind, Function&& function) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopping_worker_) {
            return false;
        }
        system_events_[static_cast<std::size_t>(kind)] = std::forward<Function>(function);
        queue_condition_.notify_one();
        return true;
    }

    void submit_cleanup_result(DeleteResult result) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto& slot : cleanup_results_) {
            if (!slot) {
                slot.emplace(std::move(result));
                queue_condition_.notify_one();
                return;
            }
        }
        // enqueue_cleanup holds a credit until mutation consumes its result.
        std::terminate();  // A full result channel would violate credit conservation.
    }

    // Mutation domain only, after the cleanup worker has joined.
    void drain_cleanup_results() {
        for (auto& slot : cleanup_results_) {
            std::optional<DeleteResult> result;
            {
                std::lock_guard<std::mutex> queue_lock(queue_mutex_);
                result = std::move(slot);
                slot.reset();
            }
            if (result) {
                apply_cleanup_result(std::move(*result));
            }
        }
    }

    void apply_cleanup_result(DeleteResult result) {
        --cleanup_outstanding_;
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (poisoned_) {
            return;
        }
        const bool current =
            manifest_ && manifest_->data().task.recording_id == result.work.recording_id;
        if (!result.result) {
            if (current && cleanup_) {
                state_.cleanup.last_error = result.result.error();
                cleanup_retry_.push_back(std::move(result.work));
                retention_deadline_steady_ns_.store(
                    steady_now_ns() + std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          config_.cleanup.retry_interval)
                                          .count());
            }
            return;
        }
        // Results remain meaningful after Stop/Start. Reload the historical
        // authority in mutation rather than silently discarding a completed delete.
        std::optional<Manifest> historical;
        Manifest* target = nullptr;
        if (current) {
            target = &*manifest_;
        } else {
            lock.unlock();
            const auto directory = config_.output_root / result.work.recording_id;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(directory))) {
                return;
            }
            auto loaded = Manifest::load(directory);
            if (!loaded) {
                return;
            }
            historical.emplace(std::move(loaded.value()));
            target = &*historical;
            lock.lock();
        }
        if (target->poisoned()) {
            return;
        }
        auto updated = target->data();
        bool found = false;
        for (auto& segment : updated.segments) {
            if (segment.segment_id != result.work.segment_id) {
                continue;
            }
            if (target->task_directory() / segment.relative_path != result.work.path) {
                state_.cleanup.last_error =
                    error(ErrorCode::InternalError, "cleanup path identity conflict");
                return;
            }
            if (segment.state == SegmentLifecycleState::Removed) {
                return;
            }
            if (segment.state != SegmentLifecycleState::Deleting) {
                state_.cleanup.last_error =
                    error(ErrorCode::InternalError, "cleanup lifecycle conflict");
                return;
            }
            segment.state = SegmentLifecycleState::Removed;
            found = true;
            break;
        }
        if (!found) {
            return;
        }
        if (updated.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            if (current) {
                state_.last_error = error(
                    ErrorCode::StorageError, "manifest revision exhausted",
                    Effects::ConfirmedPartial);
                state_.recorder_state = RecorderState::Failed;
                poisoned_ = true;
            }
            return;
        }
        ++updated.manifest_revision;
        lock.unlock();
        const auto committed = target->commit(std::move(updated));
        lock.lock();
        if (!committed) {
            state_.cleanup.last_error = committed.error();
            if (current) {
                state_.last_error = committed.error();
                state_.recorder_state = RecorderState::Failed;
                poisoned_ = target->poisoned();
            } else if (target->poisoned()) {
                poisoned_ = true;
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = committed.error();
            }
        } else if (current) {
            completed_segment_steady_.erase(result.work.segment_id);
        }
    }

    bool enqueue_cleanup(const DeleteWork& work) {
        if (cleanup_outstanding_ == cleanup_results_.size() || !cleanup_->enqueue(work)) {
            return false;
        }
        ++cleanup_outstanding_;
        return true;
    }

    static std::int64_t steady_now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool target_is_initializing(std::string_view id, std::uint64_t generation) const {
        const auto initializing = initializing_generation_.load();
        if (initializing == 0 || initializing != generation) {
            return false;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_.recorder_state == RecorderState::Initializing && state_.task &&
               state_.task->recording_id == id && state_.task->generation == generation;
    }

    bool persistence_poisoned() const {
        return poisoned_ || (manifest_ && (manifest_->poisoned() ||
                                           manifest_->data().manifest_revision ==
                                               std::numeric_limits<std::uint64_t>::max()));
    }

    void close_ingress_locked() {
        for (auto& entry : active_topics_) {
            entry.second.admission->close();
            std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
            entry.second.ingress->open = false;
        }
        for (auto& topic : state_.topics) {
            topic.state = TopicRuntimeState::Armed;
        }
    }

    void fail_writer(std::uint64_t generation, Error failure) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!state_.task || state_.task->generation != generation) {
            return;
        }
        close_ingress_locked();
        state_.recorder_state = RecorderState::Failed;
        if (!state_.last_error) {
            state_.last_error = std::move(failure);
        }
    }

    void apply_disk_safety() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!state_.task || (state_.recorder_state != RecorderState::Recording &&
                             state_.recorder_state != RecorderState::Paused)) {
            return;
        }
        const auto snapshot = disk_admission_.read();
        if (disk_snapshot_is_fresh_and_safe(snapshot, steady_now_ns())) {
            // A producer may have closed its gate just before a fresh sample.
            // Retain the protection until the normal recovery path confirms it.
            state_.pause.disk = DiskPauseReason::RecoveryPending;
        } else {
            state_.pause.disk = snapshot && snapshot->state == DiskSafetyState::Critical
                                    ? DiskPauseReason::Critical
                                    : DiskPauseReason::Unknown;
        }
        disk_safe_since_ = {};
        close_ingress_locked();
        state_.recorder_state = RecorderState::Paused;
    }

    // Called only by mutation, after the operation's stack has unwound. Resources
    // remain owned through real producer/dispatch/native completion, including
    // failures during partially constructed Start.
    void terminalize_failure() {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (state_.recorder_state != RecorderState::Failed || failure_settled_) {
            return;
        }
        initializing_generation_.store(0);
        if (manifest_ && state_.last_error && state_.last_error->effects == Effects::None) {
            state_.last_error->effects = Effects::ConfirmedPartial;
        }
        close_ingress_locked();
        auto topics = std::move(active_topics_);
        const auto writer = writer_;
        auto* cleanup = cleanup_.get();
        lock.unlock();
        for (auto& entry : topics) {
            std::unique_lock<std::mutex> ingress_lock(entry.second.ingress->mutex);
            entry.second.ingress->condition.wait(
                ingress_lock, [&entry] { return entry.second.ingress->producers == 0; });
        }
        topics.clear();
        resource_monitor_.reset();
        stop_dispatch_thread();
        std::optional<Error> secondary;
        if (writer) {
            const auto closed = writer->close();
            if (!closed) {
                secondary = closed.error();
            }
        }
        if (cleanup) {
            cleanup->stop();
            cleanup->join();
            drain_cleanup_results();
        }
        lock.lock();
        poisoned_ = poisoned_ || (manifest_ && manifest_->poisoned());
        bool terminal = false;
        if (manifest_ && !poisoned_) {
            auto failed = manifest_->data();
            if (failed.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
                poisoned_ = persistence_poisoned();
            } else {
                ++failed.manifest_revision;
                if (failed.task.state != TaskState::Completed) {
                    failed.task.state = TaskState::Failed;
                    failed.task.error = state_.last_error;
                    failed.task.terminal_time_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
                    for (auto& segment : failed.segments) {
                        if (segment.state == SegmentLifecycleState::Writing ||
                            segment.state == SegmentLifecycleState::Finalizing) {
                            segment.state = SegmentLifecycleState::Incomplete;
                        }
                    }
                }
                if (secondary && failed.secondary_errors.size() < 8) {
                    failed.secondary_errors.push_back(*secondary);
                }
                lock.unlock();
                const auto committed = manifest_->commit(std::move(failed));
                lock.lock();
                terminal = committed.has_value();
                poisoned_ = manifest_->poisoned();
            }
        }
        writer_open_ = false;
        writer_.reset();
        snapshot_.reset();
        message_queue_.reset();
        message_pool_.reset();
        cleanup_.reset();
        cleanup_retry_.clear();
        timed_deadline_steady_ns_.store(0);
        rolling_deadline_steady_ns_.store(0);
        retention_deadline_steady_ns_.store(0);
        failure_settled_ = true;
        if (terminal && !poisoned_ && !shutdown_requested_.load()) {
            const auto primary = state_.last_error;
            manifest_.reset();
            topic_events_.reset();
            state_ = State{};
            state_.last_error = primary;
        }
    }

    template <class T, class Function>
    bool submit_operation(CompletionSource<T> source, Function&& function) {
        return submit([this, source, function = std::forward<Function>(function)]() mutable {
            operation_effects_ = Effects::None;
            try {
                if (shutdown_requested_.load()) {
                    source.set_error(Error{ErrorCode::InvalidState, Effects::None, {}});
                    return;
                }
                function();
            } catch (const std::bad_alloc&) {
                handle_operation_exception(source, ErrorCode::ResourceExhausted);
            } catch (const std::filesystem::filesystem_error&) {
                handle_operation_exception(source, ErrorCode::StorageError);
            } catch (const std::system_error& exception) {
                handle_operation_exception(
                    source, exception.code() == std::errc::resource_unavailable_try_again
                                ? ErrorCode::ResourceExhausted
                                : ErrorCode::InternalError);
            } catch (...) {
                handle_operation_exception(source, ErrorCode::InternalError);
            }
        });
    }

    template <class T>
    void handle_operation_exception(const CompletionSource<T>& source, ErrorCode code) {
        unsigned owned = 1;
        start_commit_authority_.compare_exchange_strong(owned, 0);
        // Empty messages make the bad_alloc boundary itself allocation-free.
        Error failure{code, operation_effects_, {}};
        if (manifest_ && manifest_->poisoned()) {
            failure.code = ErrorCode::StorageError;
            failure.effects = Effects::Possible;
        }
        if (topic_events_ && topic_events_->uncertainty_watermark() != 0 &&
            operation_effects_ != Effects::None) {
            failure.code = ErrorCode::EventCommitFailed;
            failure.effects = Effects::Possible;
        }
        if (operation_effects_ != Effects::None) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_.recorder_state = RecorderState::Failed;
            if (operation_effects_ == Effects::Possible) {
                poisoned_ = true;
            }
            if (!state_.last_error) {
                state_.last_error = failure;
            }
        }
        source.set_error(std::move(failure));
    }

    std::vector<PublisherVisibility> collect_publisher_visibility() const {
        if (!node_) {
            return {};
        }
        struct TopicVisibilityRequest {
            TopicId topic_id = 0;
            std::string name;
            QosOptions qos;
        };
        std::vector<TopicVisibilityRequest> topics;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            topics.reserve(active_topics_.size());
            for (const auto& entry : active_topics_) {
                topics.push_back(TopicVisibilityRequest{
                    entry.first, entry.second.info.name, entry.second.info.qos});
            }
        }
        std::vector<PublisherVisibility> observations;
        observations.reserve(topics.size());
        const auto observed_steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count();
        for (const auto& topic : topics) {
            try {
                const auto publishers =
                    publisher_qos_snapshot(topic.name, config_.discovery.max_profiles);
                auto compatibility = TopicState::Compatibility::Unknown;
                if (publishers.count != 0 && !publishers.truncated &&
                    publishers.profiles.size() == publishers.count) {
                    compatibility = TopicState::Compatibility::Compatible;
                    for (const auto& profile : publishers.profiles) {
                        if (!qos_compatible(profile, topic.qos)) {
                            compatibility = TopicState::Compatibility::Incompatible;
                            break;
                        }
                    }
                }
                observations.push_back(PublisherVisibility{
                    topic.topic_id, publishers.count, observed_steady_ns, compatibility});
            } catch (const std::exception&) {
            }
        }
        return observations;
    }

    ProcessSample sample_process() {
        ProcessSample sample;
        sample.sampled_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) == 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto cpu_ns = static_cast<std::int64_t>(usage.ru_utime.tv_sec) * 1000000000LL +
                                static_cast<std::int64_t>(usage.ru_utime.tv_usec) * 1000LL +
                                static_cast<std::int64_t>(usage.ru_stime.tv_sec) * 1000000000LL +
                                static_cast<std::int64_t>(usage.ru_stime.tv_usec) * 1000LL;
            if (process_baseline_) {
                const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            now - process_baseline_->sampled_at)
                                            .count();
                if (elapsed_ns > 0 && cpu_ns >= process_baseline_->cpu_ns) {
                    sample.cpu.percent = 100.0 *
                                         static_cast<double>(cpu_ns - process_baseline_->cpu_ns) /
                                         static_cast<double>(elapsed_ns);
                }
            }
            process_baseline_ = ProcessBaseline{cpu_ns, now};
        }
        std::ifstream statm("/proc/self/statm");
        std::uint64_t pages = 0;
        std::uint64_t resident_pages = 0;
        if (statm >> pages >> resident_pages) {
            const auto page_size = ::sysconf(_SC_PAGESIZE);
            if (page_size > 0 && resident_pages <= std::numeric_limits<std::uint64_t>::max() /
                                                       static_cast<std::uint64_t>(page_size)) {
                sample.memory.rss_bytes = resident_pages * static_cast<std::uint64_t>(page_size);
            }
        }
        const auto count_entries =
            [](const std::filesystem::path& path) -> std::optional<std::uint32_t> {
            std::error_code error_code;
            std::uint32_t count = 0;
            for (std::filesystem::directory_iterator entry(path, error_code), end;
                 !error_code && entry != end; entry.increment(error_code)) {
                if (count == std::numeric_limits<std::uint32_t>::max()) {
                    return std::nullopt;
                }
                ++count;
            }
            if (error_code) {
                return std::nullopt;
            }
            return count;
        };
        if (const auto count = count_entries("/proc/self/task")) {
            sample.threads.count = *count;
        }
        if (const auto count = count_entries("/proc/self/fd")) {
            sample.file_descriptors.count = *count;
        }
        return sample;
    }

    void run() noexcept {
        while (true) {
            try {
                // Check every due deadline each iteration, even under a continuous
                // stream of external requests. No separate timer execution domain.
                terminalize_failure();
                const auto now = steady_now_ns();
                const auto timed = timed_deadline_steady_ns_.load();
                const auto rolling = rolling_deadline_steady_ns_.load();
                const auto retention = retention_deadline_steady_ns_.load();
                if (timed != 0 && now >= timed) {
                    stop_timed_task();
                }
                if (rolling != 0 && now >= rolling) {
                    rotate_rolling_segment();
                }
                if (retention != 0 && now >= retention) {
                    run_retention();
                }
                const auto disk_generation = producer_disk_event_.exchange(0);
                if (disk_generation != 0) {
                    bool current = false;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        current = state_.task && state_.task->generation == disk_generation;
                    }
                    if (current) {
                        apply_disk_safety();
                    }
                }
                terminalize_failure();
                std::function<void()> operation;
                std::optional<DeleteResult> cleanup_result;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    const auto ready = [this] {
                        return stopping_worker_ || shutdown_event_ ||
                               producer_disk_event_.load() != 0 || !queue_.empty() ||
                               std::any_of(
                                   system_events_.begin(), system_events_.end(),
                                   [](const auto& slot) { return bool(slot); }) ||
                               std::any_of(
                                   cleanup_results_.begin(), cleanup_results_.end(),
                                   [](const auto& slot) { return bool(slot); });
                    };
                    std::int64_t next = 0;
                    for (const auto* deadline :
                         {&timed_deadline_steady_ns_, &rolling_deadline_steady_ns_,
                          &retention_deadline_steady_ns_}) {
                        const auto value = deadline->load();
                        if (value != 0 && (next == 0 || value < next)) {
                            next = value;
                        }
                    }
                    if (next == 0) {
                        queue_condition_.wait(lock, ready);
                    } else {
                        queue_condition_.wait_until(
                            lock,
                            std::chrono::steady_clock::time_point(std::chrono::nanoseconds(next)),
                            ready);
                    }
                    // Safety/results precede external traffic. Shutdown is a
                    // dedicated single slot; it cannot grow the external queue.
                    for (auto& slot : system_events_) {
                        if (slot) {
                            operation = std::move(slot);
                            slot = {};
                            break;
                        }
                    }
                    if (!operation) {
                        for (auto& slot : cleanup_results_) {
                            if (slot) {
                                cleanup_result = std::move(slot);
                                slot.reset();
                                break;
                            }
                        }
                    }
                    if (!operation && !cleanup_result && shutdown_event_) {
                        operation = std::move(shutdown_event_);
                        shutdown_event_ = {};
                    }
                    if (!operation && !cleanup_result && !queue_.empty()) {
                        operation = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    if (!operation && !cleanup_result && stopping_worker_) {
                        return;
                    }
                }
                if (cleanup_result) {
                    apply_cleanup_result(std::move(*cleanup_result));
                } else if (operation) {
                    operation();
                }
                terminalize_failure();
            } catch (...) {
                // Even failure cleanup can allocate. Retain ownership and close
                // admission if a secondary exception prevents terminalization.
                std::lock_guard<std::mutex> lock(state_mutex_);
                close_ingress_locked();
                state_.recorder_state = RecorderState::Failed;
                if (!state_.last_error) {
                    state_.last_error = Error{ErrorCode::InternalError, Effects::Possible, {}};
                }
                failure_settled_ = true;
                timed_deadline_steady_ns_.store(0);
                rolling_deadline_steady_ns_.store(0);
                retention_deadline_steady_ns_.store(0);
            }
        }
    }

    void schedule_retention_locked() {
        retention_deadline_steady_ns_.store(0, std::memory_order_release);
        if (!manifest_ || !cleanup_ || state_.recorder_state == RecorderState::Failed) {
            return;
        }
        if (!state_.task) {
            return;
        }
        if (state_.task->config.mode != TaskMode::Rolling) {
            return;
        }
        if (state_.task->config.rolling.retention_duration.count() <= 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> next_due;
        const auto candidates = manifest_->data().segments;
        for (const auto& segment : candidates) {
            const auto completed = completed_segment_steady_.find(segment.segment_id);
            if (segment.state != SegmentLifecycleState::Complete ||
                completed == completed_segment_steady_.end()) {
                continue;
            }
            const auto due = completed->second + state_.task->config.rolling.retention_duration;
            if (now < due) {
                if (!next_due || due < *next_due) {
                    next_due = due;
                }
                continue;
            }
            ManifestData updated = manifest_->data();
            if (updated.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = error(
                    ErrorCode::StorageError, "manifest revision exhausted",
                    Effects::ConfirmedPartial);
                poisoned_ = persistence_poisoned();
                return;
            }
            auto entry = std::find_if(
                updated.segments.begin(), updated.segments.end(),
                [&segment](const SegmentInfo& candidate) {
                    return candidate.segment_id == segment.segment_id;
                });
            if (entry == updated.segments.end() ||
                entry->state != SegmentLifecycleState::Complete) {
                continue;
            }
            entry->state = SegmentLifecycleState::Deleting;
            ++updated.manifest_revision;
            DeleteWork work{
                updated.task.recording_id, entry->segment_id,
                manifest_->task_directory() / entry->relative_path, updated.manifest_revision};
            const auto committed = manifest_->commit(std::move(updated));
            if (!committed) {
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = committed.error();
                poisoned_ = persistence_poisoned();
                return;
            }
            if (!enqueue_cleanup(work)) {
                cleanup_retry_.push_back(std::move(work));
                state_.cleanup.queue_full++;
                state_.cleanup.last_error =
                    error(ErrorCode::ResourceExhausted, "cleanup queue is full");
            }
        }
        while (!cleanup_retry_.empty() && enqueue_cleanup(cleanup_retry_.front())) {
            cleanup_retry_.pop_front();
        }
        if (!cleanup_retry_.empty()) {
            const auto retry_due = now + config_.cleanup.retry_interval;
            if (!next_due || retry_due < *next_due) {
                next_due = retry_due;
            }
        }
        if (next_due) {
            retention_deadline_steady_ns_.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(next_due->time_since_epoch())
                    .count(),
                std::memory_order_release);
            queue_condition_.notify_one();
        }
    }

    void run_retention() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        schedule_retention_locked();
    }

    bool make_room_for_segment_locked() {
        if (!manifest_) {
            return false;
        }
        ManifestData updated = manifest_->data();
        if (updated.segments.size() < config_.history.max_segment_records) {
            return true;
        }

        std::size_t erase_count = 0;
        std::uint64_t erased_bytes = 0;
        while (updated.segments.size() - erase_count >= config_.history.max_segment_records) {
            if (erase_count == updated.segments.size() ||
                updated.segments[erase_count].state != SegmentLifecycleState::Removed) {
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = error(
                    ErrorCode::ResourceExhausted,
                    "segment history limit reached without a removable prefix");
                poisoned_ = persistence_poisoned();
                return false;
            }
            const auto bytes = updated.segments[erase_count].size_bytes.value_or(0);
            if (erased_bytes > std::numeric_limits<std::uint64_t>::max() - bytes ||
                updated.task.segment_history.removed_count >
                    std::numeric_limits<std::uint64_t>::max() - erase_count - 1 ||
                updated.task.segment_history.removed_bytes >
                    std::numeric_limits<std::uint64_t>::max() - erased_bytes - bytes) {
                state_.recorder_state = RecorderState::Failed;
                state_.last_error =
                    error(ErrorCode::StorageError, "segment history summary overflow");
                poisoned_ = persistence_poisoned();
                return false;
            }
            erased_bytes += bytes;
            ++erase_count;
        }
        if (updated.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = error(
                ErrorCode::StorageError, "manifest revision exhausted", Effects::ConfirmedPartial);
            poisoned_ = persistence_poisoned();
            return false;
        }
        for (std::size_t index = 0; index < erase_count; ++index) {
            completed_segment_steady_.erase(updated.segments[index].segment_id);
        }
        updated.segments.erase(
            updated.segments.begin(),
            updated.segments.begin() + static_cast<std::ptrdiff_t>(erase_count));
        updated.task.segment_history.removed_count += erase_count;
        updated.task.segment_history.removed_bytes += erased_bytes;
        ++updated.manifest_revision;
        const auto committed = manifest_->commit(std::move(updated));
        if (committed) {
            return true;
        }
        state_.recorder_state = RecorderState::Failed;
        state_.last_error = committed.error();
        poisoned_ = persistence_poisoned();
        return false;
    }

    void rotate_rolling_segment(bool for_resume = false) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (!manifest_ || !state_.task || state_.task->config.mode != TaskMode::Rolling ||
            !state_.segments.current) {
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        if (state_.recorder_state == RecorderState::Paused && !for_resume) {
            rotate_pending_ = true;
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        if (state_.recorder_state != RecorderState::Recording &&
            !(for_resume && state_.recorder_state == RecorderState::Paused)) {
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        rotate_pending_ = false;
        if (rolling_due_steady_ != std::chrono::steady_clock::time_point{}) {
            const auto now = std::chrono::steady_clock::now();
            state_.segments.rotation_lag = std::chrono::milliseconds(0);
            if (now >= rolling_due_steady_) {
                state_.segments.rotation_lag =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - rolling_due_steady_);
            }
        }
        rolling_due_steady_ = std::chrono::steady_clock::time_point{};
        state_.recorder_state = RecorderState::Stopping;
        std::vector<std::shared_ptr<Ingress>> ingress_barriers;
        ingress_barriers.reserve(active_topics_.size());
        for (auto& entry : active_topics_) {
            entry.second.admission->close();
            std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
            entry.second.ingress->open = false;
            ingress_barriers.push_back(entry.second.ingress);
        }
        lock.unlock();
        for (const auto& ingress : ingress_barriers) {
            std::unique_lock<std::mutex> ingress_lock(ingress->mutex);
            ingress->condition.wait(ingress_lock, [&ingress] { return ingress->producers == 0; });
        }
        lock.lock();
        auto previous = *state_.segments.current;
        const auto generation = state_.task->generation;
        const auto writer = writer_;
        std::vector<TopicInfo> topics;
        topics.reserve(active_topics_.size());
        for (const auto& entry : active_topics_) {
            topics.push_back(entry.second.info);
        }
        lock.unlock();
        stop_dispatch_thread(false);
        const auto closed = writer->close();
        if (!closed) {
            lock.lock();
            state_.recorder_state = RecorderState::Failed;
            if (!state_.last_error) {
                state_.last_error = closed.error();
            }
            poisoned_ = persistence_poisoned();
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        lock.lock();
        if (state_.recorder_state == RecorderState::Failed) {
            return;
        }
        synchronize_segment_counters_locked();
        previous = *state_.segments.current;
        ManifestData complete = manifest_->data();
        if (complete.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = error(
                ErrorCode::StorageError, "manifest revision exhausted", Effects::ConfirmedPartial);
            poisoned_ = persistence_poisoned();
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        for (auto& segment : complete.segments) {
            if (segment.segment_id == previous.segment_id &&
                segment.state == SegmentLifecycleState::Writing) {
                segment.state = SegmentLifecycleState::Complete;
                segment.completed_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
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
            poisoned_ = persistence_poisoned();
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        previous.state = SegmentLifecycleState::Complete;
        state_.segments.last_completed = previous;
        schedule_retention_locked();
        if (state_.recorder_state == RecorderState::Failed) {
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        const auto next_segment_id = previous.segment_id + 1;
        if (next_segment_id == 0) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error =
                error(ErrorCode::StorageError, "segment or manifest revision exhausted");
            poisoned_ = persistence_poisoned();
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        if (!make_room_for_segment_locked()) {
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        if (manifest_->data().manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = error(
                ErrorCode::StorageError, "manifest revision exhausted", Effects::ConfirmedPartial);
            poisoned_ = persistence_poisoned();
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
        next.started_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        opening.segments.push_back(next);
        ++opening.manifest_revision;
        const auto committed_opening = manifest_->commit(std::move(opening));
        if (!committed_opening) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = committed_opening.error();
            poisoned_ = persistence_poisoned();
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        const auto segment_directory = manifest_->task_directory() / next.relative_path;
        lock.unlock();
        const auto opened = writer->open(segment_directory);
        if (!opened) {
            lock.lock();
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = opened.error();
            poisoned_ = persistence_poisoned();
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        for (const auto& topic : topics) {
            const auto registered = writer->register_topic(topic);
            if (!registered) {
                lock.lock();
                state_.recorder_state = RecorderState::Failed;
                state_.last_error = registered.error();
                poisoned_ = persistence_poisoned();
                rolling_deadline_steady_ns_.store(0, std::memory_order_release);
                return;
            }
        }
        std::vector<RecordedMessage> replay;
        {
            std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
            if (snapshot_) {
                replay = snapshot_->replay_for(generation);
            }
        }
        replay.erase(
            std::remove_if(
                replay.begin(), replay.end(),
                [](const RecordedMessage& message) { return !message.topic; }),
            replay.end());
        const auto replayed = writer->write_batch(replay);
        if (!replayed) {
            lock.lock();
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = replayed.error();
            poisoned_ = persistence_poisoned();
            rolling_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        lock.lock();
        message_queue_ = std::make_shared<MessageQueue>(
            config_.queue.max_messages, config_.queue.max_bytes, config_.max_active_topics);
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
        for (const auto& message : replay) {
            current.written_bytes += message.payload_bytes;
        }
        segment_written_messages_.store(current.written_messages, std::memory_order_relaxed);
        segment_written_bytes_.store(current.written_bytes, std::memory_order_relaxed);
        segment_has_live_messages_.store(false, std::memory_order_relaxed);
        dispatch_segment_id_.store(next_segment_id, std::memory_order_release);
        state_.segments.current = current;
        state_.segments.last_completed = previous;
        state_.recorder_state = RecorderState::Recording;
        if (state_.pause.user_paused || state_.pause.disk != DiskPauseReason::None) {
            state_.recorder_state = RecorderState::Paused;
            close_ingress_locked();
            rolling_deadline_steady_ns_.store(0);
            return;
        }
        if (shutdown_requested_.load() ||
            !disk_snapshot_is_fresh_and_safe(disk_admission_.read(), steady_now_ns())) {
            state_.pause.disk = DiskPauseReason::Unknown;
            state_.recorder_state = RecorderState::Paused;
            close_ingress_locked();
            rolling_deadline_steady_ns_.store(0);
            return;
        }
        for (auto& entry : active_topics_) {
            {
                std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                entry.second.ingress->open = true;
            }
            entry.second.admission->open(generation, entry.second.info.topic_epoch);
        }
        for (auto& topic : state_.topics) {
            topic.state = TopicRuntimeState::Active;
        }
        rolling_deadline_steady_ns_.store(0, std::memory_order_release);
        rolling_due_steady_ = std::chrono::steady_clock::time_point{};
    }

    void stop_timed_task() {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (!manifest_ || (state_.recorder_state != RecorderState::Recording &&
                           state_.recorder_state != RecorderState::Paused)) {
            timed_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        state_.recorder_state = RecorderState::Stopping;
        for (auto& entry : active_topics_) {
            entry.second.admission->close();
            std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
            entry.second.ingress->open = false;
        }
        auto topics = std::move(active_topics_);
        state_.topics.clear();
        const auto writer = writer_;
        const bool writer_open = writer_open_;
        lock.unlock();
        for (auto& entry : topics) {
            std::unique_lock<std::mutex> ingress_lock(entry.second.ingress->mutex);
            entry.second.ingress->condition.wait(
                ingress_lock, [&entry] { return entry.second.ingress->producers == 0; });
        }
        topics.clear();
        resource_monitor_.reset();
        stop_dispatch_thread();
        if (writer_open) {
            const auto closed = writer->close();
            if (!closed) {
                lock.lock();
                state_.recorder_state = RecorderState::Failed;
                if (!state_.last_error) {
                    state_.last_error = closed.error();
                }
                poisoned_ = persistence_poisoned();
                timed_deadline_steady_ns_.store(0, std::memory_order_release);
                return;
            }
        }
        if (cleanup_) {
            cleanup_->stop();
            cleanup_->join();
            drain_cleanup_results();
        }
        lock.lock();
        if (state_.recorder_state == RecorderState::Failed) {
            return;
        }
        synchronize_segment_counters_locked();
        writer_open_ = false;
        ManifestData completed = manifest_->data();
        if (completed.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = error(
                ErrorCode::StorageError, "manifest revision exhausted", Effects::ConfirmedPartial);
            poisoned_ = persistence_poisoned();
            timed_deadline_steady_ns_.store(0, std::memory_order_release);
            return;
        }
        ++completed.manifest_revision;
        completed.task.state = TaskState::Completed;
        completed.task.terminal_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
        for (auto& segment : completed.segments) {
            if (segment.state == SegmentLifecycleState::Writing) {
                segment.state = SegmentLifecycleState::Complete;
                segment.completed_at_ns = completed.task.terminal_time_ns;
                segment.message_count = state_.segments.current->written_messages;
                segment.size_bytes = state_.segments.current->written_bytes;
            }
        }
        const auto committed = manifest_->commit(completed);
        if (!committed) {
            state_.recorder_state = RecorderState::Failed;
            state_.last_error = committed.error();
            poisoned_ = persistence_poisoned();
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
        retention_deadline_steady_ns_.store(0, std::memory_order_release);
        cleanup_retry_.clear();
    }

    void request_shutdown() {
        bool expected = false;
        if (!shutdown_requested_.compare_exchange_strong(expected, true)) {
            return;
        }
        start_commit_authority_.exchange(2);
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_event_ = [this] {
            std::shared_ptr<Writer> writer;
            bool writer_open = false;
            decltype(active_topics_) topics;
            Cleanup* cleanup = nullptr;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                for (auto& entry : active_topics_) {
                    entry.second.admission->close();
                    std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                    entry.second.ingress->open = false;
                }
                topics = std::move(active_topics_);
                state_.topics.clear();
                writer = writer_;
                writer_open = writer_open_;
                cleanup = cleanup_.get();
            }
            for (auto& entry : topics) {
                std::unique_lock<std::mutex> ingress_lock(entry.second.ingress->mutex);
                entry.second.ingress->condition.wait(
                    ingress_lock, [&entry] { return entry.second.ingress->producers == 0; });
            }
            topics.clear();
            resource_monitor_.reset();
            stop_dispatch_thread();
            std::optional<Error> close_error;
            if (writer && writer_open) {
                const auto closed = writer->close();
                if (!closed) {
                    close_error = closed.error();
                }
            }
            if (cleanup) {
                cleanup->stop();
                cleanup->join();
            }
            // The worker has joined; consume every result while the manifest
            // and cleanup ownership are still available to the mutation domain.
            drain_cleanup_results();
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                if (manifest_) {
                    ManifestData failed = manifest_->data();
                    if (!poisoned_ && !manifest_->poisoned() &&
                        failed.task.state != TaskState::Completed &&
                        failed.task.state != TaskState::Failed &&
                        failed.manifest_revision != std::numeric_limits<std::uint64_t>::max()) {
                        ++failed.manifest_revision;
                        failed.task.state = TaskState::Failed;
                        failed.task.terminal_time_ns =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
                        if (state_.last_error) {
                            failed.task.error = state_.last_error;
                        } else if (close_error) {
                            failed.task.error = *close_error;
                        } else {
                            failed.task.error = error(
                                ErrorCode::InternalError, "shutdown interrupted active task",
                                Effects::ConfirmedPartial);
                        }
                        const auto committed = manifest_->commit(failed);
                        if (!committed) {
                            state_.last_error = committed.error();
                        }
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
            std::deque<std::function<void()>> rejected;
            {
                std::lock_guard<std::mutex> queue_lock(queue_mutex_);
                rejected.swap(queue_);
                // All referenced task identities are now stale; their native
                // sources have joined and task terminalization already owns them.
                for (auto& event : system_events_) {
                    event = {};
                }
                stopping_worker_ = true;
            }
            for (auto& operation : rejected) {
                operation();
            }
            shutdown_source_.set_value();
            queue_condition_.notify_all();
        };
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
    std::deque<DeleteWork> cleanup_retry_;
    std::thread dispatch_thread_;
    std::atomic<bool> dispatch_stopping_{false};
    std::mutex dispatch_failure_mutex_;
    std::optional<Error> dispatch_failure_;
    DiskAdmissionState disk_admission_;
    bool writer_open_ = false;
    std::unordered_map<TopicId, ActiveTopic> active_topics_;
    std::unordered_map<std::string, std::uint64_t> topic_epochs_;
    std::unordered_map<std::string, TopicSignature> task_topic_signatures_;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>
        completed_segment_steady_;
    std::uint64_t next_topic_id_ = 1;
    std::atomic<std::uint64_t> pool_exhausted_{0};
    std::atomic<std::uint64_t> queue_full_{0};
    std::atomic<std::uint64_t> writer_write_failure_{0};
    std::atomic<std::uint64_t> segment_written_messages_{0};
    std::atomic<std::uint64_t> segment_written_bytes_{0};
    std::atomic<bool> segment_has_live_messages_{false};
    std::atomic<std::uint64_t> dispatch_segment_id_{0};
    std::chrono::steady_clock::time_point disk_safe_since_{};
    std::optional<ProcessBaseline> process_baseline_;
    std::chrono::steady_clock::time_point rolling_due_steady_{};
    bool rotate_pending_ = false;

    void publish_dispatch_failure(std::uint64_t generation, Error failure) {
        {
            std::lock_guard<std::mutex> lock(dispatch_failure_mutex_);
            if (!dispatch_failure_) {
                dispatch_failure_ = std::move(failure);
            }
        }
        (void)submit_critical(SystemEvent::WriterFailure, [this, generation] {
            std::optional<Error> failure;
            {
                std::lock_guard<std::mutex> lock(dispatch_failure_mutex_);
                failure = dispatch_failure_;
            }
            if (failure) {
                fail_writer(generation, std::move(*failure));
            }
        });
    }

    void start_dispatch_thread(std::shared_ptr<Writer> writer) {
        dispatch_stopping_.store(false, std::memory_order_release);
        const auto generation = state_.task->generation;
        dispatch_thread_ = std::thread([this, writer = std::move(writer), generation] {
            try {
                while (true) {
                    auto first = message_queue_->pop_for(std::chrono::milliseconds(50));
                    if (!first) {
                        if (message_queue_->closed()) {
                            return;
                        }
                        continue;
                    }

                    std::vector<RecordedMessage> batch;
                    batch.reserve(config_.queue.batch_size);
                    batch.push_back(std::move(*first));
                    if (config_.queue.batch_size > 1) {
                        auto additional = message_queue_->pop_batch(config_.queue.batch_size - 1);
                        batch.insert(
                            batch.end(), std::make_move_iterator(additional.begin()),
                            std::make_move_iterator(additional.end()));
                    }

                    const auto result = writer->write_batch(batch);
                    const auto written_at = std::chrono::steady_clock::now();
                    if (!result) {
                        writer_write_failure_.fetch_add(1, std::memory_order_relaxed);
                        const auto generation = batch.front().generation;
                        publish_dispatch_failure(generation, result.error());
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
                        if (snapshot_) {
                            const auto now = std::chrono::steady_clock::now();
                            for (const auto& message : batch) {
                                if (!message.replayed) {
                                    snapshot_->push_written_live(message, now);
                                }
                            }
                        }
                    }

                    const auto generation = batch.front().generation;
                    bool has_live_messages = false;
                    for (const auto& message : batch) {
                        if (message.written_counter) {
                            message.written_counter->fetch_add(1, std::memory_order_relaxed);
                        }
                        segment_written_messages_.fetch_add(1, std::memory_order_relaxed);
                        segment_written_bytes_.fetch_add(
                            message.payload_bytes, std::memory_order_relaxed);
                        if (!message.replayed) {
                            has_live_messages = true;
                        }
                    }

                    if (!has_live_messages ||
                        segment_has_live_messages_.exchange(true, std::memory_order_acq_rel)) {
                        continue;
                    }

                    const auto segment_id = dispatch_segment_id_.load(std::memory_order_acquire);
                    const auto submitted = submit_critical(
                        SystemEvent::FirstLive, [this, generation, segment_id, written_at] {
                            std::lock_guard<std::mutex> state_lock(state_mutex_);
                            if (!state_.task || state_.task->generation != generation) {
                                return;
                            }
                            if (!state_.segments.current ||
                                state_.segments.current->segment_id != segment_id) {
                                return;
                            }
                            if (state_.recorder_state != RecorderState::Recording &&
                                state_.recorder_state != RecorderState::Paused) {
                                return;
                            }
                            state_.segments.current->has_live_messages = true;
                            if (state_.task->config.mode == TaskMode::Rolling) {
                                const auto deadline =
                                    written_at + state_.task->config.rolling.segment_duration;
                                rolling_due_steady_ = deadline;
                                rolling_deadline_steady_ns_.store(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        deadline.time_since_epoch())
                                        .count(),
                                    std::memory_order_release);
                                queue_condition_.notify_one();
                            }
                        });
                    if (!submitted && !shutdown_requested_.load(std::memory_order_acquire)) {
                        segment_has_live_messages_.store(false, std::memory_order_release);
                    }
                }
            } catch (const std::bad_alloc&) {
                publish_dispatch_failure(
                    generation, Error{ErrorCode::ResourceExhausted, Effects::Possible, {}});
            } catch (...) {
                publish_dispatch_failure(
                    generation, Error{ErrorCode::InternalError, Effects::Possible, {}});
            }
        });
    }

    void stop_dispatch_thread(bool stop_pool = true) {
        if (message_queue_) {
            message_queue_->close();
        }
        if (dispatch_thread_.joinable()) {
            dispatch_thread_.join();
        }
        std::optional<Error> failure;
        {
            std::lock_guard<std::mutex> lock(dispatch_failure_mutex_);
            failure = dispatch_failure_;
        }
        if (failure) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_.recorder_state = RecorderState::Failed;
            if (!state_.last_error) {
                state_.last_error = std::move(failure);
            }
        }
        dispatch_stopping_.store(true, std::memory_order_release);
        if (stop_pool && message_pool_) {
            message_pool_->begin_stop();
        }
    }

    void synchronize_segment_counters_locked() {
        if (!state_.segments.current) {
            return;
        }
        state_.segments.current->written_messages =
            segment_written_messages_.load(std::memory_order_relaxed);
        state_.segments.current->written_bytes =
            segment_written_bytes_.load(std::memory_order_relaxed);
        state_.segments.current->has_live_messages =
            segment_has_live_messages_.load(std::memory_order_relaxed);
    }
    std::uint64_t next_generation_ = 1;
    bool poisoned_ = false;
    std::atomic<bool> shutdown_requested_{false};
    // 0: available, 1: Start owns its final commit, 2: shutdown owns the barrier.
    std::atomic<unsigned> start_commit_authority_{0};
    std::atomic<std::uint64_t> initializing_generation_{0};
    std::atomic<std::int64_t> timed_deadline_steady_ns_{0};
    std::atomic<std::int64_t> rolling_deadline_steady_ns_{0};
    std::atomic<std::int64_t> retention_deadline_steady_ns_{0};
    CompletionSource<void> shutdown_source_;
    Completion<void> shutdown_completion_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::deque<std::function<void()>> queue_;
    std::function<void()> shutdown_event_;
    bool stopping_worker_ = false;
    std::array<std::function<void()>, 3> system_events_;
    std::vector<std::optional<DeleteResult>> cleanup_results_;
    std::size_t cleanup_outstanding_ = 0;  // mutation-owned credits
    std::atomic<std::uint64_t> producer_disk_event_{0};
    bool failure_settled_ = false;
    Effects operation_effects_ = Effects::None;
    std::thread worker_;
};

Recorder::Recorder(RecorderConfig config) : Recorder(std::move(config), nullptr) {}
Recorder::Recorder(RecorderConfig config, rclcpp::Node::SharedPtr node)
: implementation_(std::make_unique<Implementation>(std::move(config), std::move(node))) {}
Recorder::~Recorder() = default;

Completion<TaskInfo> Recorder::start(const StartOptions& options) {
    if (implementation_->shutdown_requested_.load()) {
        return failed_completion<TaskInfo>(error(ErrorCode::InvalidState, "shutdown requested"));
    }
    CompletionSource<TaskInfo> source;
    const auto completion = source.completion();
    if (!implementation_->submit_operation(
            source, [implementation = implementation_.get(), options, source] {
                std::unique_lock<std::mutex> lock(implementation->state_mutex_);
                if (implementation->shutdown_requested_.load() || implementation->poisoned_ ||
                    implementation->state_.recorder_state != RecorderState::Idle) {
                    source.set_error(error(ErrorCode::InvalidState, "recorder is not idle"));
                    return;
                }
                implementation->completed_segment_steady_.clear();
                implementation->task_topic_signatures_.clear();
                if (!implementation->valid_recording_id(options.recording_id)) {
                    source.set_error(error(ErrorCode::InvalidArgument, "invalid recording_id"));
                    return;
                }
                const auto config = make_task_config(implementation->config_, options);
                if (!config) {
                    source.set_error(config.error());
                    return;
                }
                auto disk_config = implementation->disk_config();
                // A first-ever output root may not exist yet. Sample its nearest
                // existing ancestor before creating any task-owned directory.
                lock.unlock();
                while (!std::filesystem::exists(disk_config.path) &&
                       disk_config.path.has_relative_path()) {
                    disk_config.path = std::filesystem::absolute(disk_config.path).parent_path();
                }
                const auto sample = ResourceMonitor::sample_now(disk_config);
                const auto valid_until =
                    Implementation::steady_now_ns() +
                    std::chrono::duration_cast<std::chrono::nanoseconds>(disk_config.max_age)
                        .count();
                implementation->disk_admission_.publish(sample.state, valid_until);
                lock.lock();
                implementation->state_.disk = sample;
                if (!implementation->disk_snapshot_is_fresh_and_safe(
                        implementation->disk_admission_.read(), Implementation::steady_now_ns())) {
                    source.set_error(error(
                        sample.state == DiskSafetyState::Critical ? ErrorCode::DiskCritical
                                                                  : ErrorCode::DiskUnknown,
                        "disk is not safe for Start"));
                    return;
                }
                if (implementation->shutdown_requested_.load()) {
                    source.set_error(error(ErrorCode::InvalidState, "shutdown requested"));
                    return;
                }
                implementation->failure_settled_ = false;
                implementation->state_.last_error.reset();
                const auto directory = implementation->config_.output_root / options.recording_id;
                if (std::filesystem::exists(directory)) {
                    source.set_error(
                        error(ErrorCode::RecordingExists, "recording directory already exists"));
                    return;
                }
                if (implementation->next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
                    source.set_error(error(ErrorCode::ResourceExhausted, "generation exhausted"));
                    return;
                }
                ManifestData data;
                data.manifest_revision = 1;
                data.task.recording_id = options.recording_id;
                data.task.generation = implementation->next_generation_++;
                data.task.state = TaskState::Initializing;
                data.task.config = config.value();
                data.task.start_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
                State::Task identity{
                    data.task.recording_id, data.task.generation, data.task.config};
                if (!implementation->disk_snapshot_is_fresh_and_safe(
                        implementation->disk_admission_.read(), Implementation::steady_now_ns())) {
                    source.set_error(error(ErrorCode::DiskUnknown, "Start disk sample expired"));
                    return;
                }
                auto initial_data = data;
                implementation->operation_effects_ = Effects::Possible;
                lock.unlock();
                auto initial = Manifest::create(directory, std::move(initial_data));
                lock.lock();
                if (!initial) {
                    if (initial.error().effects == Effects::Possible) {
                        implementation->poisoned_ = true;
                        implementation->state_.recorder_state = RecorderState::Failed;
                        implementation->state_.last_error = initial.error();
                    }
                    source.set_error(initial.error());
                    return;
                }
                implementation->manifest_ = std::move(initial.value());
                implementation->state_.recorder_state = RecorderState::Initializing;
                implementation->state_.task = std::move(identity);
                implementation->initializing_generation_.store(data.task.generation);
                implementation->operation_effects_ = Effects::ConfirmedPartial;
                lock.unlock();
                const auto events =
                    TopicEvents::create(directory, data.task.recording_id, data.task.generation);
                lock.lock();
                if (!events) {
                    auto failure = events.error();
                    failure.effects = Effects::ConfirmedPartial;
                    implementation->state_.recorder_state = RecorderState::Failed;
                    implementation->state_.last_error = failure;
                    source.set_error(std::move(failure));
                    return;
                }
                implementation->topic_events_ = events.value();
                implementation->writer_ =
                    std::make_shared<Writer>(implementation->config_.compression);
                const auto pool_capacity = message_pool_capacity(implementation->config_);
                if (!pool_capacity) {
                    implementation->state_.recorder_state = RecorderState::Failed;
                    implementation->state_.last_error = pool_capacity.error();
                    implementation->poisoned_ = implementation->persistence_poisoned();
                    source.set_error(pool_capacity.error());
                    return;
                }
                implementation->message_pool_ =
                    std::make_shared<MessagePool>(pool_capacity.value());
                implementation->message_queue_ = std::make_shared<MessageQueue>(
                    implementation->config_.queue.max_messages,
                    implementation->config_.queue.max_bytes,
                    implementation->config_.max_active_topics);
                if (config.value().mode == TaskMode::Rolling) {
                    SnapshotConfig snapshot_config;
                    snapshot_config.max_messages_per_topic =
                        implementation->config_.snapshot.max_messages_per_topic;
                    snapshot_config.max_bytes_per_topic =
                        implementation->config_.snapshot.max_bytes_per_topic;
                    snapshot_config.max_total_bytes =
                        implementation->config_.snapshot.max_total_bytes;
                    snapshot_config.max_age = config.value().rolling.retention_duration;
                    implementation->snapshot_ = std::make_unique<Snapshot>(snapshot_config);
                }
                implementation->pool_exhausted_.store(0, std::memory_order_relaxed);
                implementation->queue_full_.store(0, std::memory_order_relaxed);
                implementation->writer_write_failure_.store(0, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> failure_lock(
                        implementation->dispatch_failure_mutex_);
                    implementation->dispatch_failure_.reset();
                }
                implementation->segment_written_messages_.store(0, std::memory_order_relaxed);
                implementation->segment_written_bytes_.store(0, std::memory_order_relaxed);
                implementation->segment_has_live_messages_.store(false, std::memory_order_relaxed);
                implementation->dispatch_segment_id_.store(0, std::memory_order_relaxed);
                implementation->process_baseline_.reset();
                implementation->start_dispatch_thread(implementation->writer_);
                implementation->cleanup_ = std::make_unique<Cleanup>(
                    implementation->config_.cleanup.queue_capacity,
                    implementation->config_.cleanup.warning, [implementation](DeleteResult result) {
                        implementation->submit_cleanup_result(std::move(result));
                    });
                disk_config = implementation->disk_config();
                implementation->resource_monitor_ = std::make_unique<ResourceMonitor>(
                    disk_config, implementation->disk_admission_,
                    [implementation, generation = data.task.generation](DiskSample sample) {
                        bool disk_pause_required = sample.state == DiskSafetyState::Critical;
                        if (sample.state == DiskSafetyState::Unknown &&
                            sample.consecutive_failures >=
                                implementation->config_.disk.failure_threshold) {
                            disk_pause_required = true;
                        }
                        if (disk_pause_required) {
                            implementation->producer_disk_event_.store(generation);
                            implementation->queue_condition_.notify_one();
                        }
                        auto visibility = implementation->collect_publisher_visibility();
                        auto process = implementation->sample_process();
                        auto apply_sample = [implementation, sample, generation,
                                             disk_pause_required, process = std::move(process),
                                             visibility = std::move(visibility)] {
                            std::unique_lock<std::mutex> state_lock(implementation->state_mutex_);
                            if (!implementation->state_.task ||
                                implementation->state_.task->generation != generation) {
                                return;
                            }
                            implementation->state_.disk = sample;
                            implementation->state_.process = process;
                            for (const auto& observation : visibility) {
                                const auto active =
                                    implementation->active_topics_.find(observation.topic_id);
                                if (active == implementation->active_topics_.end()) {
                                    continue;
                                }
                                active->second.counters->publisher_count.store(
                                    observation.publisher_count, std::memory_order_relaxed);
                                active->second.counters->publisher_observed_steady_ns.store(
                                    observation.observed_steady_ns, std::memory_order_relaxed);
                                active->second.counters->qos_compatible.store(
                                    observation.compatibility, std::memory_order_relaxed);
                            }
                            if (disk_pause_required) {
                                implementation->disk_safe_since_ =
                                    std::chrono::steady_clock::time_point{};
                                implementation->state_.pause.disk = DiskPauseReason::Unknown;
                                if (sample.state == DiskSafetyState::Critical) {
                                    implementation->state_.pause.disk = DiskPauseReason::Critical;
                                }
                                if (implementation->state_.recorder_state !=
                                    RecorderState::Recording) {
                                    return;
                                }
                                implementation->state_.recorder_state = RecorderState::Paused;
                                for (auto& entry : implementation->active_topics_) {
                                    entry.second.admission->close();
                                    std::lock_guard<std::mutex> ingress_lock(
                                        entry.second.ingress->mutex);
                                    entry.second.ingress->open = false;
                                }
                                for (auto& topic : implementation->state_.topics) {
                                    topic.state = TopicRuntimeState::Armed;
                                }
                                return;
                            }
                            if (sample.state == DiskSafetyState::Unknown) {
                                return;
                            }
                            if (implementation->state_.pause.disk == DiskPauseReason::None) {
                                return;
                            }
                            if (implementation->disk_safe_since_ ==
                                std::chrono::steady_clock::time_point{}) {
                                implementation->disk_safe_since_ = std::chrono::steady_clock::now();
                            }
                            if (std::chrono::steady_clock::now() -
                                    implementation->disk_safe_since_ <
                                implementation->config_.disk.stable) {
                                return;
                            }
                            if (!implementation->config_.disk.auto_resume) {
                                implementation->state_.pause.disk =
                                    DiskPauseReason::RecoveryPending;
                                return;
                            }
                            const auto deadline = implementation->timed_deadline_steady_ns_.load();
                            if (implementation->shutdown_requested_.load() ||
                                (deadline != 0 && Implementation::steady_now_ns() >= deadline) ||
                                !implementation->disk_snapshot_is_fresh_and_safe(
                                    implementation->disk_admission_.read(),
                                    Implementation::steady_now_ns())) {
                                return;
                            }
                            implementation->state_.pause.disk = DiskPauseReason::None;
                            if (implementation->state_.pause.user_paused) {
                                return;
                            }
                            if (implementation->state_.recorder_state != RecorderState::Paused) {
                                return;
                            }
                            if (!implementation->state_.task) {
                                return;
                            }
                            implementation->state_.recorder_state = RecorderState::Recording;
                            if (implementation->rotate_pending_) {
                                // A rolling deadline may have elapsed while disk protection
                                // held ingress closed. Rotation owns reopening ingress, so do
                                // not admit another live message into the expired segment.
                                state_lock.unlock();
                                implementation->rotate_rolling_segment();
                                return;
                            }
                            const auto deadline_ns = implementation->timed_deadline_steady_ns_.load(
                                std::memory_order_acquire);
                            for (auto& entry : implementation->active_topics_) {
                                {
                                    std::lock_guard<std::mutex> ingress_lock(
                                        entry.second.ingress->mutex);
                                    entry.second.ingress->open = true;
                                }
                                entry.second.admission->open(
                                    implementation->state_.task->generation,
                                    entry.second.info.topic_epoch, deadline_ns);
                            }
                            for (auto& topic : implementation->state_.topics) {
                                topic.state = TopicRuntimeState::Active;
                            }
                        };
                        (void)implementation->submit_critical(
                            Implementation::SystemEvent::DiskSample, std::move(apply_sample));
                    });
                unsigned available = 0;
                if (implementation->shutdown_requested_.load() ||
                    !implementation->start_commit_authority_.compare_exchange_strong(
                        available, 1)) {
                    implementation->state_.recorder_state = RecorderState::Failed;
                    auto failure = error(
                        ErrorCode::InvalidState, "shutdown interrupted Start",
                        Effects::ConfirmedPartial);
                    implementation->state_.last_error = failure;
                    source.set_error(std::move(failure));
                    return;
                }
                ManifestData recording = data;
                recording.manifest_revision = 2;
                recording.task.state = TaskState::Recording;
                lock.unlock();
                const auto committed = implementation->manifest_->commit(recording);
                lock.lock();
                if (!committed) {
                    unsigned owned = 1;
                    implementation->start_commit_authority_.compare_exchange_strong(owned, 0);
                    implementation->state_.recorder_state = RecorderState::Failed;
                    implementation->state_.last_error = committed.error();
                    implementation->poisoned_ = implementation->persistence_poisoned();
                    source.set_error(error(
                        ErrorCode::StorageError, "recording manifest transition failed",
                        committed.error().effects == Effects::Possible
                            ? Effects::Possible
                            : Effects::ConfirmedPartial));
                    return;
                }
                implementation->state_.recorder_state = RecorderState::Recording;
                implementation->initializing_generation_.store(0);
                if (recording.task.config.mode == TaskMode::Timed) {
                    const auto deadline =
                        std::chrono::steady_clock::now() + recording.task.config.timed.duration;
                    implementation->timed_deadline_steady_ns_.store(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            deadline.time_since_epoch())
                            .count(),
                        std::memory_order_release);
                    implementation->queue_condition_.notify_one();
                }
                lock.unlock();
                if (!implementation->disk_snapshot_is_fresh_and_safe(
                        implementation->disk_admission_.read(), Implementation::steady_now_ns())) {
                    implementation->apply_disk_safety();
                }
                source.set_value(recording.task);
                unsigned owned = 1;
                implementation->start_commit_authority_.compare_exchange_strong(owned, 0);
            })) {
        source.set_error(error(ErrorCode::ResourceExhausted, "recorder mutation queue is full"));
    }
    return completion;
}

Completion<void> Recorder::stop(std::string_view recording_id, std::uint64_t generation) {
    if (implementation_->shutdown_requested_.load()) {
        return failed_completion<void>(error(ErrorCode::InvalidState, "shutdown requested"));
    }
    if (implementation_->target_is_initializing(recording_id, generation)) {
        return failed_completion<void>(error(ErrorCode::InvalidState, "task is initializing"));
    }
    CompletionSource<void> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (!implementation_->submit_operation(
            source, [implementation = implementation_.get(), id, generation, source] {
                std::unique_lock<std::mutex> lock(implementation->state_mutex_);
                if (!implementation->state_.task ||
                    implementation->state_.task->recording_id != id ||
                    implementation->state_.task->generation != generation) {
                    source.set_error(error(ErrorCode::StaleRecording, "stale recording identity"));
                    return;
                }
                if (implementation->state_.recorder_state != RecorderState::Recording &&
                    implementation->state_.recorder_state != RecorderState::Paused) {
                    source.set_error(error(
                        ErrorCode::InvalidState, "recording cannot be stopped in this state"));
                    return;
                }
                implementation->operation_effects_ = Effects::ConfirmedPartial;
                implementation->state_.recorder_state = RecorderState::Stopping;
                for (auto& entry : implementation->active_topics_) {
                    entry.second.admission->close();
                    std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                    entry.second.ingress->open = false;
                }
                auto topics = std::move(implementation->active_topics_);
                implementation->state_.topics.clear();
                const auto writer = implementation->writer_;
                const bool writer_open = implementation->writer_open_;
                lock.unlock();
                for (auto& entry : topics) {
                    std::unique_lock<std::mutex> ingress_lock(entry.second.ingress->mutex);
                    entry.second.ingress->condition.wait(
                        ingress_lock, [&entry] { return entry.second.ingress->producers == 0; });
                }
                topics.clear();
                implementation->resource_monitor_.reset();
                implementation->stop_dispatch_thread();
                if (writer_open) {
                    const auto closed = writer->close();
                    if (!closed) {
                        lock.lock();
                        implementation->state_.recorder_state = RecorderState::Failed;
                        if (!implementation->state_.last_error) {
                            implementation->state_.last_error = closed.error();
                        }
                        implementation->poisoned_ = implementation->persistence_poisoned();
                        source.set_error(*implementation->state_.last_error);
                        return;
                    }
                }
                if (implementation->cleanup_) {
                    implementation->cleanup_->stop();
                    implementation->cleanup_->join();
                    implementation->drain_cleanup_results();
                }
                lock.lock();
                if (implementation->state_.recorder_state == RecorderState::Failed) {
                    source.set_error(*implementation->state_.last_error);
                    return;
                }
                implementation->synchronize_segment_counters_locked();
                implementation->writer_open_ = false;
                ManifestData completed = implementation->manifest_->data();
                if (completed.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
                    implementation->state_.recorder_state = RecorderState::Failed;
                    implementation->state_.last_error = error(
                        ErrorCode::StorageError, "manifest revision exhausted",
                        Effects::ConfirmedPartial);
                    implementation->poisoned_ = implementation->persistence_poisoned();
                    source.set_error(*implementation->state_.last_error);
                    return;
                }
                completed.manifest_revision++;
                completed.task.state = TaskState::Completed;
                completed.task.terminal_time_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                for (auto& segment : completed.segments) {
                    if (segment.state == SegmentLifecycleState::Writing) {
                        segment.state = SegmentLifecycleState::Complete;
                        segment.completed_at_ns = completed.task.terminal_time_ns;
                        segment.message_count =
                            implementation->state_.segments.current->written_messages;
                        segment.size_bytes = implementation->state_.segments.current->written_bytes;
                    }
                }
                const auto committed = implementation->manifest_->commit(completed);
                if (!committed) {
                    implementation->state_.recorder_state = RecorderState::Failed;
                    implementation->state_.last_error = committed.error();
                    implementation->poisoned_ = implementation->persistence_poisoned();
                    auto failure = committed.error();
                    if (failure.effects == Effects::None) {
                        failure.effects = Effects::ConfirmedPartial;
                    }
                    source.set_error(std::move(failure));
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
                implementation->retention_deadline_steady_ns_.store(0, std::memory_order_release);
                implementation->cleanup_retry_.clear();
                source.set_value();
            })) {
        source.set_error(error(ErrorCode::ResourceExhausted, "recorder mutation queue is full"));
    }
    return completion;
}

Completion<void> Recorder::pause(std::string_view recording_id, std::uint64_t generation) {
    if (implementation_->target_is_initializing(recording_id, generation)) {
        return failed_completion<void>(error(ErrorCode::InvalidState, "task is initializing"));
    }
    CompletionSource<void> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (implementation_->shutdown_requested_.load() ||
        !implementation_->submit_operation(
            source, [implementation = implementation_.get(), id, generation, source] {
                std::lock_guard<std::mutex> lock(implementation->state_mutex_);
                if (!implementation->state_.task ||
                    implementation->state_.task->recording_id != id ||
                    implementation->state_.task->generation != generation) {
                    source.set_error(error(ErrorCode::StaleRecording, "stale recording identity"));
                    return;
                }
                if (implementation->state_.recorder_state != RecorderState::Recording &&
                    implementation->state_.recorder_state != RecorderState::Paused) {
                    source.set_error(error(ErrorCode::InvalidState, "recording cannot be paused"));
                    return;
                }
                if (implementation->state_.pause.user_paused) {
                    source.set_error(error(ErrorCode::AlreadyPaused, "already user paused"));
                    return;
                }
                implementation->state_.pause.user_paused = true;
                for (auto& entry : implementation->active_topics_) {
                    entry.second.admission->close();
                    std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                    entry.second.ingress->open = false;
                }
                for (auto& topic : implementation->state_.topics) {
                    topic.state = TopicRuntimeState::Armed;
                }
                implementation->state_.recorder_state = RecorderState::Paused;
                source.set_value();
            })) {
        source.set_error(error(
            implementation_->shutdown_requested_.load() ? ErrorCode::InvalidState
                                                        : ErrorCode::ResourceExhausted,
            "shutdown requested or mutation queue full"));
    }
    return completion;
}

Completion<void> Recorder::resume(std::string_view recording_id, std::uint64_t generation) {
    if (implementation_->target_is_initializing(recording_id, generation)) {
        return failed_completion<void>(error(ErrorCode::InvalidState, "task is initializing"));
    }
    CompletionSource<void> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (implementation_->shutdown_requested_.load() ||
        !implementation_->submit_operation(
            source, [implementation = implementation_.get(), id, generation, source] {
                std::unique_lock<std::mutex> lock(implementation->state_mutex_);
                if (!implementation->state_.task ||
                    implementation->state_.task->recording_id != id ||
                    implementation->state_.task->generation != generation) {
                    source.set_error(error(ErrorCode::StaleRecording, "stale recording identity"));
                    return;
                }
                if (implementation->state_.recorder_state != RecorderState::Paused) {
                    source.set_error(error(ErrorCode::InvalidState, "recording is not paused"));
                    return;
                }
                const auto finalization_due = [implementation] {
                    const auto deadline = implementation->timed_deadline_steady_ns_.load();
                    return implementation->shutdown_requested_.load() ||
                           (deadline != 0 && Implementation::steady_now_ns() >= deadline);
                };
                if (finalization_due()) {
                    source.set_error(
                        error(ErrorCode::InvalidState, "task finalization already due"));
                    return;
                }
                if (!implementation->state_.pause.user_paused &&
                    implementation->state_.pause.disk == DiskPauseReason::None) {
                    source.set_error(error(ErrorCode::InvalidState, "no pause reason to resume"));
                    return;
                }
                if (implementation->rotate_pending_) {
                    implementation->operation_effects_ = Effects::ConfirmedPartial;
                    lock.unlock();
                    implementation->rotate_rolling_segment(true);
                    lock.lock();
                    if (!implementation->state_.task ||
                        implementation->state_.task->recording_id != id ||
                        implementation->state_.task->generation != generation ||
                        implementation->state_.recorder_state != RecorderState::Paused) {
                        source.set_error(implementation->state_.last_error.value_or(error(
                            ErrorCode::StorageError, "rotation failed",
                            Effects::ConfirmedPartial)));
                        return;
                    }
                }
                auto candidate = implementation->state_.pause;
                const bool cleared_user = candidate.user_paused;
                candidate.user_paused = false;
                const auto disk = implementation->disk_admission_.read();
                const bool safe = implementation->disk_snapshot_is_fresh_and_safe(
                    disk, Implementation::steady_now_ns());
                if (candidate.disk == DiskPauseReason::RecoveryPending &&
                    implementation->disk_recovery_confirmed(implementation->state_.disk, safe)) {
                    candidate.disk = DiskPauseReason::None;
                }
                if (!safe) {
                    candidate.disk = disk && disk->state == DiskSafetyState::Critical
                                         ? DiskPauseReason::Critical
                                         : DiskPauseReason::Unknown;
                }
                // The commit below is Resume's linearization point. No candidate
                // pause changes have been published before all final guards pass.
                if (finalization_due()) {
                    source.set_error(
                        error(ErrorCode::InvalidState, "task finalization already due"));
                    return;
                }
                implementation->state_.pause = candidate;
                if (candidate.disk != DiskPauseReason::None) {
                    auto code = candidate.disk == DiskPauseReason::Critical
                                    ? ErrorCode::DiskCritical
                                    : ErrorCode::DiskUnknown;
                    source.set_error(error(
                        code, "disk protection remains active",
                        cleared_user ? Effects::ConfirmedPartial : Effects::None));
                    return;
                }
                implementation->state_.recorder_state = RecorderState::Recording;
                const auto deadline_ns = implementation->timed_deadline_steady_ns_.load();
                for (auto& entry : implementation->active_topics_) {
                    {
                        std::lock_guard<std::mutex> ingress_lock(entry.second.ingress->mutex);
                        entry.second.ingress->open = true;
                    }
                    entry.second.admission->open(
                        generation, entry.second.info.topic_epoch, deadline_ns);
                }
                for (auto& topic : implementation->state_.topics) {
                    topic.state = TopicRuntimeState::Active;
                }
                source.set_value();
            })) {
        source.set_error(error(
            implementation_->shutdown_requested_.load() ? ErrorCode::InvalidState
                                                        : ErrorCode::ResourceExhausted,
            "shutdown requested or mutation queue full"));
    }
    return completion;
}

Completion<TopicInfo> Recorder::add_topic(
    std::string_view recording_id, std::uint64_t generation, const TopicOptions& options,
    const OperationId& operation_id) {
    if (implementation_->shutdown_requested_.load()) {
        return failed_completion<TopicInfo>(error(ErrorCode::InvalidState, "shutdown requested"));
    }
    if (implementation_->target_is_initializing(recording_id, generation)) {
        return failed_completion<TopicInfo>(error(ErrorCode::InvalidState, "task is initializing"));
    }
    CompletionSource<TopicInfo> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (!implementation_->submit_operation(
            source, [implementation = implementation_.get(), id, generation, options, operation_id,
                     source] {
                std::unique_lock<std::mutex> lock(implementation->state_mutex_);
                if (!implementation->state_.task ||
                    implementation->state_.task->recording_id != id ||
                    implementation->state_.task->generation != generation) {
                    source.set_error(error(ErrorCode::StaleRecording, "stale recording identity"));
                    return;
                }
                if (!implementation->node_) {
                    source.set_error(error(ErrorCode::Unavailable, "ROS node is not available"));
                    return;
                }
                if (implementation->state_.recorder_state != RecorderState::Recording &&
                    implementation->state_.recorder_state != RecorderState::Paused) {
                    source.set_error(
                        error(ErrorCode::InvalidState, "topics require an active task"));
                    return;
                }
                if (operation_id.empty()) {
                    source.set_error(
                        error(ErrorCode::InvalidArgument, "operation_id must not be empty"));
                    return;
                }
                if (implementation->active_topics_.size() >=
                    implementation->config_.max_active_topics) {
                    source.set_error(
                        error(ErrorCode::ResourceExhausted, "active topic limit reached"));
                    return;
                }
                const auto valid = validate_topic_options(options);
                if (!valid) {
                    source.set_error(valid.error());
                    return;
                }
                Implementation::PublisherQosSnapshot publishers;
                try {
                    publishers = implementation->publisher_qos_snapshot(
                        options.name, implementation->config_.discovery.max_profiles);
                } catch (const std::exception& exception) {
                    source.set_error(error(ErrorCode::Unavailable, exception.what()));
                    return;
                }
                const auto qos = select_qos(
                    options.qos, publishers.profiles, publishers.count, publishers.truncated);
                if (!qos) {
                    source.set_error(qos.error());
                    return;
                }
                for (const auto& item : implementation->active_topics_) {
                    if (item.second.info.name == options.name &&
                        item.second.info.type != options.type) {
                        source.set_error(error(
                            ErrorCode::TopicTypeConflict,
                            "active topic name has a different type"));
                        return;
                    }
                    if (item.second.info.name == options.name &&
                        item.second.info.type == options.type) {
                        source.set_error(error(ErrorCode::InvalidState, "topic is already active"));
                        return;
                    }
                }
                if (!implementation->topic_events_ ||
                    implementation->topic_events_->uncertainty_watermark()) {
                    source.set_error(error(
                        ErrorCode::EventCommitFailed, "topic event chain is uncertain",
                        Effects::None));
                    return;
                }
                const auto existing_operation =
                    implementation->topic_events_->operation_state(operation_id);
                if (!existing_operation) {
                    source.set_error(existing_operation.error());
                    return;
                }
                if (existing_operation.value() != TopicOperationState::NotFound) {
                    source.set_error(
                        error(ErrorCode::InvalidArgument, "operation_id has already been used"));
                    return;
                }
                const auto schema = resolve_schema(options.type);
                if (!schema) {
                    source.set_error(schema.error());
                    return;
                }
                Implementation::TopicSignature signature;
                signature.type = options.type;
                signature.serialization_format = "cdr";
                signature.schema_encoding = schema->encoding;
                signature.schema_sha256 = schema->sha256;
                const auto frozen_signature =
                    implementation->task_topic_signatures_.find(options.name);
                if (frozen_signature != implementation->task_topic_signatures_.end() &&
                    !frozen_signature->second.matches(signature)) {
                    source.set_error(error(
                        ErrorCode::TopicTypeConflict,
                        "topic name has a different task-local storage signature"));
                    return;
                }
                if (implementation->next_topic_id_ == std::numeric_limits<TopicId>::max()) {
                    source.set_error(
                        error(ErrorCode::ResourceExhausted, "topic id space exhausted"));
                    return;
                }
                const TopicId topic_id = implementation->next_topic_id_++;
                auto& epoch = implementation->topic_epochs_[options.name];
                if (epoch == std::numeric_limits<std::uint64_t>::max()) {
                    source.set_error(
                        error(ErrorCode::ResourceExhausted, "topic epoch space exhausted"));
                    return;
                }
                ++epoch;
                TopicInfo info;
                info.topic_id = topic_id;
                info.topic_epoch = epoch;
                info.name = options.name;
                info.type = options.type;
                info.qos = qos->qos;
                // Humble does not provide the standard RIHS type hash through the portable API
                // used here. Keep type_hash empty rather than substituting schema_sha256.
                info.schema_sha256 = schema->sha256;
                info.expected_hz = options.expected_hz;

                TopicEvent prepared;
                prepared.operation_id = operation_id;
                prepared.topic_id = info.topic_id;
                prepared.topic_epoch = info.topic_epoch;
                prepared.type = TopicEventType::AddPrepared;
                prepared.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count();
                const auto appended = implementation->topic_events_->append(std::move(prepared));
                if (!appended) {
                    source.set_error(appended.error());
                    return;
                }

                implementation->operation_effects_ = Effects::ConfirmedPartial;
                if (frozen_signature == implementation->task_topic_signatures_.end()) {
                    implementation->task_topic_signatures_.emplace(
                        options.name, std::move(signature));
                }
                const auto directory =
                    implementation->manifest_->task_directory() / "segment_000001";
                auto writer = implementation->writer_;
                lock.unlock();
                if (!implementation->writer_open_) {
                    ManifestData opening = implementation->manifest_->data();
                    if (opening.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
                        lock.lock();
                        implementation->state_.recorder_state = RecorderState::Failed;
                        implementation->state_.last_error = error(
                            ErrorCode::StorageError, "manifest revision exhausted",
                            Effects::ConfirmedPartial);
                        implementation->poisoned_ = implementation->persistence_poisoned();
                        source.set_error(*implementation->state_.last_error);
                        return;
                    }
                    SegmentInfo segment;
                    segment.segment_id = 1;
                    segment.state = SegmentLifecycleState::Writing;
                    segment.relative_path = "segment_000001";
                    segment.started_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count();
                    opening.segments.push_back(segment);
                    ++opening.manifest_revision;
                    const auto persisted_segment =
                        implementation->manifest_->commit(std::move(opening));
                    if (!persisted_segment) {
                        lock.lock();
                        implementation->state_.recorder_state = RecorderState::Failed;
                        implementation->state_.last_error = persisted_segment.error();
                        implementation->poisoned_ = implementation->persistence_poisoned();
                        auto failure = persisted_segment.error();
                        if (failure.effects == Effects::None) {
                            failure.effects = Effects::ConfirmedPartial;
                        }
                        source.set_error(std::move(failure));
                        return;
                    }
                    const auto opened = writer->open(directory);
                    if (!opened) {
                        lock.lock();
                        implementation->state_.recorder_state = RecorderState::Failed;
                        implementation->state_.last_error = opened.error();
                        implementation->state_.last_error->effects = Effects::ConfirmedPartial;
                        TopicEvent failed;
                        failed.operation_id = operation_id;
                        failed.topic_id = info.topic_id;
                        failed.topic_epoch = info.topic_epoch;
                        failed.type = TopicEventType::AddFailed;
                        failed.timestamp_ns =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
                        failed.error = opened.error();
                        const auto failure_event =
                            implementation->topic_events_->append(std::move(failed));
                        if (!failure_event) {
                            auto failure = failure_event.error();
                            if (failure.effects == Effects::None) {
                                failure.effects = Effects::ConfirmedPartial;
                            }
                            source.set_error(std::move(failure));
                        } else {
                            auto failure = opened.error();
                            failure.effects = Effects::ConfirmedPartial;
                            source.set_error(failure);
                        }
                        return;
                    }
                    implementation->writer_open_ = true;
                    lock.lock();
                    SegmentState state_segment;
                    state_segment.segment_id = 1;
                    state_segment.state = SegmentLifecycleState::Writing;
                    implementation->state_.segments.current = state_segment;
                    implementation->dispatch_segment_id_.store(1, std::memory_order_release);
                    lock.unlock();
                }
                const auto registered = writer->register_topic(info);
                if (!registered) {
                    lock.lock();
                    TopicEvent failed;
                    failed.operation_id = operation_id;
                    failed.topic_id = info.topic_id;
                    failed.topic_epoch = info.topic_epoch;
                    failed.type = TopicEventType::AddFailed;
                    failed.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
                    failed.error = registered.error();
                    const auto failure_event =
                        implementation->topic_events_->append(std::move(failed));
                    if (!failure_event) {
                        auto failure = failure_event.error();
                        if (failure.effects == Effects::None) {
                            failure.effects = Effects::ConfirmedPartial;
                        }
                        source.set_error(std::move(failure));
                    } else {
                        auto failure = registered.error();
                        failure.effects = Effects::ConfirmedPartial;
                        source.set_error(std::move(failure));
                    }
                    return;
                }

                auto ingress = std::make_shared<Implementation::Ingress>();
                auto admission = std::make_shared<AdmissionGate>();
                auto frozen_info = std::make_shared<TopicInfo>(info);
                auto counters = std::make_shared<Implementation::ActiveTopic::Counters>();
                const auto publisher_count = publishers.count;
                std::shared_ptr<rclcpp::GenericSubscription> subscription;
                try {
                    counters->publisher_count.store(publisher_count, std::memory_order_relaxed);
                    counters->publisher_observed_steady_ns.store(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count(),
                        std::memory_order_relaxed);
                    auto compatibility = TopicState::Compatibility::Unknown;
                    if (publisher_count != 0 && !publishers.truncated) {
                        compatibility = TopicState::Compatibility::Incompatible;
                        if (qos->all_compatible) {
                            compatibility = TopicState::Compatibility::Compatible;
                        }
                    }
                    counters->qos_compatible.store(compatibility, std::memory_order_relaxed);
                    ingress->queue = implementation->message_queue_;
                    subscription = implementation->node_->create_generic_subscription(
                        info.name, info.type, implementation->make_qos(info.qos),
                        [ingress, admission, pool = implementation->message_pool_,
                         disk = &implementation->disk_admission_, implementation, frozen_info,
                         pool_exhausted = &implementation->pool_exhausted_,
                         queue_full = &implementation->queue_full_, counters, generation,
                         topic_id = info.topic_id, topic_epoch = info.topic_epoch](
                            std::shared_ptr<rclcpp::SerializedMessage> message) {
                            // Acquire the producer lease before touching any
                            // Recorder-owned state. An executor may retain a
                            // callback after the subscription is detached.
                            std::shared_ptr<MessageQueue> queue;
                            {
                                std::lock_guard<std::mutex> ingress_lock(ingress->mutex);
                                if (!ingress->open) {
                                    counters->dropped.fetch_add(1, std::memory_order_relaxed);
                                    return;
                                }
                                ++ingress->producers;
                                queue = ingress->queue;
                            }
                            Implementation::Ingress::Producer producer(*ingress);
                            const auto admitted = admission->admit(generation, topic_epoch, *disk);
                            if (admitted != AdmissionResult::Accepted) {
                                if (admitted == AdmissionResult::DiskUnsafe) {
                                    implementation->producer_disk_event_.store(generation);
                                    implementation->queue_condition_.notify_one();
                                }
                                if (admitted == AdmissionResult::TimedDeadline) {
                                    implementation->queue_condition_.notify_one();
                                }
                                counters->dropped.fetch_add(1, std::memory_order_relaxed);
                                return;
                            }
                            counters->received.fetch_add(1, std::memory_order_relaxed);
                            counters->record_arrival(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
                            try {
                                if (!queue) {
                                    counters->dropped.fetch_add(1, std::memory_order_relaxed);
                                    return;
                                }
                                auto owned = pool->acquire();
                                if (!owned) {
                                    pool_exhausted->fetch_add(1, std::memory_order_relaxed);
                                    counters->dropped.fetch_add(1, std::memory_order_relaxed);
                                    return;
                                }
                                RecordedMessage recorded;
                                recorded.generation = generation;
                                recorded.topic_id = topic_id;
                                recorded.topic_epoch = topic_epoch;
                                recorded.payload_bytes = message->size();
                                recorded.receive_timestamp_ns =
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
                                recorded.topic = frozen_info;
                                recorded.written_counter = counters->written;
                                *owned = *message;
                                recorded.message = std::move(owned);
                                if (!queue->try_push(std::move(recorded))) {
                                    queue_full->fetch_add(1, std::memory_order_relaxed);
                                    counters->dropped.fetch_add(1, std::memory_order_relaxed);
                                }
                            } catch (...) {
                                counters->dropped.fetch_add(1, std::memory_order_relaxed);
                            }
                        });
                } catch (const std::exception& exception) {
                    lock.lock();
                    const auto subscription_error =
                        error(ErrorCode::Unavailable, exception.what(), Effects::ConfirmedPartial);
                    TopicEvent failed;
                    failed.operation_id = operation_id;
                    failed.topic_id = info.topic_id;
                    failed.topic_epoch = info.topic_epoch;
                    failed.type = TopicEventType::AddFailed;
                    failed.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
                    failed.error = subscription_error;
                    const auto failure_event =
                        implementation->topic_events_->append(std::move(failed));
                    if (!failure_event) {
                        auto failure = failure_event.error();
                        if (failure.effects == Effects::None) {
                            failure.effects = Effects::ConfirmedPartial;
                        }
                        source.set_error(std::move(failure));
                    } else {
                        source.set_error(subscription_error);
                    }
                    return;
                }
                lock.lock();
                implementation->active_topics_.emplace(
                    info.topic_id,
                    Implementation::ActiveTopic{
                        info, std::move(subscription), ingress, admission, frozen_info, counters});
                TopicState state_topic;
                state_topic.info = info;
                state_topic.state = TopicRuntimeState::Adding;
                state_topic.publishers.count = publisher_count;
                state_topic.publishers.fresh = true;
                implementation->state_.topics.push_back(std::move(state_topic));
                TopicEvent added;
                added.operation_id = operation_id;
                added.topic_id = info.topic_id;
                added.topic_epoch = info.topic_epoch;
                added.type = TopicEventType::TopicAdded;
                added.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
                const auto added_event = implementation->topic_events_->append(std::move(added));
                if (!added_event) {
                    implementation->state_.topics.back().state = TopicRuntimeState::Failed;
                    auto failure = added_event.error();
                    if (failure.effects == Effects::None) {
                        failure.effects = Effects::ConfirmedPartial;
                    }
                    source.set_error(std::move(failure));
                    return;
                }
                const auto deadline_ns =
                    implementation->timed_deadline_steady_ns_.load(std::memory_order_acquire);
                const auto disk_snapshot = implementation->disk_admission_.read();
                const bool disk_fresh_and_safe = implementation->disk_snapshot_is_fresh_and_safe(
                    disk_snapshot, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
                const bool timed_deadline_reached =
                    deadline_ns != 0 && std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                                .count() >= deadline_ns;

                bool open_ingress =
                    implementation->state_.recorder_state == RecorderState::Recording;
                if (implementation->state_.pause.user_paused) {
                    open_ingress = false;
                }
                if (implementation->state_.pause.disk != DiskPauseReason::None) {
                    open_ingress = false;
                }
                if (!disk_fresh_and_safe) {
                    open_ingress = false;
                }
                if (timed_deadline_reached || implementation->shutdown_requested_.load()) {
                    open_ingress = false;
                }
                if (!disk_fresh_and_safe) {
                    implementation->producer_disk_event_.store(generation);
                    implementation->queue_condition_.notify_one();
                }
                if (open_ingress) {
                    {
                        std::lock_guard<std::mutex> ingress_lock(ingress->mutex);
                        ingress->open = true;
                    }
                    admission->open(generation, info.topic_epoch, deadline_ns);
                }
                implementation->state_.topics.back().state =
                    open_ingress ? TopicRuntimeState::Active : TopicRuntimeState::Armed;
                source.set_value(std::move(info));
            })) {
        source.set_error(error(ErrorCode::ResourceExhausted, "recorder mutation queue is full"));
    }
    return completion;
}

Completion<void> Recorder::remove_topic(
    std::string_view recording_id, std::uint64_t generation, TopicId topic_id,
    std::uint64_t topic_epoch, const OperationId& operation_id) {
    if (implementation_->shutdown_requested_.load()) {
        return failed_completion<void>(error(ErrorCode::InvalidState, "shutdown requested"));
    }
    if (implementation_->target_is_initializing(recording_id, generation)) {
        return failed_completion<void>(error(ErrorCode::InvalidState, "task is initializing"));
    }
    CompletionSource<void> source;
    const auto completion = source.completion();
    const std::string id(recording_id);
    if (!implementation_->submit_operation(
            source, [implementation = implementation_.get(), id, generation, topic_id, topic_epoch,
                     operation_id, source] {
                std::unique_lock<std::mutex> lock(implementation->state_mutex_);
                if (!implementation->state_.task ||
                    implementation->state_.task->recording_id != id ||
                    implementation->state_.task->generation != generation) {
                    source.set_error(error(ErrorCode::StaleRecording, "stale recording identity"));
                    return;
                }
                if (implementation->state_.recorder_state != RecorderState::Recording &&
                    implementation->state_.recorder_state != RecorderState::Paused) {
                    source.set_error(error(
                        ErrorCode::InvalidState, "topics can only be removed from an active task"));
                    return;
                }
                if (operation_id.empty()) {
                    source.set_error(
                        error(ErrorCode::InvalidArgument, "operation_id must not be empty"));
                    return;
                }
                const auto found = implementation->active_topics_.find(topic_id);
                if (found == implementation->active_topics_.end() ||
                    found->second.info.topic_epoch != topic_epoch) {
                    source.set_error(error(ErrorCode::StaleTopicEpoch, "stale topic identity"));
                    return;
                }
                if (!implementation->topic_events_ ||
                    implementation->topic_events_->uncertainty_watermark()) {
                    source.set_error(error(
                        ErrorCode::EventCommitFailed, "topic event chain is uncertain",
                        Effects::None));
                    return;
                }
                const auto existing_operation =
                    implementation->topic_events_->operation_state(operation_id);
                if (!existing_operation) {
                    source.set_error(existing_operation.error());
                    return;
                }
                if (existing_operation.value() != TopicOperationState::NotFound) {
                    source.set_error(
                        error(ErrorCode::InvalidArgument, "operation_id has already been used"));
                    return;
                }
                TopicEvent prepared;
                prepared.operation_id = operation_id;
                prepared.topic_id = topic_id;
                prepared.topic_epoch = topic_epoch;
                prepared.type = TopicEventType::RemovePrepared;
                prepared.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count();
                const auto appended = implementation->topic_events_->append(std::move(prepared));
                if (!appended) {
                    source.set_error(appended.error());
                    return;
                }
                implementation->operation_effects_ = Effects::ConfirmedPartial;
                for (auto& topic : implementation->state_.topics) {
                    if (topic.info.topic_id == topic_id) {
                        topic.state = TopicRuntimeState::Removing;
                    }
                }
                auto active = std::move(found->second);
                implementation->active_topics_.erase(found);
                active.admission->close();
                {
                    std::lock_guard<std::mutex> ingress_lock(active.ingress->mutex);
                    active.ingress->open = false;
                }
                lock.unlock();
                {
                    std::unique_lock<std::mutex> ingress_lock(active.ingress->mutex);
                    active.ingress->condition.wait(
                        ingress_lock, [&active] { return active.ingress->producers == 0; });
                }
                active.subscription.reset();
                lock.lock();
                {
                    std::lock_guard<std::mutex> snapshot_lock(implementation->snapshot_mutex_);
                    if (implementation->snapshot_) {
                        implementation->snapshot_->erase_topic(topic_id, topic_epoch);
                    }
                }
                TopicEvent removed;
                removed.operation_id = operation_id;
                removed.topic_id = topic_id;
                removed.topic_epoch = topic_epoch;
                removed.type = TopicEventType::TopicRemoved;
                removed.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                const auto removed_event =
                    implementation->topic_events_->append(std::move(removed));
                if (!removed_event) {
                    for (auto& topic : implementation->state_.topics) {
                        if (topic.info.topic_id == topic_id) {
                            topic.state = TopicRuntimeState::Failed;
                        }
                    }
                    auto failure = removed_event.error();
                    if (failure.effects == Effects::None) {
                        failure.effects = Effects::ConfirmedPartial;
                    }
                    source.set_error(std::move(failure));
                    return;
                }
                implementation->state_.topics.erase(
                    std::remove_if(
                        implementation->state_.topics.begin(), implementation->state_.topics.end(),
                        [topic_id](const TopicState& topic) {
                            return topic.info.topic_id == topic_id;
                        }),
                    implementation->state_.topics.end());
                implementation->state_.topics_removed_recent.push_back(active.info);
                ++implementation->state_.topics_removed_total;
                const auto capacity = implementation->config_.recent_removed_instances;
                if (implementation->state_.topics_removed_recent.size() > capacity) {
                    implementation->state_.topics_removed_recent.erase(
                        implementation->state_.topics_removed_recent.begin());
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
    const auto observed_at_steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count();
    if (implementation_->message_queue_) {
        snapshot.queue = implementation_->message_queue_->state();
    }
    if (implementation_->message_pool_) {
        snapshot.pool = implementation_->message_pool_->state();
    }
    if (implementation_->cleanup_) {
        snapshot.cleanup = implementation_->cleanup_->state();
        snapshot.cleanup.queued += implementation_->cleanup_retry_.size();
        snapshot.cleanup.pending += implementation_->cleanup_retry_.size();
        snapshot.cleanup.queue_full += implementation_->state_.cleanup.queue_full;
        if (implementation_->state_.cleanup.last_error) {
            snapshot.cleanup.last_error = implementation_->state_.cleanup.last_error;
        }
    }
    snapshot.dropped.pool.exhausted =
        implementation_->pool_exhausted_.load(std::memory_order_relaxed);
    snapshot.dropped.queue.full = implementation_->queue_full_.load(std::memory_order_relaxed);
    snapshot.dropped.writer.write_failure =
        implementation_->writer_write_failure_.load(std::memory_order_relaxed);
    for (const auto& entry : implementation_->active_topics_) {
        const auto dropped = entry.second.admission->counters();
        snapshot.dropped.admission.stale_identity += dropped.admission.stale_identity;
        snapshot.dropped.admission.not_accepting += dropped.admission.not_accepting;
        snapshot.dropped.admission.timed_deadline += dropped.admission.timed_deadline;
        snapshot.dropped.admission.disk_unsafe += dropped.admission.disk_unsafe;
        snapshot.dropped.admission.disk_snapshot_busy += dropped.admission.disk_snapshot_busy;
        for (auto& topic : snapshot.topics) {
            if (topic.info.topic_id != entry.first) {
                continue;
            }
            topic.messages.received =
                entry.second.counters->received.load(std::memory_order_relaxed);
            topic.messages.written =
                entry.second.counters->written->load(std::memory_order_relaxed);
            topic.messages.dropped = entry.second.counters->dropped.load(std::memory_order_relaxed);
            topic.publishers.count =
                entry.second.counters->publisher_count.load(std::memory_order_relaxed);
            topic.publishers.compatibility =
                entry.second.counters->qos_compatible.load(std::memory_order_relaxed);
            if (topic.publishers.count == 0) {
                topic.publishers.compatibility = TopicState::Compatibility::Unknown;
            }
            const auto observed_steady_ns =
                entry.second.counters->publisher_observed_steady_ns.load(std::memory_order_relaxed);
            topic.publishers.fresh = observed_steady_ns != 0 &&
                                     observed_at_steady_ns - observed_steady_ns <=
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             implementation_->config_.discovery.visibility_max_age)
                                             .count();
            const auto frequency = entry.second.counters->frequency(
                observed_at_steady_ns, topic.info.expected_hz,
                topic.state == TopicRuntimeState::Active &&
                    snapshot.recorder_state == RecorderState::Recording && topic.publishers.fresh &&
                    topic.publishers.count != 0);
            topic.frequency.actual_hz = frequency.actual_hz;
            topic.frequency.gap = frequency.gap;
            break;
        }
    }
    if (snapshot.segments.current) {
        snapshot.segments.current->written_messages =
            implementation_->segment_written_messages_.load(std::memory_order_relaxed);
        snapshot.segments.current->written_bytes =
            implementation_->segment_written_bytes_.load(std::memory_order_relaxed);
        snapshot.segments.current->has_live_messages =
            implementation_->segment_has_live_messages_.load(std::memory_order_relaxed);
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
        discovered.reserve(std::min(graph.size(), implementation_->config_.discovery.max_results));
        for (const auto& entry : graph) {
            if (discovered.size() == implementation_->config_.discovery.max_results) {
                break;
            }
            DiscoveredTopic topic;
            topic.name = entry.first;
            topic.types = entry.second;
            std::sort(topic.types.begin(), topic.types.end());
            topic.types.erase(
                std::unique(topic.types.begin(), topic.types.end()), topic.types.end());
            const auto publishers = implementation_->publisher_qos_snapshot(
                topic.name, implementation_->config_.discovery.max_profiles);
            topic.publishers.count = publishers.count;
            topic.publishers.profiles = publishers.profiles;
            topic.publishers.truncated = publishers.truncated;
            if (topic.types.size() > 1) {
                topic.recordability = TopicRecordability::TypeConflict;
            } else {
                const auto selection = select_qos(
                    QosOptions{}, topic.publishers.profiles, topic.publishers.count,
                    topic.publishers.truncated);
                if (!selection) {
                    return Result<std::vector<DiscoveredTopic>, Error>(unexpect, selection.error());
                }
                if (!selection->all_compatible) {
                    topic.recordability = TopicRecordability::QosIncompatible;
                }
            }
            discovered.push_back(std::move(topic));
        }
        std::sort(
            discovered.begin(), discovered.end(),
            [](const DiscoveredTopic& left, const DiscoveredTopic& right) {
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
    if (segment_limit == 0 || segment_limit > implementation_->config_.history.max_segment_limit) {
        return Result<TaskDetail, Error>(
            unexpect, error(ErrorCode::InvalidArgument, "invalid segment limit"));
    }
    if (!Implementation::valid_recording_id(recording_id)) {
        return Result<TaskDetail, Error>(
            unexpect, error(ErrorCode::InvalidArgument, "invalid recording_id"));
    }
    const auto directory = implementation_->config_.output_root / std::string(recording_id);
    std::error_code filesystem_error;
    const auto status = std::filesystem::symlink_status(directory, filesystem_error);
    if (filesystem_error && filesystem_error != std::errc::no_such_file_or_directory) {
        return Result<TaskDetail, Error>(
            unexpect, error(ErrorCode::StorageError, filesystem_error.message()));
    }
    if (filesystem_error || !std::filesystem::exists(status)) {
        return Result<TaskDetail, Error>(
            unexpect, error(ErrorCode::NotFound, "recording does not exist"));
    }
    if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
        return Result<TaskDetail, Error>(
            unexpect,
            error(ErrorCode::StorageError, "recording path is not a safe task directory"));
    }
    const auto manifest = Manifest::load(directory);
    if (!manifest) {
        return Result<TaskDetail, Error>(unexpect, manifest.error());
    }
    const auto recovery_evidence =
        Implementation::read_recovery_evidence(directory, manifest->data().task.recording_id);
    if (!recovery_evidence) {
        return Result<TaskDetail, Error>(unexpect, recovery_evidence.error());
    }
    TaskDetail detail;
    detail.task = manifest->data().task;
    detail.recovery.recovered = recovery_evidence->present;
    detail.recovery.has_findings = recovery_evidence->findings_available;
    detail.segments.offset = segment_offset;
    detail.segments.available = manifest->data().segments.size();
    if (detail.task.segment_history.removed_count >
        std::numeric_limits<std::uint64_t>::max() - detail.segments.available) {
        return Result<TaskDetail, Error>(
            unexpect, error(ErrorCode::StorageError, "segment history count overflow"));
    }
    detail.segments.total = detail.segments.available + detail.task.segment_history.removed_count;
    for (const auto& segment : manifest->data().segments) {
        if (segment.state == SegmentLifecycleState::Incomplete &&
            detail.recovery.incomplete_segments != std::numeric_limits<std::uint32_t>::max()) {
            ++detail.recovery.incomplete_segments;
        }
    }
    if (segment_offset < detail.segments.available) {
        const auto end =
            segment_offset + std::min(detail.segments.available - segment_offset, segment_limit);
        detail.segments.items.insert(
            detail.segments.items.end(), manifest->data().segments.begin() + segment_offset,
            manifest->data().segments.begin() + end);
        detail.segments.has_more = end < detail.segments.available;
    }
    return Result<TaskDetail, Error>(std::move(detail));
}

Result<TaskList, Error> Recorder::list_tasks(std::size_t offset, std::size_t limit) const {
    if (limit == 0) {
        return Result<TaskList, Error>(
            unexpect, error(ErrorCode::InvalidArgument, "limit must be positive"));
    }
    TaskList list;
    std::error_code filesystem_error;
    std::vector<std::string> candidate_names;
    std::vector<TaskInfo> candidates;
    if (!std::filesystem::exists(implementation_->config_.output_root, filesystem_error)) {
        if (filesystem_error) {
            return Result<TaskList, Error>(
                unexpect, error(ErrorCode::StorageError, filesystem_error.message()));
        }
        return Result<TaskList, Error>(std::move(list));
    }
    for (const auto& entry : std::filesystem::directory_iterator(
             implementation_->config_.output_root, filesystem_error)) {
        if (filesystem_error) {
            return Result<TaskList, Error>(
                unexpect, error(ErrorCode::StorageError, filesystem_error.message()));
        }
        if (!entry.is_directory() || entry.is_symlink()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (candidate_names.size() < implementation_->config_.history.max_task_scan_entries) {
            candidate_names.push_back(name);
            continue;
        }
        list.scan_truncated = true;
        const auto greatest = std::max_element(candidate_names.begin(), candidate_names.end());
        if (name < *greatest) {
            *greatest = name;
        }
    }
    std::sort(candidate_names.begin(), candidate_names.end());
    std::vector<TaskInfo> loaded;
    loaded.reserve(candidate_names.size());
    for (const auto& candidate_name : candidate_names) {
        ++list.scanned_entries;
        const auto manifest = Manifest::load(implementation_->config_.output_root / candidate_name);
        if (manifest) {
            loaded.push_back(manifest->data().task);
        }
    }
    candidates = std::move(loaded);
    std::sort(
        candidates.begin(), candidates.end(), [](const TaskInfo& left, const TaskInfo& right) {
            if (left.start_time_ns != right.start_time_ns) {
                return left.start_time_ns > right.start_time_ns;
            }
            return left.recording_id < right.recording_id;
        });
    if (offset < candidates.size()) {
        const auto end = offset + std::min(candidates.size() - offset, limit);
        list.items.insert(list.items.end(), candidates.begin() + offset, candidates.begin() + end);
        list.has_more = end < candidates.size();
    }
    return Result<TaskList, Error>(std::move(list));
}

Result<TopicOperationState, Error> Recorder::topic_operation_state(
    std::string_view recording_id, const OperationId& operation_id) const {
    if (!Implementation::valid_recording_id(recording_id)) {
        return Result<TopicOperationState, Error>(
            unexpect, error(ErrorCode::InvalidArgument, "invalid recording_id"));
    }
    {
        std::lock_guard<std::mutex> lock(implementation_->state_mutex_);
        if (implementation_->state_.task &&
            implementation_->state_.task->recording_id == recording_id &&
            implementation_->topic_events_) {
            return implementation_->topic_events_->operation_state(operation_id);
        }
    }
    const auto directory = implementation_->config_.output_root / std::string(recording_id);
    if (!std::filesystem::exists(directory)) {
        return Result<TopicOperationState, Error>(
            unexpect, error(ErrorCode::NotFound, "recording does not exist"));
    }
    const auto manifest = Manifest::load(directory);
    if (!manifest) {
        return Result<TopicOperationState, Error>(unexpect, manifest.error());
    }
    const auto events = TopicEvents::open(
        directory, manifest->data().task.recording_id, manifest->data().task.generation);
    if (!events) {
        return Result<TopicOperationState, Error>(unexpect, events.error());
    }
    return events->operation_state(operation_id);
}

Completion<void> Recorder::shutdown() {
    implementation_->request_shutdown();
    return implementation_->shutdown_completion_;
}

}  // namespace ros2_bag::recorder
