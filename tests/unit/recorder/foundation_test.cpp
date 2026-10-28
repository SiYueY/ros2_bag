#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "ros2_bag/recorder/completion.hpp"
#include "ros2_bag/recorder/admission.hpp"
#include "ros2_bag/recorder/cleanup.hpp"
#include "ros2_bag/recorder/config.hpp"
#include "ros2_bag/recorder/message_pool.hpp"
#include "ros2_bag/recorder/message_queue.hpp"
#include "ros2_bag/recorder/manifest.hpp"
#include "ros2_bag/recorder/resource_monitor.hpp"
#include "ros2_bag/recorder/schema.hpp"
#include "ros2_bag/recorder/recorder.hpp"
#include "ros2_bag/recorder/snapshot.hpp"
#include "ros2_bag/recorder/topic_events.hpp"

namespace {

int failures = 0;

void check(bool value, const char* expression, int line) {
    if (!value) {
        ++failures;
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void test_completion() {
    ros2_bag::recorder::CompletionSource<int> source;
    const auto completion = source.completion();
    CHECK(!completion.done());
    CHECK(!completion.wait_for(std::chrono::milliseconds(0)));
    source.set_value(42);
    CHECK(completion.done());
    CHECK(completion.wait_for(std::chrono::milliseconds(0)));
    CHECK(completion.result().has_value());
    CHECK(completion.result()->value() == 42);
}

void test_config() {
    ros2_bag::recorder::RecorderConfig config;
    CHECK(ros2_bag::recorder::validate_config(config).has_value());
    CHECK(ros2_bag::recorder::message_pool_capacity(config).value() == 81920U);

    ros2_bag::recorder::StartOptions options;
    options.recording_id = "valid-id";
    options.mode = ros2_bag::recorder::TaskMode::Rolling;
    const auto task_config = ros2_bag::recorder::make_task_config(config, options);
    CHECK(task_config.has_value());
    CHECK(task_config->segment_duration == config.rolling_segment_duration);

    options.mode = ros2_bag::recorder::TaskMode::Timed;
    CHECK(!ros2_bag::recorder::make_task_config(config, options).has_value());
}

void test_pool() {
    ros2_bag::recorder::MessagePool pool(1);
    auto message = pool.acquire();
    CHECK(message != nullptr);
    CHECK(pool.acquire() == nullptr);
    CHECK(pool.state().in_use == 1U);
    pool.begin_stop();
    CHECK(pool.acquire() == nullptr);
    CHECK(!pool.wait_stopped_for(std::chrono::milliseconds(0)));
    message.reset();
    CHECK(pool.wait_stopped_for(std::chrono::milliseconds(0)));
    CHECK(pool.state().in_use == 0U);
}

void test_queue_and_disk_snapshot() {
    ros2_bag::recorder::MessageQueue queue(1, 10);
    ros2_bag::recorder::RecordedMessage first;
    first.payload_bytes = 6;
    CHECK(queue.try_push(first));
    CHECK(!queue.try_push(first));
    CHECK(queue.state().used_messages == 1U);
    CHECK(queue.state().used_bytes == 6U);
    const auto popped = queue.pop_for(std::chrono::milliseconds(0));
    CHECK(popped.has_value());
    CHECK(queue.state().used_bytes == 0U);
    queue.close();
    CHECK(!queue.pop_for(std::chrono::milliseconds(0)).has_value());

    ros2_bag::recorder::DiskAdmissionState disk;
    disk.publish(ros2_bag::recorder::DiskSafetyState::Normal, 123);
    const auto snapshot = disk.read();
    CHECK(snapshot.has_value());
    CHECK(snapshot->state == ros2_bag::recorder::DiskSafetyState::Normal);
    CHECK(snapshot->valid_until_steady_ns == 123);
}

void test_rolling_snapshot() {
    ros2_bag::recorder::Snapshot snapshot({2, 10, 10, std::nullopt});
    ros2_bag::recorder::RecordedMessage first;
    first.generation = 7;
    first.topic_id = 1;
    first.topic_epoch = 1;
    first.payload_bytes = 6;
    const auto now = std::chrono::steady_clock::now();
    snapshot.push_written_live(first, now);
    ros2_bag::recorder::RecordedMessage second = first;
    second.topic_id = 2;
    second.payload_bytes = 6;
    snapshot.push_written_live(second, now);
    CHECK(snapshot.total_bytes() == 6U);
    const auto replay = snapshot.replay_for(7);
    CHECK(replay.size() == 1U);
    CHECK(replay.front().topic_id == 2U);
    CHECK(replay.front().replayed);
    snapshot.push_written_live(replay.front(), now);
    CHECK(snapshot.total_bytes() == 6U);
}

void test_manifest_commit() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_manifest_foundation_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    ros2_bag::recorder::ManifestData data;
    data.manifest_revision = 1;
    data.task.recording_id = "manifest-test";
    data.task.generation = 3;
    data.task.state = ros2_bag::recorder::TaskState::Initializing;
    const auto created = ros2_bag::recorder::Manifest::create(root, data);
    CHECK(created.has_value());
    auto next = data;
    next.manifest_revision = 2;
    next.task.state = ros2_bag::recorder::TaskState::Recording;
    auto manifest = std::move(created).value();
    CHECK(manifest.commit(next).has_value());
    const auto loaded = ros2_bag::recorder::Manifest::load(root);
    CHECK(loaded.has_value());
    CHECK(loaded->data().manifest_revision == 2U);
    CHECK(loaded->data().task.state == ros2_bag::recorder::TaskState::Recording);
    std::filesystem::remove_all(root, ignored);
}

void test_topic_events() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_events_foundation_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    auto events = ros2_bag::recorder::TopicEvents::open(root, "events-test", 1).value();
    ros2_bag::recorder::TopicEvent prepared;
    prepared.operation_id = "operation-1";
    prepared.topic_id = 4;
    prepared.topic_epoch = 2;
    prepared.type = ros2_bag::recorder::TopicEventType::AddPrepared;
    CHECK(events.append(prepared).value() == 1U);
    CHECK(events.operation_state("operation-1").value() == ros2_bag::recorder::TopicOperationState::Uncertain);
    prepared.type = ros2_bag::recorder::TopicEventType::TopicAdded;
    CHECK(events.append(prepared).value() == 2U);
    CHECK(events.operation_state("operation-1").value() == ros2_bag::recorder::TopicOperationState::ConfirmedSuccess);
    CHECK(events.operation_state("missing").value() == ros2_bag::recorder::TopicOperationState::NotFound);
    std::filesystem::remove_all(root, ignored);
}

void test_recorder_state_machine() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_recorder_state_machine_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    ros2_bag::recorder::RecorderConfig config;
    config.output_root = root;
    ros2_bag::recorder::Recorder recorder(config);
    const auto missing = recorder.get_task("missing-task");
    CHECK(!missing.has_value());
    CHECK(missing.error().code == ros2_bag::recorder::ErrorCode::NotFound);
    ros2_bag::recorder::StartOptions options;
    options.recording_id = "task-one";
    const auto started = recorder.start(options);
    CHECK(started.wait_for(std::chrono::milliseconds(1000)));
    CHECK(started.result()->has_value());
    const auto task = started.result()->value();
    CHECK(recorder.state().recorder_state == ros2_bag::recorder::RecorderState::Recording);
    const auto paused = recorder.pause(task.recording_id, task.generation);
    CHECK(paused.wait_for(std::chrono::milliseconds(1000)));
    CHECK(paused.result()->has_value());
    CHECK(recorder.state().recorder_state == ros2_bag::recorder::RecorderState::Paused);
    const auto resumed = recorder.resume(task.recording_id, task.generation);
    CHECK(resumed.wait_for(std::chrono::milliseconds(1000)));
    CHECK(resumed.result()->has_value());
    const auto stopped = recorder.stop(task.recording_id, task.generation);
    CHECK(stopped.wait_for(std::chrono::milliseconds(1000)));
    CHECK(stopped.result()->has_value());
    CHECK(recorder.state().recorder_state == ros2_bag::recorder::RecorderState::Idle);
    CHECK(recorder.get_task(task.recording_id).has_value());
    const auto shutdown_one = recorder.shutdown();
    const auto shutdown_two = recorder.shutdown();
    CHECK(shutdown_one.wait_for(std::chrono::milliseconds(1000)));
    CHECK(shutdown_two.done());
    CHECK(shutdown_one.result()->has_value());
    CHECK(recorder.state().shutting_down);
    std::filesystem::remove_all(root, ignored);
}

