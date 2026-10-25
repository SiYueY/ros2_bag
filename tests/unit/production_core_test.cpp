// Focused regression tests for formal pool, budget and move-only queue contracts.

#include <array>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>

#include <nlohmann/json.hpp>

#include "ros2_bag/manifest.hpp"
#include "ros2_bag/cleanup_worker.hpp"
#include "ros2_bag/config.hpp"
#include "ros2_bag/queue_budget.hpp"
#include "ros2_bag/recorded_message_pool.hpp"
#include "ros2_bag/recording_queue.hpp"
#include "ros2_bag/recovery.hpp"
#include "ros2_bag/resource_monitor.hpp"
#include "ros2_bag/snapshot_buffer.hpp"

namespace {

bool test_pool_fallback_and_stop() {
    ros2_bag::PoolConfig config;
    config.mode = ros2_bag::PoolMode::kGlobalMutex;
    config.capacity = 2;
    config.max_fallback_in_use = 1;
    config.local_capacity = 1;
    config.batch_size = 1;
    auto pool = std::make_shared<ros2_bag::PoolState>(config);
    auto first = pool->acquire();
    auto second = pool->acquire();
    auto fallback = pool->acquire();
    if (!first || !second || !fallback || pool->acquire()) return false;
    pool->begin_stop();
    if (pool->acquire()) return false;
    first.reset();
    second.reset();
    fallback.reset();
    return pool->try_finish_stop() &&
           pool->statistics().runtime_state == ros2_bag::PoolRuntimeState::kStopped;
}

bool test_queue_budget_rollback() {
    auto budget = std::make_shared<ros2_bag::QueueBudget>(32, 2);
    ros2_bag::BudgetLease first = budget->try_acquire(16);
    ros2_bag::BudgetLease second = budget->try_acquire(16);
    if (!first.valid() || !second.valid() || budget->try_acquire(1).valid()) return false;
    first.release(ros2_bag::BudgetReleaseReason::kNormal);
    second = ros2_bag::BudgetLease{};
    const ros2_bag::QueueBudgetStatistics statistics = budget->statistics();
    return statistics.used_bytes == 0 && statistics.used_messages == 0 &&
           statistics.release_normal_count == 1 && statistics.release_rollback_count == 1;
}

bool test_move_only_enqueue() {
    ros2_bag::PoolConfig config;
    config.mode = ros2_bag::PoolMode::kGlobalMutex;
    config.capacity = 1;
    config.max_fallback_in_use = 0;
    auto pool = std::make_shared<ros2_bag::PoolState>(config);
    auto budget = std::make_shared<ros2_bag::QueueBudget>(64, 1);
    ros2_bag::MessageQueue queue;
    ros2_bag::ProducerToken producer(&queue);
    ros2_bag::QueuedMessage message{pool->acquire(), budget->try_acquire(8)};
    if (!message.message || !message.budget.valid() || !producer.try_enqueue(&message))
        return false;
    ros2_bag::QueuedMessage received;
    if (!queue.try_dequeue(received) || !received.message || !received.budget.valid()) return false;
    received = ros2_bag::QueuedMessage{};
    pool->begin_stop();
    return pool->try_finish_stop() && budget->statistics().used_messages == 0;
}

bool test_manifest_atomic_commit() {
    const std::filesystem::path directory = "/tmp/ros2_bag_production_core_manifest";
    std::error_code filesystem_error;
    std::filesystem::remove_all(directory, filesystem_error);
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) return false;
    ros2_bag::RecordingManifest manifest;
    manifest.recording_id = "test-recording";
    manifest.mode = "normal";
    manifest.recording_state = "COMPLETED";
    manifest.secondary_errors.push_back("retention cleanup failed");
    manifest.segments.push_back({1, "segment_000000", "COMPLETE"});
    std::string error;
    const std::filesystem::path path = directory / "manifest.json";
    if (!ros2_bag::write_manifest_atomic(path, manifest, &error) ||
        std::filesystem::exists(path.string() + ".tmp")) {
        return false;
    }
    std::ifstream input(path);
    nlohmann::json document;
    input >> document;
    std::filesystem::remove_all(directory, filesystem_error);
    return document.at("schema_version") == 1 && document.at("recording_id") == "test-recording" &&
           document.at("recording_state") == "COMPLETED" &&
           document.at("secondary_errors").at(0) == "retention cleanup failed" &&
           document.at("segments").at(0).at("state") == "COMPLETE";
}

