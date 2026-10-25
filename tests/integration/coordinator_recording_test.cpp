// End-to-end Humble/Jazzy smoke test for Coordinator dynamic-topic recording.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/serialization.hpp>
#include <rosbag2_cpp/reader.hpp>
#include <rosbag2_storage/storage_options.hpp>
#include <std_msgs/msg/string.hpp>

#include "ros2_bag/recording_coordinator.hpp"

namespace {

std::size_t message_count_in_segment(const std::filesystem::path& segment) {
    rosbag2_storage::StorageOptions options;
    options.uri = segment.string();
    options.storage_id = "mcap";
    rosbag2_cpp::Reader reader;
    reader.open(options);
    std::size_t count = 0;
    while (reader.has_next()) {
        static_cast<void>(reader.read_next());
        ++count;
    }
    return count;
}

void require_empty_segment(const std::filesystem::path& root, const std::string& task_id) {
    const std::filesystem::path task_directory = root / task_id;
    if (!std::filesystem::exists(task_directory / "manifest.json") ||
        !std::filesystem::exists(task_directory / "segment_000000")) {
        throw std::runtime_error("empty recording artifacts are missing");
    }
    if (message_count_in_segment(task_directory / "segment_000000") != 0) {
        throw std::runtime_error("empty recording contains a message");
    }
}

}  // namespace