void test_timed_task() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_timed_task_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    ros2_bag::recorder::RecorderConfig config;
    config.output_root = root;
    ros2_bag::recorder::Recorder recorder(config);
    ros2_bag::recorder::StartOptions options;
    options.recording_id = "timed-task";
    options.mode = ros2_bag::recorder::TaskMode::Timed;
    options.duration = std::chrono::milliseconds(10);
    const auto started = recorder.start(options);
    CHECK(started.wait_for(std::chrono::milliseconds(1000)));
    CHECK(started.result()->has_value());
    for (int attempt = 0; attempt < 100 && recorder.state().recorder_state != ros2_bag::recorder::RecorderState::Idle; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(recorder.state().recorder_state == ros2_bag::recorder::RecorderState::Idle);
    CHECK(recorder.shutdown().wait_for(std::chrono::milliseconds(1000)));
    std::filesystem::remove_all(root, ignored);
}

void test_cleanup() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_cleanup_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    std::atomic<bool> called{false};
    std::atomic<bool> succeeded{false};
    ros2_bag::recorder::Cleanup cleanup(1, std::chrono::seconds(1), [&](ros2_bag::recorder::DeleteResult result) {
        succeeded.store(!result.error.has_value());
        called.store(true);
    });
    CHECK(cleanup.enqueue({"cleanup", 1, root, 4}));
    for (int attempt = 0; attempt < 100 && !called.load(); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(called.load());
    CHECK(succeeded.load());
    CHECK(!std::filesystem::exists(root));
    cleanup.stop();
}