bool test_recovery_finding() {
    const std::filesystem::path root = "/tmp/ros2_bag_production_core_recovery";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    std::filesystem::create_directories(root / "incomplete", filesystem_error);
    ros2_bag::RecordingManifest manifest;
    manifest.recording_id = "incomplete";
    manifest.mode = "rolling";
    manifest.recording_state = "RECORDING";
    manifest.segments.push_back({1, "segment_000001", "WRITING"});
    std::string error;
    if (!ros2_bag::write_manifest_atomic(root / "incomplete" / "manifest.json", manifest, &error)) {
        return false;
    }
    const std::vector<ros2_bag::RecoveryFinding> findings =
        ros2_bag::inspect_recovery_candidates(root);
    std::filesystem::remove_all(root, filesystem_error);
    return findings.size() == 1 && findings[0].recording_id == "incomplete" &&
           findings[0].incomplete_segments.size() == 1;
}

bool test_deleting_recovery() {
    const std::filesystem::path root = "/tmp/ros2_bag_production_core_deleting";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    std::filesystem::create_directories(root / "task" / "segment_000000", filesystem_error);
    if (filesystem_error) {
        return false;
    }
    ros2_bag::RecordingManifest manifest;
    manifest.recording_id = "task";
    manifest.mode = "rolling";
    manifest.recording_state = "COMPLETED";
    manifest.segments.push_back({1, "segment_000000", "DELETING"});
    manifest.cleanup_pending.push_back("segment_000000");
    std::string error;
    if (!ros2_bag::write_manifest_atomic(root / "task" / "manifest.json", manifest, &error)) {
        return false;
    }
    if (!ros2_bag::recover_deleting_segments(root).empty() ||
        std::filesystem::exists(root / "task" / "segment_000000")) {
        return false;
    }
    std::ifstream input(root / "task" / "manifest.json");
    nlohmann::json document;
    input >> document;
    std::filesystem::remove_all(root, filesystem_error);
    return document.at("segments").at(0).at("state") == "REMOVED" &&
           document.at("cleanup_pending").empty();
}

bool test_incomplete_recovery() {
    const std::filesystem::path root = "/tmp/ros2_bag_production_core_incomplete";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    std::filesystem::create_directories(root / "task" / "segment_000000", filesystem_error);
    if (filesystem_error) {
        return false;
    }
    ros2_bag::RecordingManifest manifest;
    manifest.recording_id = "task";
    manifest.mode = "normal";
    manifest.recording_state = "RECORDING";
    manifest.segments.push_back({1, "segment_000000", "WRITING"});
    std::string error;
    if (!ros2_bag::write_manifest_atomic(root / "task" / "manifest.json", manifest, &error)) {
        return false;
    }
    {
        std::ofstream mcap(root / "task" / "segment_000000" / "evidence.mcap", std::ios::binary);
        const auto write_uint = [&mcap](std::uint64_t value, std::size_t byte_count) {
            for (std::size_t index = 0; index < byte_count; ++index) {
                mcap.put(static_cast<char>((value >> (index * 8U)) & 0xffU));
            }
        };
        mcap.write("\x89MCAP0\r\n", 8);
        mcap.put(static_cast<char>(0x03));
        write_uint(2, 8);
        write_uint(1, 2);
        mcap.put(static_cast<char>(0x04));
        write_uint(4, 8);
        write_uint(1, 2);
        write_uint(1, 2);
        mcap.write("\x89MCAP0\r\n", 8);
        if (!mcap) {
            return false;
        }
    }
    {
        std::ofstream events(root / "task" / "topic_events.jsonl");
        events << "{\"schema_version\":1,\"event\":\"TOPIC_ADD_PREPARED\","
                  "\"operation_id\":\"operation-1\"}\n";
    }
    if (!ros2_bag::recover_incomplete_recordings(root).empty()) {
        return false;
    }
    std::ifstream manifest_input(root / "task" / "manifest.json");
    std::ifstream report_input(root / "task" / "recovery.json");
    std::ifstream events_input(root / "task" / "topic_events.jsonl");
    nlohmann::json recovered_manifest;
    nlohmann::json report;
    manifest_input >> recovered_manifest;
    report_input >> report;
    bool uncertainty_event_found = false;
    for (std::string line; std::getline(events_input, line);) {
        const nlohmann::json event = nlohmann::json::parse(line);
        uncertainty_event_found =
            uncertainty_event_found || (event.value("event", "") == "TOPIC_ADD_UNCERTAIN" &&
                                        event.value("operation_id", "") == "operation-1");
    }
    std::filesystem::remove_all(root, filesystem_error);
    return recovered_manifest.at("recording_state") == "FAILED" &&
           recovered_manifest.at("last_error") == "UNEXPECTED_TERMINATION" &&
           recovered_manifest.at("segments").at(0).at("state") == "INCOMPLETE" &&
           report.at("reason") == "UNEXPECTED_TERMINATION" &&
           report.at("uncertain_operations").at(0) == "operation-1" &&
           report.at("segment_evidence").at(0).at("path") == "segment_000000" &&
           !report.at("segment_evidence").at(0).at("reader_opened").get<bool>() &&
           report.at("segment_evidence").at(0).at("raw_schema_count") == 1 &&
           report.at("segment_evidence").at(0).at("raw_channel_count") == 1 &&
           report.at("segment_evidence").at(0).at("raw_channel_schema_links_valid") == true &&
           report.at("segment_evidence").at(0).at("raw_mcap_error").is_null() &&
           uncertainty_event_found;
}