int main(int argc, char** argv) {
    int message_count = 10;
    if (argc == 2) {
        try {
            message_count = std::stoi(argv[1]);
        } catch (const std::exception&) {
            std::cerr << "message count must be an integer\n";
            return 2;
        }
    }
    if (message_count <= 0) {
        std::cerr << "message count must be positive\n";
        return 2;
    }
    int topic_stress_cycles = 0;
    const char* const stress_cycles = std::getenv("ROS2_BAG_TEST_TOPIC_STRESS_CYCLES");
    if (stress_cycles != nullptr) {
        try {
            topic_stress_cycles = std::stoi(stress_cycles);
        } catch (const std::exception&) {
            std::cerr << "ROS2_BAG_TEST_TOPIC_STRESS_CYCLES must be an integer\n";
            return 2;
        }
        if (topic_stress_cycles <= 0) {
            std::cerr << "ROS2_BAG_TEST_TOPIC_STRESS_CYCLES must be positive\n";
            return 2;
        }
    }
    rclcpp::init(argc, argv);
    const std::filesystem::path root = "/tmp/ros2_bag_coordinator_recording_test";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    ros2_bag::RecorderConfig config;
    config.recording.output_root = root;
    config.storage.config_file.clear();
    config.status.frequency_window_sec = 0.2;
    config.status.frequency_bucket_ms = 10;
    config.pool.pool.mode = ros2_bag::PoolMode::kGlobalMutex;
    config.pool.pool.max_fallback_in_use = 16;
    auto recorder_node = std::make_shared<rclcpp::Node>("ros2_bag_coordinator_test_recorder");
    auto publisher_node = std::make_shared<rclcpp::Node>("ros2_bag_coordinator_test_publisher");
    std::size_t executor_threads = 1;
    const char* const configured_executor_threads = std::getenv("ROS2_BAG_TEST_EXECUTOR_THREADS");
    if (configured_executor_threads != nullptr) {
        try {
            const int parsed = std::stoi(configured_executor_threads);
            if (parsed <= 0) {
                throw std::runtime_error("executor thread count must be positive");
            }
            executor_threads = static_cast<std::size_t>(parsed);
        } catch (const std::exception&) {
            std::cerr << "ROS2_BAG_TEST_EXECUTOR_THREADS must be a positive integer\n";
            return 2;
        }
    }
    rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), executor_threads);
    executor.add_node(recorder_node);
    executor.add_node(publisher_node);
    std::thread spinning([&executor]() { executor.spin(); });
    int result = 0;
    try {
        ros2_bag::RecorderConfig unavailable_disk_config = config;
        unavailable_disk_config.recording.output_root = "/proc/ros2_bag_unavailable_output";
        ros2_bag::RecordingCoordinator unavailable_disk_coordinator(
            recorder_node,
            unavailable_disk_config);
        std::string unavailable_disk_error;
        if (unavailable_disk_coordinator.start("unavailable", &unavailable_disk_error) ||
            unavailable_disk_error.rfind("DISK_STATE_UNKNOWN", 0) != 0) {
            throw std::runtime_error("initial disk safety gate did not reject start");
        }

        ros2_bag::RecordingCoordinator coordinator(recorder_node, config);
        std::string error;
        if (!coordinator.start("run", &error)) throw std::runtime_error(error);
        ros2_bag::TopicInfo topic;
        topic.name = "/ros2_bag_coordinator_test";
        topic.type = "std_msgs/msg/String";
        topic.serialization_format = "cdr";
        topic.qos.depth = 10;
        topic.expected_hz = 100.0;
        const ros2_bag::TopicOperationResult added = coordinator.add_topic(topic, "add-test-topic");
        if (!added.ok) throw std::runtime_error(added.error_message);
        const ros2_bag::TopicOperationResult removed =
            coordinator.remove_topic(topic.name, "remove-test-topic");
        if (!removed.ok || removed.topic_id != added.topic_id ||
            removed.topic_epoch != added.topic_epoch) {
            throw std::runtime_error("topic remove lifecycle mismatch");
        }
        const ros2_bag::TopicOperationResult readded =
            coordinator.add_topic(topic, "readd-test-topic");
        if (!readded.ok || readded.topic_id != added.topic_id ||
            readded.topic_epoch != added.topic_epoch + 1) {
            throw std::runtime_error("topic readd lifecycle mismatch");
        }
        const ros2_bag::RecorderStatus removed_status = coordinator.status();
        if (removed_status.topics_removed_total != 1 || removed_status.topics_removed.size() != 1 ||
            removed_status.topics_removed[0].topic_epoch != added.topic_epoch ||
            removed_status.topics_removed[0].final_state != "REMOVED") {
            throw std::runtime_error("removed topic status history mismatch");
        }
        const auto publisher =
            publisher_node->create_publisher<std_msgs::msg::String>(topic.name, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std_msgs::msg::String message;
        message.data = "recorded";
        for (int index = 0; index < message_count; ++index) {
            publisher->publish(message);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int index = 0; index < 30; ++index) {
            publisher->publish(message);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const ros2_bag::RecorderStatus frequency_status = coordinator.status();
        if (frequency_status.topics.empty() || !frequency_status.topics[0].actual_hz ||
            *frequency_status.topics[0].actual_hz < 50.0 || !frequency_status.topics[0].gap ||
            *frequency_status.topics[0].gap || frequency_status.topics[0].counters.written == 0 ||
            frequency_status.topics[0].counters.write_errors != 0) {
            throw std::runtime_error("topic frequency snapshot is invalid");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        static_cast<void>(coordinator.status());
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        const ros2_bag::RecorderStatus gap_status = coordinator.status();
        if (gap_status.topics.empty() || !gap_status.topics[0].gap || !*gap_status.topics[0].gap) {
            throw std::runtime_error("topic gap was not detected");
        }
        const bool stopped = coordinator.stop(&error);
        if (!stopped) throw std::runtime_error(error);
        const std::filesystem::path manifest = root / "run" / "manifest.json";
        const std::filesystem::path bag = root / "run" / "segment_000000";
        if (!std::filesystem::exists(manifest) || !std::filesystem::exists(bag)) {
            throw std::runtime_error("recording artifacts are missing");
        }
        rosbag2_storage::StorageOptions options;
        options.uri = bag.string();
        options.storage_id = "mcap";
        rosbag2_cpp::Reader reader;
        reader.open(options);
        rclcpp::Serialization<std_msgs::msg::String> serializer;
        std::size_t count = 0;
        while (reader.has_next()) {
            const auto recorded = reader.read_next();
            if (!recorded || recorded->topic_name != topic.name) {
                throw std::runtime_error("recorded topic mismatch");
            }
            rclcpp::SerializedMessage serialized(*recorded->serialized_data);
            std_msgs::msg::String decoded;
            serializer.deserialize_message(&serialized, &decoded);
            if (decoded.data != message.data) {
                throw std::runtime_error("recorded payload mismatch");
            }
            ++count;
        }
        if (count == 0) throw std::runtime_error("MCAP contains no messages");
        if (!coordinator.start("restart", &error)) {
            throw std::runtime_error(error);
        }
        if (!coordinator.stop(&error)) {
            throw std::runtime_error(error);
        }
        if (!std::filesystem::exists(root / "restart" / "manifest.json")) {
            throw std::runtime_error("restart manifest is missing");
        }
        require_empty_segment(root, "restart");
        for (int cycle = 0; cycle < 100; ++cycle) {
            const std::string task_id = "generation-" + std::to_string(cycle);
            if (!coordinator.start(task_id, &error) || !coordinator.stop(&error) ||
                coordinator.status().state != ros2_bag::RecorderState::kIdle) {
                throw std::runtime_error("Stop-to-Start generation cycle failed: " + error);
            }
        }

        ros2_bag::RecorderConfig empty_topics_config = config;
        empty_topics_config.topics.allow_empty_topics = true;
        empty_topics_config.recording.output_root = root / "empty_topics_root";
        ros2_bag::RecordingCoordinator empty_topics_coordinator(recorder_node, empty_topics_config);
        if (!empty_topics_coordinator.start("allow-empty", &error) ||
            !empty_topics_coordinator.stop(&error)) {
            throw std::runtime_error("allow_empty_topics lifecycle failed: " + error);
        }
        require_empty_segment(empty_topics_config.recording.output_root, "allow-empty");

        ros2_bag::RecorderConfig empty_lifecycle_config = config;
        empty_lifecycle_config.recording.output_root = root / "empty_lifecycle_root";
        ros2_bag::RecordingCoordinator empty_lifecycle_coordinator(
            recorder_node,
            empty_lifecycle_config);
        if (!empty_lifecycle_coordinator.start("all-removed", &error)) {
            throw std::runtime_error(error);
        }
        const ros2_bag::TopicOperationResult empty_added =
            empty_lifecycle_coordinator.add_topic(topic, "add-empty-topic");
        if (!empty_added.ok ||
            !empty_lifecycle_coordinator.remove_topic(topic.name, "remove-empty-topic").ok ||
            !empty_lifecycle_coordinator.stop(&error)) {
            throw std::runtime_error("all-removed lifecycle failed: " + error);
        }
        require_empty_segment(empty_lifecycle_config.recording.output_root, "all-removed");

        ros2_bag::RecorderConfig paused_empty_config = config;
        paused_empty_config.recording.output_root = root / "paused_empty_root";
        ros2_bag::RecordingCoordinator paused_empty_coordinator(recorder_node, paused_empty_config);
        if (!paused_empty_coordinator.start("paused-empty", &error) ||
            !paused_empty_coordinator.add_topic(topic, "add-paused-empty-topic").ok ||
            !paused_empty_coordinator.pause(&error) || !paused_empty_coordinator.stop(&error)) {
            throw std::runtime_error("paused empty lifecycle failed: " + error);
        }
        require_empty_segment(paused_empty_config.recording.output_root, "paused-empty");

        ros2_bag::RecorderConfig registered_empty_config = config;
        registered_empty_config.recording.output_root = root / "registered_empty_root";
        ros2_bag::RecordingCoordinator registered_empty_coordinator(
            recorder_node,
            registered_empty_config);
        if (!registered_empty_coordinator.start("registered-empty", &error) ||
            !registered_empty_coordinator.add_topic(topic, "add-registered-empty-topic").ok ||
            !registered_empty_coordinator.stop(&error)) {
            throw std::runtime_error("registered empty lifecycle failed: " + error);
        }
        require_empty_segment(registered_empty_config.recording.output_root, "registered-empty");

        ros2_bag::RecorderConfig timed_config = config;
        timed_config.recording.mode = ros2_bag::RecordingMode::kTimed;
        timed_config.recording.output_root = root / "timed_root";
        ros2_bag::RecordingCoordinator timed_coordinator(recorder_node, timed_config);
        if (!timed_coordinator.start_timed("timed", std::chrono::milliseconds(100), &error)) {
            throw std::runtime_error(error);
        }
        bool timed_stopped = false;
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (timed_coordinator.status().state == ros2_bag::RecorderState::kIdle) {
                timed_stopped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!timed_stopped) {
            throw std::runtime_error("timed recording did not stop");
        }
        if (!std::filesystem::exists(
                timed_config.recording.output_root / "timed" / "manifest.json")) {
            throw std::runtime_error("timed manifest is missing");
        }
        ros2_bag::RecorderConfig disk_config = config;
        disk_config.recording.output_root = root / "disk_root";
        disk_config.resource.enabled = false;
        disk_config.resource.sample_interval_sec = 1;
        disk_config.disk.check_interval_sec = 1;
        disk_config.disk.max_sample_age_sec = 2;
        disk_config.disk.min_free_gb = 1000000000;
        ros2_bag::RecordingCoordinator disk_coordinator(recorder_node, disk_config);
        if (!disk_coordinator.start("disk", &error)) {
            throw std::runtime_error(error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if (disk_coordinator.status().state != ros2_bag::RecorderState::kPaused ||
            disk_coordinator.resume(&error)) {
            throw std::runtime_error("disk protection did not retain system pause");
        }
        if (!disk_coordinator.stop(&error)) {
            throw std::runtime_error(error);
        }

        std::atomic<unsigned int> disk_sample_count(0);
        ros2_bag::RecorderConfig auto_resume_config = config;
        auto_resume_config.recording.output_root = root / "auto_resume_root";
        auto_resume_config.resource.enabled = false;
        auto_resume_config.resource.sample_interval_sec = 1;
        auto_resume_config.disk.check_interval_sec = 1;
        auto_resume_config.disk.max_sample_age_sec = 2;
        auto_resume_config.disk.min_free_gb = 0;
        auto_resume_config.disk.auto_resume = true;
        auto_resume_config.disk.resume_stable_sec = 1;
        ros2_bag::RecordingCoordinator auto_resume_coordinator(
            recorder_node,
            auto_resume_config,
            ros2_bag::CleanupWorker::RemoveDirectory(),
            [&disk_sample_count]() {
                const unsigned int sample = disk_sample_count.fetch_add(1);
                ros2_bag::DiskSample result;
                result.total_bytes = 1000;
                result.available_bytes = 1000;
                result.sampled_at_ns = 1;
                result.last_attempt_at_ns = 1;
                result.state = sample == 1 ? ros2_bag::DiskProtectionState::kCritical
                                           : ros2_bag::DiskProtectionState::kNormal;
                return result;
            });
        if (!auto_resume_coordinator.start("auto-resume", &error)) {
            throw std::runtime_error(error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if (auto_resume_coordinator.status().state != ros2_bag::RecorderState::kPaused) {
            throw std::runtime_error("disk critical sample did not pause recording");
        }
        bool auto_resumed = false;
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (auto_resume_coordinator.status().state == ros2_bag::RecorderState::kRecording) {
                auto_resumed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!auto_resumed) {
            throw std::runtime_error("disk auto-resume did not restore recording");
        }
        if (!auto_resume_coordinator.stop(&error)) {
            throw std::runtime_error(error);
        }

        ros2_bag::RecorderConfig snapshot_config = config;
        snapshot_config.recording.mode = ros2_bag::RecordingMode::kRolling;
        snapshot_config.recording.segment_duration_sec = 1;
        snapshot_config.recording.retention_duration_sec = 60;
        snapshot_config.recording.output_root = root / "snapshot_root";
        snapshot_config.snapshot.max_total_bytes = 64;
        ros2_bag::TopicInfo snapshot_topic = topic;
        snapshot_topic.repeat_policy.enabled = true;
        snapshot_topic.repeat_policy.max_messages = 2;
        snapshot_topic.repeat_policy.max_bytes = 64;
        ros2_bag::RecordingCoordinator snapshot_coordinator(recorder_node, snapshot_config);
        if (!snapshot_coordinator.start("snapshot", &error) ||
            !snapshot_coordinator.add_topic(snapshot_topic, "add-snapshot-topic").ok) {
            throw std::runtime_error("snapshot setup failed: " + error);
        }
        for (int index = 0; index < 3; ++index) {
            publisher->publish(message);
        }
        bool snapshot_written = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const ros2_bag::RecorderStatus snapshot_status = snapshot_coordinator.status();
            if (!snapshot_status.topics.empty() &&
                snapshot_status.topics.front().counters.written >= 3) {
                snapshot_written = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!snapshot_written) {
            throw std::runtime_error("snapshot source messages were not written");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if (!snapshot_coordinator.stop(&error)) {
            throw std::runtime_error(error);
        }
        const std::filesystem::path snapshot_segment =
            snapshot_config.recording.output_root / "snapshot" / "segment_000001";
        const std::size_t snapshot_message_count = std::filesystem::exists(snapshot_segment)
                                                       ? message_count_in_segment(snapshot_segment)
                                                       : 0;
        if (snapshot_message_count != 2) {
            throw std::runtime_error(
                "snapshot replay message count is " + std::to_string(snapshot_message_count));
        }

        ros2_bag::RecorderConfig rolling_config = config;
        rolling_config.recording.mode = ros2_bag::RecordingMode::kRolling;
        rolling_config.recording.segment_duration_sec = 1;
        rolling_config.recording.retention_duration_sec = 1;
        rolling_config.recording.output_root = root / "rolling_root";
        ros2_bag::RecordingCoordinator rolling_coordinator(recorder_node, rolling_config);
        if (!rolling_coordinator.start("rolling", &error)) {
            throw std::runtime_error(error);
        }
        const ros2_bag::TopicOperationResult rolling_added =
            rolling_coordinator.add_topic(topic, "add-rolling-topic");
        if (!rolling_added.ok) {
            throw std::runtime_error(rolling_added.error_message);
        }
        for (int index = 0; index < 10; ++index) {
            publisher->publish(message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        for (int index = 0; index < 10; ++index) {
            publisher->publish(message);
        }
        const std::filesystem::path rolling_root = rolling_config.recording.output_root / "rolling";
        bool first_segment_removed = false;
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (!std::filesystem::exists(rolling_root / "segment_000000")) {
                first_segment_removed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!first_segment_removed) {
            throw std::runtime_error("rolling retention did not remove first segment");
        }
        const bool rolling_stopped = rolling_coordinator.stop(&error);
        if (!rolling_stopped) {
            throw std::runtime_error(error);
        }
        std::filesystem::path latest_segment;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(rolling_root)) {
            if (entry.is_directory() &&
                entry.path().filename().string().rfind("segment_", 0) == 0 &&
                (latest_segment.empty() || entry.path().filename() > latest_segment.filename())) {
                latest_segment = entry.path();
            }
        }
        if (std::filesystem::exists(rolling_root / "segment_000000") || latest_segment.empty()) {
            throw std::runtime_error("rolling retention state mismatch");
        }
        {
            rosbag2_cpp::Reader rolling_reader;
            options.uri = latest_segment.string();
            rolling_reader.open(options);
        }
        ros2_bag::RecorderConfig paused_rolling_config = rolling_config;
        paused_rolling_config.recording.output_root = root / "paused_rolling_root";
        ros2_bag::RecordingCoordinator paused_rolling_coordinator(
            recorder_node,
            paused_rolling_config);
        if (!paused_rolling_coordinator.start("paused-rolling", &error)) {
            throw std::runtime_error(error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if (!paused_rolling_coordinator.pause(&error)) {
            throw std::runtime_error(error);
        }
        const std::filesystem::path paused_rolling_root =
            paused_rolling_config.recording.output_root / "paused-rolling";
        bool paused_segment_removed = false;
        for (int attempt = 0; attempt < 175; ++attempt) {
            if (!std::filesystem::exists(paused_rolling_root / "segment_000000")) {
                paused_segment_removed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!paused_segment_removed) {
            throw std::runtime_error("paused rolling retention did not remove first segment");
        }
        if (!paused_rolling_coordinator.stop(&error)) {
            throw std::runtime_error(error);
        }

        std::atomic<bool> delayed_cleanup_started(false);
        ros2_bag::RecorderConfig delayed_cleanup_config = rolling_config;
        delayed_cleanup_config.recording.output_root = root / "delayed_cleanup_root";
        ros2_bag::RecordingCoordinator delayed_cleanup_coordinator(
            recorder_node,
            delayed_cleanup_config,
            [&delayed_cleanup_started](
                const std::filesystem::path& path,
                std::string* remove_error) {
                delayed_cleanup_started.store(true);
                std::this_thread::sleep_for(std::chrono::seconds(35));
                std::error_code remove_filesystem_error;
                std::filesystem::remove_all(path, remove_filesystem_error);
                if (remove_filesystem_error && remove_error != nullptr) {
                    *remove_error = remove_filesystem_error.message();
                }
                return !remove_filesystem_error;
            });
        if (!delayed_cleanup_coordinator.start("delayed-cleanup", &error) ||
            !delayed_cleanup_coordinator.add_topic(topic, "add-delayed-cleanup-topic").ok) {
            throw std::runtime_error("delayed cleanup setup failed: " + error);
        }
        for (int index = 0; index < 10; ++index) {
            publisher->publish(message);
        }
        bool cleanup_started = false;
        for (int attempt = 0; attempt < 350; ++attempt) {
            if (delayed_cleanup_started.load()) {
                cleanup_started = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!cleanup_started) {
            throw std::runtime_error("delayed cleanup did not start");
        }
        const auto status_started_at = std::chrono::steady_clock::now();
        static_cast<void>(delayed_cleanup_coordinator.status());
        if (std::chrono::steady_clock::now() - status_started_at > std::chrono::milliseconds(200)) {
            throw std::runtime_error("Coordinator status blocked on cleanup");
        }
        if (!delayed_cleanup_coordinator.stop(&error)) {
            throw std::runtime_error("delayed cleanup stop failed: " + error);
        }

        std::atomic<std::size_t> writer_failure_attempts(0);
        ros2_bag::RecorderConfig writer_failure_config = config;
        writer_failure_config.recording.output_root = root / "writer_failure_root";
        ros2_bag::RecordingCoordinator writer_failure_coordinator(
            recorder_node,
            writer_failure_config,
            ros2_bag::CleanupWorker::RemoveDirectory(),
            std::function<ros2_bag::DiskSample()>(),
            [&writer_failure_attempts](const ros2_bag::RecordedMessage&, std::string* write_error) {
                ++writer_failure_attempts;
                if (write_error != nullptr) {
                    *write_error = "injected writer failure";
                }
                return false;
            });
        if (!writer_failure_coordinator.start("writer-failure", &error) ||
            !writer_failure_coordinator.add_topic(topic, "add-writer-failure-topic").ok) {
            throw std::runtime_error("writer failure setup failed: " + error);
        }
        publisher->publish(message);
        bool writer_failed = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const ros2_bag::RecorderStatus writer_status = writer_failure_coordinator.status();
            if (writer_status.state == ros2_bag::RecorderState::kFailed) {
                writer_failed = writer_status.last_error == "injected writer failure" &&
                                writer_failure_attempts.load() == 1 &&
                                !writer_status.topics.empty() &&
                                writer_status.topics.front().counters.write_errors == 1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!writer_failed) {
            throw std::runtime_error("writer failure did not enter safe ERROR state");
        }
        if (!writer_failure_coordinator.stop(&error)) {
            throw std::runtime_error("writer failure stop did not finalize: " + error);
        }

        ros2_bag::RecorderConfig event_failure_config = config;
        event_failure_config.recording.output_root = root / "event_failure_root";
        ros2_bag::RecordingCoordinator event_failure_coordinator(
            recorder_node,
            event_failure_config,
            ros2_bag::CleanupWorker::RemoveDirectory(),
            std::function<ros2_bag::DiskSample()>(),
            ros2_bag::WriterAdapter::WriteOverride(),
            [](const ros2_bag::TopicEvent&, std::string* persistence_error) {
                if (persistence_error != nullptr) {
                    *persistence_error = "injected event persistence failure";
                }
                return false;
            });
        if (!event_failure_coordinator.start("event-failure", &error)) {
            throw std::runtime_error("event failure setup failed: " + error);
        }
        const ros2_bag::TopicOperationResult event_failure_result =
            event_failure_coordinator.add_topic(topic, "add-event-failure-topic");
        const ros2_bag::RecorderStatus event_failure_status = event_failure_coordinator.status();
        if (event_failure_result.ok || event_failure_result.error_code != "STORAGE_UNAVAILABLE" ||
            event_failure_status.state != ros2_bag::RecorderState::kFailed ||
            event_failure_status.last_error != "injected event persistence failure") {
            throw std::runtime_error("event persistence failure did not enter safe ERROR state");
        }
        if (!event_failure_coordinator.stop(&error)) {
            throw std::runtime_error("event failure stop did not finalize: " + error);
        }
        if (topic_stress_cycles > 0) {
            ros2_bag::RecorderConfig stress_config = config;
            stress_config.recording.output_root = root / "topic_stress_root";
            ros2_bag::RecordingCoordinator stress_coordinator(recorder_node, stress_config);
            if (!stress_coordinator.start("topic-stress", &error)) {
                throw std::runtime_error(error);
            }
            for (int cycle = 0; cycle < topic_stress_cycles; ++cycle) {
                const ros2_bag::TopicOperationResult stress_added =
                    stress_coordinator.add_topic(topic, "stress-add-" + std::to_string(cycle));
                if (!stress_added.ok) {
                    throw std::runtime_error(stress_added.error_message);
                }
                const ros2_bag::TopicOperationResult stress_removed =
                    stress_coordinator.remove_topic(
                        topic.name,
                        "stress-remove-" + std::to_string(cycle));
                if (!stress_removed.ok || stress_removed.topic_id != stress_added.topic_id ||
                    stress_removed.topic_epoch != stress_added.topic_epoch) {
                    throw std::runtime_error("topic stress lifecycle mismatch");
                }
            }
            if (!stress_coordinator.stop(&error)) {
                throw std::runtime_error(error);
            }
        }
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        result = 1;
    }
    executor.cancel();
    spinning.join();
    executor.remove_node(recorder_node);
    executor.remove_node(publisher_node);
    publisher_node.reset();
    recorder_node.reset();
    rclcpp::shutdown();
    std::filesystem::remove_all(root, filesystem_error);
    return result;
}