void test_topic_validation() {
    ros2_bag::recorder::TopicOptions options;
    options.name = "/camera";
    options.type = "sensor_msgs/msg/Image";
    CHECK(ros2_bag::recorder::validate_topic_options(options).has_value());
    const auto automatic = ros2_bag::recorder::normalize_qos(options.qos);
    CHECK(automatic.has_value());
    CHECK(automatic->mode == ros2_bag::recorder::QosMode::Explicit);
    CHECK(automatic->depth == std::optional<std::uint32_t>(10));
    options.expected_hz = 0.0;
    CHECK(!ros2_bag::recorder::validate_topic_options(options).has_value());
    options.expected_hz.reset();
    ros2_bag::recorder::QosOptions invalid;
    invalid.mode = ros2_bag::recorder::QosMode::Explicit;
    invalid.history = ros2_bag::recorder::History::KeepAll;
    options.qos = invalid;
    CHECK(!ros2_bag::recorder::validate_topic_options(options).has_value());
}

void test_admission_gate() {
    ros2_bag::recorder::DiskAdmissionState disk;
    const auto now = std::chrono::steady_clock::now();
    const auto expiry = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            (now + std::chrono::seconds(1)).time_since_epoch())
                            .count();
    disk.publish(ros2_bag::recorder::DiskSafetyState::Normal, expiry);
    ros2_bag::recorder::AdmissionGate gate;
    gate.open(3, 4);
    CHECK(gate.admit(3, 4, disk, now) == ros2_bag::recorder::AdmissionResult::Accepted);
    CHECK(gate.admit(2, 4, disk, now) == ros2_bag::recorder::AdmissionResult::StaleIdentity);
    gate.open(3, 4, now + std::chrono::milliseconds(1));
    CHECK(gate.admit(3, 4, disk, now + std::chrono::milliseconds(1)) ==
          ros2_bag::recorder::AdmissionResult::TimedDeadline);
    CHECK(gate.admit(3, 4, disk, now) == ros2_bag::recorder::AdmissionResult::NotAccepting);
    CHECK(gate.counters().timed_deadline == 1U);
}

void test_schema_identity() {
    const auto digest = ros2_bag::recorder::schema_sha256("int32 value\n");
    CHECK(digest.size() == 64U);
    CHECK(digest == ros2_bag::recorder::schema_sha256("int32 value\n"));
    CHECK(digest != ros2_bag::recorder::schema_sha256("int32 another\n"));
    CHECK(!ros2_bag::recorder::validate_type_support("").has_value());
}

}  // namespace

int main() {
    test_completion();
    test_config();
    test_pool();
    test_queue_and_disk_snapshot();
    test_rolling_snapshot();
    test_manifest_commit();
    test_topic_events();
    test_recorder_state_machine();
    test_timed_task();
    test_cleanup();
    test_topic_validation();
    test_admission_gate();
    test_schema_identity();
    if (failures != 0) {
        std::cerr << failures << " foundation test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