bool test_resource_monitor_disk_states() {
    ros2_bag::DiskConfig config;
    config.unknown_after_failures = 1;
    config.max_sample_age_sec = 10;
    ros2_bag::ResourceConfig resource_config;
    ros2_bag::ResourceMonitor valid_monitor("/tmp", config, resource_config);
    const ros2_bag::DiskSample valid = valid_monitor.sample();
    if (valid.state == ros2_bag::DiskProtectionState::kUnknown || valid.total_bytes == 0 ||
        valid.sampled_at_ns == 0 || !valid_monitor.latest_process().sampling_enabled ||
        !valid_monitor.latest_process().valid) {
        return false;
    }
    ros2_bag::ResourceMonitor invalid_monitor("/path/that/does/not/exist", config, resource_config);
    const ros2_bag::DiskSample invalid = invalid_monitor.sample();
    return invalid.state == ros2_bag::DiskProtectionState::kUnknown && invalid.sampled_at_ns == 0 &&
           invalid.last_attempt_at_ns != 0;
}

bool test_cleanup_worker() {
    const std::filesystem::path directory = "/tmp/ros2_bag_production_core_cleanup";
    std::error_code filesystem_error;
    std::filesystem::remove_all(directory, filesystem_error);
    std::filesystem::create_directories(directory / "segment", filesystem_error);
    if (filesystem_error) {
        return false;
    }
    std::mutex mutex;
    std::condition_variable completion;
    bool called = false;
    bool removed = false;
    ros2_bag::CleanupWorker worker(
        1,
        [&mutex,
         &completion,
         &called,
         &removed](const std::filesystem::path&, bool success, const std::string&) {
            std::lock_guard<std::mutex> lock(mutex);
            called = true;
            removed = success;
            completion.notify_one();
        });
    if (!worker.enqueue(directory / "segment")) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex);
    if (!completion.wait_for(lock, std::chrono::seconds(5), [&called]() { return called; })) {
        return false;
    }
    lock.unlock();
    worker.shutdown();
    const bool absent = !std::filesystem::exists(directory / "segment");
    std::filesystem::remove_all(directory, filesystem_error);
    return removed && absent;
}

bool test_cleanup_worker_failure() {
    std::mutex mutex;
    std::condition_variable completion;
    bool called = false;
    bool succeeded = true;
    std::string completion_error;
    ros2_bag::CleanupWorker worker(
        1,
        [&mutex, &completion, &called, &succeeded, &completion_error](
            const std::filesystem::path&,
            bool success,
            const std::string& error) {
            std::lock_guard<std::mutex> lock(mutex);
            called = true;
            succeeded = success;
            completion_error = error;
            completion.notify_one();
        },
        [](const std::filesystem::path&, std::string* error) {
            *error = "injected cleanup failure";
            return false;
        });
    if (!worker.enqueue("/tmp/ros2_bag_cleanup_failure")) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex);
    if (!completion.wait_for(lock, std::chrono::seconds(5), [&called]() { return called; })) {
        return false;
    }
    return !succeeded && completion_error == "injected cleanup failure";
}

bool test_cleanup_worker_timeout() {
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    bool completed = false;
    ros2_bag::CleanupWorker worker(
        1,
        [&mutex, &condition, &completed](const std::filesystem::path&, bool, const std::string&) {
            std::lock_guard<std::mutex> lock(mutex);
            completed = true;
            condition.notify_one();
        },
        [&mutex, &condition, &started, &release](const std::filesystem::path&, std::string*) {
            std::unique_lock<std::mutex> lock(mutex);
            started = true;
            condition.notify_one();
            condition.wait(lock, [&release]() { return release; });
            return true;
        });
    const std::filesystem::path path("/tmp/ros2_bag_cleanup_timeout");
    bool success = worker.enqueue(path);
    {
        std::unique_lock<std::mutex> lock(mutex);
        success = success && condition.wait_for(lock, std::chrono::seconds(5), [&started]() {
            return started;
        });
    }
    if (success) {
        const std::optional<std::filesystem::path> timeout =
            worker.take_timed_out_path(std::chrono::seconds(0));
        success =
            timeout && *timeout == path && !worker.take_timed_out_path(std::chrono::seconds(0));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_one();
    std::unique_lock<std::mutex> lock(mutex);
    return success &&
           condition.wait_for(lock, std::chrono::seconds(5), [&completed]() { return completed; });
}

bool test_queue_overflow_policy_validation() {
    ros2_bag::RecorderConfig config;
    config.queue.overflow_policy = "drop_oldest";
    try {
        ros2_bag::validate_recorder_config(&config);
    } catch (const ros2_bag::ConfigError&) {
        return true;
    }
    return false;
}

bool test_pool_payload_cache_validation() {
    ros2_bag::RecorderConfig config;
    config.pool.max_cached_payload_bytes = 1;
    try {
        ros2_bag::validate_recorder_config(&config);
    } catch (const ros2_bag::ConfigError&) {
        return true;
    }
    return false;
}

bool test_config_relative_storage_path() {
    const std::filesystem::path directory = "/tmp/ros2_bag_production_core_config";
    std::error_code filesystem_error;
    std::filesystem::remove_all(directory, filesystem_error);
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        return false;
    }
    const std::filesystem::path config_path = directory / "recorder.yaml";
    {
        std::ofstream output(config_path);
        output << "storage:\n  config_file: mcap.yaml\n";
    }
    const ros2_bag::RecorderConfig config = ros2_bag::load_recorder_config(config_path);
    std::filesystem::remove_all(directory, filesystem_error);
    return config.storage.config_file ==
           std::filesystem::absolute(config_path).parent_path() / "mcap.yaml";
}

bool test_config_rejects_unknown_key() {
    const std::filesystem::path directory = "/tmp/ros2_bag_production_core_unknown_config";
    std::error_code filesystem_error;
    std::filesystem::remove_all(directory, filesystem_error);
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        return false;
    }
    const std::filesystem::path config_path = directory / "recorder.yaml";
    {
        std::ofstream output(config_path);
        output << "queue:\n  unsupported_policy: true\n";
    }
    bool rejected = false;
    try {
        static_cast<void>(ros2_bag::load_recorder_config(config_path));
    } catch (const ros2_bag::ConfigError&) {
        rejected = true;
    }
    std::filesystem::remove_all(directory, filesystem_error);
    return rejected;
}

bool test_snapshot_buffer_budgets_and_replay() {
    ros2_bag::SnapshotBuffer snapshot(7);
    auto topic_one = std::make_shared<ros2_bag::TopicInfo>();
    topic_one->id = 1;
    topic_one->name = "/one";
    topic_one->type = "std_msgs/msg/String";
    topic_one->repeat_policy.enabled = true;
    topic_one->repeat_policy.max_messages = 2;
    topic_one->repeat_policy.max_bytes = 6;
    auto topic_two = std::make_shared<ros2_bag::TopicInfo>(*topic_one);
    topic_two->id = 2;
    topic_two->name = "/two";
    auto make_message = [](const std::shared_ptr<const ros2_bag::TopicInfo>& topic,
                           std::uint64_t epoch,
                           std::int64_t timestamp) {
        ros2_bag::RecordedMessage message;
        message.topic_id = topic->id;
        message.topic_epoch = epoch;
        message.topic_info = topic;
        message.serialized_message = std::make_shared<rclcpp::SerializedMessage>(3);
        message.payload_bytes = 3;
        message.recv_timestamp_ns = timestamp;
        message.send_timestamp_ns = timestamp;
        message.timestamp_source = ros2_bag::TimestampSource::kReceive;
        return message;
    };
    snapshot.add(make_message(topic_one, 1, 1));
    snapshot.add(make_message(topic_one, 1, 2));
    snapshot.add(make_message(topic_two, 1, 3));
    if (snapshot.used_bytes() != 6 || snapshot.message_count() != 2) {
        return false;
    }
    snapshot.remove_topic(topic_one->id, 1);
    if (snapshot.used_bytes() != 3 || snapshot.message_count() != 1) {
        return false;
    }
    bool replayed = false;
    const bool result = snapshot.replay([&replayed](const ros2_bag::RecordedMessage& message) {
        replayed = message.is_replayed && message.topic_id == 2 && message.recv_timestamp_ns == 3;
        return true;
    });
    return result && replayed;
}

}  // namespace

int main() {
    try {
        if (!test_pool_fallback_and_stop() || !test_queue_budget_rollback() ||
            !test_move_only_enqueue() || !test_manifest_atomic_commit() ||
            !test_recovery_finding() || !test_deleting_recovery() || !test_incomplete_recovery() ||
            !test_resource_monitor_disk_states() || !test_cleanup_worker() ||
            !test_cleanup_worker_failure() || !test_cleanup_worker_timeout() ||
            !test_queue_overflow_policy_validation() || !test_pool_payload_cache_validation() ||
            !test_config_relative_storage_path() || !test_config_rejects_unknown_key() ||
            !test_snapshot_buffer_budgets_and_replay()) {
            std::cerr << "production core contract failed\n";
            return 1;
        }
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
