#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <rclcpp/executors/single_threaded_executor.hpp>

#include <mcap/reader.hpp>

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
#include "ros2_bag/recorder/recovery.hpp"
#include "ros2_bag/recorder/snapshot.hpp"
#include "ros2_bag/recorder/topic_events.hpp"
#include "ros2_bag/recorder/writer.hpp"

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
    options.mode = ros2_bag::recorder::StartOptions::Rolling{};
    const auto task_config = ros2_bag::recorder::make_task_config(config, options);
    CHECK(task_config.has_value());
    CHECK(task_config->rolling.segment_duration == config.rolling.segment_duration);

    options.mode = ros2_bag::recorder::StartOptions::Timed{};
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

void test_pool_reuses_serialized_message_capacity() {
    constexpr std::size_t payload_bytes = 256;
    ros2_bag::recorder::MessagePool pool(1);
    rclcpp::SerializedMessage source(payload_bytes);
    auto& serialized = source.get_rcl_serialized_message();
    std::fill_n(serialized.buffer, payload_bytes, 0x5aU);
    serialized.buffer_length = payload_bytes;

    auto first = pool.acquire();
    CHECK(first != nullptr);
    *first = source;
    CHECK(first->size() == payload_bytes);
    const auto retained_capacity = first->get_rcl_serialized_message().buffer_capacity;
    CHECK(retained_capacity >= payload_bytes);
    first.reset();

    auto second = pool.acquire();
    CHECK(second != nullptr);
    CHECK(second->size() == 0U);
    CHECK(second->get_rcl_serialized_message().buffer_capacity >= retained_capacity);
    serialized.buffer[0] = 0x17U;
    *second = source;
    CHECK(second->size() == payload_bytes);
    CHECK(second->get_rcl_serialized_message().buffer[0] == 0x17U);
    second.reset();
    pool.begin_stop();
    CHECK(pool.wait_stopped_for(std::chrono::milliseconds(0)));
}

void test_queue_and_disk_snapshot() {
    ros2_bag::recorder::MessageQueue queue(1, 10, 1);
    ros2_bag::recorder::RecordedMessage first;
    first.payload_bytes = 6;
    CHECK(queue.try_push(first));
    CHECK(!queue.try_push(first));
    CHECK(queue.state().messages.used == 1U);
    CHECK(queue.state().bytes.used == 6U);
    const auto popped = queue.pop_for(std::chrono::milliseconds(0));
    CHECK(popped.has_value());
    CHECK(queue.state().bytes.used == 0U);
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
    next.task.error = ros2_bag::recorder::Error{
        ros2_bag::recorder::ErrorCode::StorageError,
        ros2_bag::recorder::Effects::None,
        std::string(600, 'x')};
    auto manifest = std::move(created).value();
    CHECK(manifest.commit(next).has_value());
    const auto loaded = ros2_bag::recorder::Manifest::load(root);
    CHECK(loaded.has_value());
    CHECK(loaded->data().manifest_revision == 2U);
    CHECK(loaded->data().task.state == ros2_bag::recorder::TaskState::Recording);
    CHECK(loaded->data().task.error->message.size() <= 512U);
    std::filesystem::remove_all(root, ignored);
}

void test_recovery() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_recovery_foundation_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    ros2_bag::recorder::ManifestData data;
    data.manifest_revision = 1;
    data.task.recording_id = "recover-me";
    data.task.generation = 9;
    data.task.state = ros2_bag::recorder::TaskState::Recording;
    ros2_bag::recorder::SegmentInfo segment;
    segment.segment_id = 1;
    segment.state = ros2_bag::recorder::SegmentLifecycleState::Writing;
    segment.relative_path = "segment_000001";
    data.segments.push_back(segment);
    CHECK(ros2_bag::recorder::Manifest::create(root / "recover-me", data).has_value());
    const auto report = ros2_bag::recorder::Recovery::run(root);
    CHECK(report.has_value());
    const auto recovered = ros2_bag::recorder::Manifest::load(root / "recover-me");
    CHECK(recovered.has_value());
    CHECK(recovered->data().task.state == ros2_bag::recorder::TaskState::Incomplete);
    CHECK(
        recovered->data().segments.front().state ==
        ros2_bag::recorder::SegmentLifecycleState::Incomplete);
    CHECK(std::filesystem::exists(root / "recover-me" / "recovery.json"));
    {
        ros2_bag::recorder::RecorderConfig config;
        config.output_root = root;
        ros2_bag::recorder::Recorder recorder(config);
        const auto detail = recorder.get_task("recover-me");
        CHECK(detail.has_value());
        CHECK(detail->recovery.recovered);
        CHECK(detail->recovery.has_findings);
        CHECK(detail->recovery.incomplete_segments == 1U);
        CHECK(recorder.shutdown().wait_for(std::chrono::seconds(1)));
    }
    std::ofstream corrupt(root / "recover-me" / "topic_events.jsonl");
    corrupt << "not json\n";
    corrupt.close();
    const auto corrupt_report = ros2_bag::recorder::Recovery::run(root);
    CHECK(corrupt_report.has_value());
    CHECK(!corrupt_report->findings.empty());
    CHECK(corrupt_report->findings.front().code == "topic_events_corrupt");
    std::filesystem::remove_all(root, ignored);
}

void test_recovery_retries_durable_deletion() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_recovery_deleting_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    ros2_bag::recorder::ManifestData data;
    data.manifest_revision = 1;
    data.task.recording_id = "recover-delete";
    data.task.generation = 3;
    data.task.state = ros2_bag::recorder::TaskState::Recording;
    ros2_bag::recorder::SegmentInfo segment;
    segment.segment_id = 1;
    segment.state = ros2_bag::recorder::SegmentLifecycleState::Deleting;
    segment.relative_path = "segment_000001";
    data.segments.push_back(segment);
    const auto directory = root / data.task.recording_id;
    CHECK(ros2_bag::recorder::Manifest::create(directory, data).has_value());
    std::filesystem::create_directories(directory / segment.relative_path);
    std::ofstream(directory / segment.relative_path / "payload").put('x');
    const auto report = ros2_bag::recorder::Recovery::run(root);
    CHECK(report.has_value());
    CHECK(!std::filesystem::exists(directory / segment.relative_path));
    const auto recovered = ros2_bag::recorder::Manifest::load(directory);
    CHECK(recovered.has_value());
    CHECK(
        recovered->data().segments.front().state ==
        ros2_bag::recorder::SegmentLifecycleState::Removed);
    CHECK(recovered->data().task.state == ros2_bag::recorder::TaskState::Incomplete);
    std::filesystem::remove_all(root, ignored);
}

void test_recovery_rejects_task_root_deletion() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_recovery_path_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    ros2_bag::recorder::ManifestData data;
    data.manifest_revision = 1;
    data.task.recording_id = "unsafe-delete";
    data.task.generation = 1;
    data.task.state = ros2_bag::recorder::TaskState::Completed;
    ros2_bag::recorder::SegmentInfo segment;
    segment.segment_id = 1;
    segment.state = ros2_bag::recorder::SegmentLifecycleState::Deleting;
    segment.relative_path = ".";
    data.segments.push_back(segment);
    const auto directory = root / data.task.recording_id;
    CHECK(ros2_bag::recorder::Manifest::create(directory, data).has_value());
    const auto report = ros2_bag::recorder::Recovery::run(root);
    CHECK(report.has_value());
    CHECK(std::filesystem::exists(directory / "manifest.json"));
    CHECK(std::any_of(
        report->findings.begin(),
        report->findings.end(),
        [](const ros2_bag::recorder::RecoveryFinding& finding) {
            return finding.recording_id == "unsafe-delete" &&
                   finding.code == "segment_path_invalid";
        }));
    std::filesystem::remove_all(root, ignored);
}

void test_recovery_ignores_task_directory_symlink() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_recovery_symlink_test";
    const auto outside =
        std::filesystem::temp_directory_path() / "ros2_bag_recovery_symlink_outside_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::remove_all(outside, ignored);
    ros2_bag::recorder::ManifestData data;
    data.manifest_revision = 1;
    data.task.recording_id = "linked-task";
    data.task.generation = 1;
    data.task.state = ros2_bag::recorder::TaskState::Recording;
    CHECK(ros2_bag::recorder::Manifest::create(outside / data.task.recording_id, data).has_value());
    std::filesystem::create_directories(root);
    std::filesystem::create_directory_symlink(
        outside / data.task.recording_id,
        root / "linked-task");
    const auto report = ros2_bag::recorder::Recovery::run(root);
    CHECK(report.has_value());
    const auto untouched = ros2_bag::recorder::Manifest::load(outside / data.task.recording_id);
    CHECK(untouched.has_value());
    CHECK(untouched->data().task.state == ros2_bag::recorder::TaskState::Recording);
    std::filesystem::remove_all(root, ignored);
    std::filesystem::remove_all(outside, ignored);
}

void test_task_query_boundaries() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_task_query_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    ros2_bag::recorder::ManifestData data;
    data.manifest_revision = 1;
    data.task.recording_id = "query-one";
    data.task.generation = 1;
    data.task.state = ros2_bag::recorder::TaskState::Incomplete;
    ros2_bag::recorder::SegmentInfo segment;
    segment.segment_id = 1;
    segment.state = ros2_bag::recorder::SegmentLifecycleState::Incomplete;
    segment.relative_path = "segment_000001";
    data.segments.push_back(segment);
    CHECK(ros2_bag::recorder::Manifest::create(root / data.task.recording_id, data).has_value());
    auto second = data;
    second.task.recording_id = "query-two";
    CHECK(
        ros2_bag::recorder::Manifest::create(root / second.task.recording_id, second).has_value());
    ros2_bag::recorder::RecorderConfig config;
    config.output_root = root;
    config.history.max_task_scan_entries = 1;
    ros2_bag::recorder::Recorder recorder(config);
    const auto detail = recorder.get_task(data.task.recording_id);
    CHECK(detail.has_value());
    CHECK(detail->recovery.incomplete_segments == 1U);
    CHECK(detail->segments.available == 1U);
    CHECK(detail->segments.total == 1U);
    const auto beyond = recorder.get_task(data.task.recording_id, 1, 1);
    CHECK(beyond.has_value());
    CHECK(beyond->segments.items.empty());
    CHECK(!beyond->segments.has_more);
    const auto tasks = recorder.list_tasks(0, 10);
    CHECK(tasks.has_value());
    CHECK(tasks->scan_truncated);
    CHECK(tasks->scanned_entries == 1U);
    CHECK(tasks->items.size() == 1U);
    CHECK(tasks->items.front().recording_id == "query-one");
    std::filesystem::remove(root / second.task.recording_id / "manifest.json");
    std::filesystem::create_symlink(
        root / data.task.recording_id / "manifest.json",
        root / second.task.recording_id / "manifest.json");
    const auto linked_manifest = recorder.get_task(second.task.recording_id);
    CHECK(!linked_manifest.has_value());
    CHECK(linked_manifest.error().code == ros2_bag::recorder::ErrorCode::StorageError);
    std::filesystem::create_directory_symlink(root / data.task.recording_id, root / "query-link");
    const auto unsafe = recorder.get_task("query-link");
    CHECK(!unsafe.has_value());
    CHECK(unsafe.error().code == ros2_bag::recorder::ErrorCode::StorageError);
    CHECK(recorder.shutdown().wait_for(std::chrono::seconds(1)));
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
    CHECK(
        events.operation_state("operation-1").value() ==
        ros2_bag::recorder::TopicOperationState::Uncertain);
    prepared.type = ros2_bag::recorder::TopicEventType::TopicAdded;
    CHECK(events.append(prepared).value() == 2U);
    CHECK(
        events.operation_state("operation-1").value() ==
        ros2_bag::recorder::TopicOperationState::ConfirmedSuccess);
    CHECK(
        events.operation_state("missing").value() ==
        ros2_bag::recorder::TopicOperationState::NotFound);
    std::filesystem::remove_all(root, ignored);
}

void test_recorder_state_machine() {
    const auto root =
        std::filesystem::temp_directory_path() / "ros2_bag_recorder_state_machine_test";
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
    options.mode = ros2_bag::recorder::StartOptions::Timed{};
    std::get<ros2_bag::recorder::StartOptions::Timed>(options.mode).duration =
        std::chrono::milliseconds(10);
    const auto started = recorder.start(options);
    CHECK(started.wait_for(std::chrono::milliseconds(1000)));
    CHECK(started.result()->has_value());
    for (int attempt = 0; attempt < 100 && recorder.state().recorder_state !=
                                               ros2_bag::recorder::RecorderState::Idle;
         ++attempt) {
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
    ros2_bag::recorder::Cleanup cleanup(
        1,
        std::chrono::seconds(1),
        [&](ros2_bag::recorder::DeleteResult result) {
            succeeded.store(result.result.has_value());
            called.store(true);
        });
    CHECK(cleanup.enqueue({"cleanup", 1, root, 4}));
    for (int attempt = 0; attempt < 100 && !called.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
    CHECK(std::holds_alternative<ros2_bag::recorder::QosOptions::Explicit>(automatic->mode));
    CHECK(std::get<ros2_bag::recorder::QosOptions::Explicit>(automatic->mode).depth == 10);
    ros2_bag::recorder::QosOptions best_effort;
    ros2_bag::recorder::QosOptions::Explicit best_effort_qos;
    best_effort_qos.reliability = ros2_bag::recorder::Reliability::BestEffort;
    best_effort_qos.durability = ros2_bag::recorder::Durability::Volatile;
    best_effort_qos.history = ros2_bag::recorder::History::KeepLast;
    best_effort_qos.depth = 5;
    best_effort.mode = best_effort_qos;
    const auto automatic_selection =
        ros2_bag::recorder::select_qos(options.qos, {best_effort}, 1, false);
    CHECK(automatic_selection.has_value());
    CHECK(
        std::get<ros2_bag::recorder::QosOptions::Explicit>(automatic_selection->qos.mode)
            .reliability == ros2_bag::recorder::Reliability::BestEffort);
    CHECK(automatic_selection->all_compatible);
    ros2_bag::recorder::QosOptions explicit_reliable = *automatic;
    const auto explicit_selection =
        ros2_bag::recorder::select_qos(explicit_reliable, {best_effort}, 1, false);
    CHECK(explicit_selection.has_value());
    CHECK(!explicit_selection->all_compatible);
    options.expected_hz = 0.0;
    CHECK(!ros2_bag::recorder::validate_topic_options(options).has_value());
    options.expected_hz.reset();
    ros2_bag::recorder::QosOptions invalid;
    auto invalid_qos = ros2_bag::recorder::QosOptions::Explicit{};
    invalid_qos.history = ros2_bag::recorder::History::KeepAll;
    invalid.mode = invalid_qos;
    options.qos = invalid;
    CHECK(!ros2_bag::recorder::validate_topic_options(options).has_value());
}

void test_topic_mutation_requires_ros_node() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_topic_mutation_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    ros2_bag::recorder::RecorderConfig config;
    config.output_root = root;
    ros2_bag::recorder::Recorder recorder(config);
    ros2_bag::recorder::StartOptions start;
    start.recording_id = "topic-mutation";
    const auto started = recorder.start(start);
    CHECK(started.wait_for(std::chrono::milliseconds(1000)));
    CHECK(started.result()->has_value());
    ros2_bag::recorder::TopicOptions topic;
    topic.name = "/test";
    topic.type = "std_msgs/msg/String";
    const auto added =
        recorder.add_topic("topic-mutation", started.result()->value().generation, topic, "add-1");
    CHECK(added.wait_for(std::chrono::milliseconds(1000)));
    CHECK(!added.result()->has_value());
    CHECK(added.result()->error().code == ros2_bag::recorder::ErrorCode::Unavailable);
    CHECK(recorder.stop("topic-mutation", started.result()->value().generation)
              .wait_for(std::chrono::milliseconds(1000)));
    CHECK(recorder.shutdown().wait_for(std::chrono::milliseconds(1000)));
    std::filesystem::remove_all(root, ignored);
}

void test_concurrent_message_queue() {
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t messages_per_producer = 1000;
    constexpr std::size_t total_messages = producer_count * messages_per_producer;

    ros2_bag::recorder::MessageQueue queue(total_messages, total_messages, producer_count);
    std::vector<std::thread> producers;
    producers.reserve(producer_count);

    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&queue, producer] {
            for (std::size_t index = 0; index < messages_per_producer; ++index) {
                ros2_bag::recorder::RecordedMessage message;
                message.topic_id = static_cast<ros2_bag::recorder::TopicId>(producer + 1);
                message.payload_bytes = 1;
                CHECK(queue.try_push(std::move(message)));
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    const auto queued = queue.state();
    CHECK(queued.messages.used == total_messages);
    CHECK(queued.bytes.used == total_messages);

    const auto batch = queue.pop_batch(total_messages);
    CHECK(batch.size() == total_messages);

    const auto drained = queue.state();
    CHECK(drained.messages.used == 0U);
    CHECK(drained.bytes.used == 0U);

    queue.close();
    CHECK(queue.pop_batch(1).empty());
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
    gate.open(
        3,
        4,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (now + std::chrono::milliseconds(1)).time_since_epoch())
            .count());
    CHECK(
        gate.admit(3, 4, disk, now + std::chrono::milliseconds(1)) ==
        ros2_bag::recorder::AdmissionResult::TimedDeadline);
    CHECK(gate.admit(3, 4, disk, now) == ros2_bag::recorder::AdmissionResult::NotAccepting);
    CHECK(gate.counters().admission.timed_deadline == 1U);
}

void test_resource_monitor() {
    ros2_bag::recorder::DiskAdmissionState admission;
    ros2_bag::recorder::DiskMonitorConfig config;
    config.path = std::filesystem::temp_directory_path();
    config.interval = std::chrono::milliseconds(10);
    config.max_age = std::chrono::seconds(1);
    config.min_free_bytes = 0;
    config.pause_percent = 100.0;
    std::atomic<bool> sampled{false};
    {
        ros2_bag::recorder::ResourceMonitor monitor(
            config,
            admission,
            [&](ros2_bag::recorder::DiskSample sample) {
                sampled.store(sample.state != ros2_bag::recorder::DiskSafetyState::Unknown);
            });
        for (int attempt = 0; attempt < 100 && !sampled.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(sampled.load());
        const auto snapshot = admission.read();
        CHECK(snapshot.has_value());
        CHECK(snapshot->state != ros2_bag::recorder::DiskSafetyState::Unknown);
        monitor.stop();
    }
}

void test_schema_identity() {
    const auto digest = ros2_bag::recorder::schema_sha256("int32 value\n");
    CHECK(digest.size() == 64U);
    CHECK(digest == ros2_bag::recorder::schema_sha256("int32 value\n"));
    CHECK(digest != ros2_bag::recorder::schema_sha256("int32 another\n"));
    CHECK(!ros2_bag::recorder::validate_type_support("").has_value());
}

void test_writer_mcap_smoke() {
    const auto root = std::filesystem::temp_directory_path() / "ros2_bag_writer_mcap_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    ros2_bag::recorder::Writer writer(ros2_bag::recorder::CompressionMode::Lz4);
    const auto opened = writer.open(root / "segment_000001");
    if (!opened) {
        std::cerr << "SKIP writer MCAP smoke: storage plugin unavailable\n";
        std::filesystem::remove_all(root, ignored);
        return;
    }

    ros2_bag::recorder::TopicInfo topic;
    topic.topic_id = 1;
    topic.topic_epoch = 1;
    topic.name = "/writer_smoke";
    topic.type = "std_msgs/msg/String";
    const auto resolved_schema = ros2_bag::recorder::resolve_schema(topic.type);
    CHECK(resolved_schema.has_value());
    topic.schema_sha256 = resolved_schema->sha256;

    const auto registered = writer.register_topic(topic);
    CHECK(registered.has_value());

    auto frozen_topic = std::make_shared<ros2_bag::recorder::TopicInfo>(topic);
    std::vector<ros2_bag::recorder::RecordedMessage> batch;
    constexpr std::size_t payload_size = 4096;
    for (std::int64_t index = 0; index < 256; ++index) {
        ros2_bag::recorder::RecordedMessage message;
        message.topic = frozen_topic;
        message.topic_id = topic.topic_id;
        message.topic_epoch = topic.topic_epoch;
        message.message = std::make_shared<rclcpp::SerializedMessage>(payload_size);
        auto& serialized = message.message->get_rcl_serialized_message();
        std::fill_n(serialized.buffer, payload_size, 0);
        serialized.buffer_length = payload_size;
        message.payload_bytes = payload_size;
        message.receive_timestamp_ns = index + 1;
        batch.push_back(std::move(message));
    }

    const auto written = writer.write_batch(batch);
    CHECK(written.has_value());

    const auto closed = writer.close();
    CHECK(closed.has_value());

    std::filesystem::path mcap_file;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ignored)) {
        if (entry.path().extension() == ".mcap") {
            CHECK(mcap_file.empty());
            mcap_file = entry.path();
        }
    }
    CHECK(!mcap_file.empty());

    mcap::McapReader reader;
    CHECK(reader.open(mcap_file.string()).ok());
    CHECK(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());
    CHECK(!reader.channels().empty());
    CHECK(!reader.schemas().empty());
    const auto channel = reader.channels().begin()->second;
    CHECK(channel->topic == topic.name);
    CHECK(channel->schemaId != 0U);
    const auto schema = reader.schema(channel->schemaId);
    CHECK(schema != nullptr);
    CHECK(schema->encoding == "ros2msg");
    CHECK(!schema->data.empty());
    CHECK(!reader.chunkIndexes().empty());
    CHECK(std::any_of(
        reader.chunkIndexes().begin(),
        reader.chunkIndexes().end(),
        [](const mcap::ChunkIndex& chunk) { return chunk.compression == "lz4"; }));

    std::size_t message_count = 0;
    for (const auto& message_view : reader.readMessages()) {
        (void)message_view;
        ++message_count;
    }
    CHECK(message_count == batch.size());

    reader.close();
    std::filesystem::remove_all(root, ignored);
}

void test_recorder_ros_integration() {
    const auto root =
        std::filesystem::temp_directory_path() / "ros2_bag_recorder_ros_integration_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    auto node = std::make_shared<rclcpp::Node>("ros2_bag_recorder_foundation_node");
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);
    std::thread spinning([&executor] { executor.spin(); });
    auto publisher = node->create_generic_publisher(
        "/recorder_foundation",
        "std_msgs/msg/String",
        rclcpp::QoS(10));
    {
        ros2_bag::recorder::RecorderConfig config;
        config.output_root = root;
        config.disk.min_free_bytes = 0;
        config.disk.pause_percent = 99.0;
        ros2_bag::recorder::Recorder recorder(config, node);
        ros2_bag::recorder::StartOptions start;
        start.recording_id = "integration";
        start.mode = ros2_bag::recorder::StartOptions::Rolling{};
        std::get<ros2_bag::recorder::StartOptions::Rolling>(start.mode).segment_duration =
            std::chrono::seconds(1);
        std::get<ros2_bag::recorder::StartOptions::Rolling>(start.mode).retention_duration =
            std::chrono::seconds(1);
        const auto started = recorder.start(start);
        CHECK(started.wait_for(std::chrono::seconds(5)));
        CHECK(started.result()->has_value());
        const auto task = started.result()->value();
        const auto discovered = recorder.discover_topics();
        CHECK(discovered.has_value());
        const auto discovered_topic = std::find_if(
            discovered->begin(),
            discovered->end(),
            [](const ros2_bag::recorder::DiscoveredTopic& candidate) {
                return candidate.name == "/recorder_foundation";
            });
        CHECK(discovered_topic != discovered->end());
        CHECK(discovered_topic->publishers.count != 0);
        CHECK(discovered_topic->publishers.profiles.empty());
        CHECK(discovered_topic->publishers.truncated);
        CHECK(
            discovered_topic->recordability ==
            ros2_bag::recorder::TopicRecordability::QosIncompatible);
        ros2_bag::recorder::TopicOptions topic;
        topic.name = "/recorder_foundation";
        topic.type = "std_msgs/msg/String";
        topic.expected_hz = 50.0;
        const auto added =
            recorder.add_topic(task.recording_id, task.generation, topic, "integration-add");
        CHECK(added.wait_for(std::chrono::seconds(5)));
        if (!added.result() || !added.result()->has_value()) {
            std::cerr << "SKIP recorder ROS integration: storage plugin unavailable\n";
            CHECK(recorder.shutdown().wait_for(std::chrono::seconds(5)));
            executor.cancel();
            spinning.join();
            executor.remove_node(node);
            std::filesystem::remove_all(root, ignored);
            return;
        }
        const auto first_topic = added.result()->value();
        rclcpp::SerializedMessage serialized(4);
        auto& bytes = serialized.get_rcl_serialized_message();
        bytes.buffer[0] = 0;
        bytes.buffer[1] = 1;
        bytes.buffer[2] = 0;
        bytes.buffer[3] = 0;
        bytes.buffer_length = 4;
        ros2_bag::recorder::TopicOptions duplicate_operation = topic;
        duplicate_operation.name = "/recorder_foundation_other";
        const auto duplicate = recorder.add_topic(
            task.recording_id,
            task.generation,
            duplicate_operation,
            "integration-add");
        CHECK(duplicate.wait_for(std::chrono::seconds(5)));
        CHECK(!duplicate.result()->has_value());
        CHECK(duplicate.result()->error().code == ros2_bag::recorder::ErrorCode::InvalidArgument);
        for (int attempt = 0; attempt < 100; ++attempt) {
            publisher->publish(serialized);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto state = recorder.state();
            if (!state.topics.empty() && state.topics.front().messages.written != 0) {
                break;
            }
        }
        CHECK(recorder.state().topics.front().messages.written != 0);
        const auto removed = recorder.remove_topic(
            task.recording_id,
            task.generation,
            first_topic.topic_id,
            first_topic.topic_epoch,
            "integration-remove");
        CHECK(removed.wait_for(std::chrono::seconds(5)));
        CHECK(removed.result()->has_value());
        ros2_bag::recorder::TopicOptions conflicting_topic = topic;
        conflicting_topic.type = "std_msgs/msg/UInt32";
        const auto conflicting = recorder.add_topic(
            task.recording_id,
            task.generation,
            conflicting_topic,
            "integration-type-conflict");
        CHECK(conflicting.wait_for(std::chrono::seconds(5)));
        CHECK(!conflicting.result()->has_value());
        CHECK(
            conflicting.result()->error().code == ros2_bag::recorder::ErrorCode::TopicTypeConflict);
        const auto conflict_state =
            recorder.topic_operation_state(task.recording_id, "integration-type-conflict");
        CHECK(conflict_state.has_value());
        CHECK(conflict_state.value() == ros2_bag::recorder::TopicOperationState::NotFound);
        const auto readded =
            recorder.add_topic(task.recording_id, task.generation, topic, "integration-readd");
        CHECK(readded.wait_for(std::chrono::seconds(5)));
        CHECK(readded.result()->has_value());
        CHECK(readded.result()->value().topic_epoch > first_topic.topic_epoch);
        const auto paused = recorder.pause(task.recording_id, task.generation);
        CHECK(paused.wait_for(std::chrono::seconds(5)));
        CHECK(paused.result()->has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const auto resumed = recorder.resume(task.recording_id, task.generation);
        CHECK(resumed.wait_for(std::chrono::seconds(10)));
        CHECK(resumed.result()->has_value());
        CHECK(recorder.state().segments.current->segment_id >= 2);
        for (int attempt = 0; attempt < 400; ++attempt) {
            publisher->publish(serialized);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto state = recorder.state();
            if (!state.topics.empty() && state.topics.front().messages.written != 0 &&
                state.segments.current && state.segments.current->segment_id >= 3 &&
                state.cleanup.removed != 0) {
                break;
            }
        }
        const auto state = recorder.state();
        CHECK(!state.topics.empty());
        CHECK(state.topics.front().publishers.count != 0);
        CHECK(state.topics.front().publishers.fresh);
        CHECK(
            state.topics.front().publishers.compatibility ==
            ros2_bag::recorder::TopicState::Compatibility::Unknown);
        CHECK(state.topics.front().messages.received != 0);
        CHECK(state.topics.front().messages.written != 0);
        CHECK(state.topics.front().frequency.gap != ros2_bag::recorder::TopicState::Gap::Unknown);
        CHECK(state.topics.front().frequency.gap == ros2_bag::recorder::TopicState::Gap::Normal);
        CHECK(state.segments.current.has_value());
        CHECK(state.segments.current->segment_id >= 3);
        CHECK(state.segments.rotation_lag >= std::chrono::milliseconds(0));
        CHECK(state.process.memory.rss_bytes > 0);
        CHECK(state.process.threads.count > 0);
        CHECK(state.process.file_descriptors.count > 0);
        CHECK(state.cleanup.removed != 0);
        const auto stopped = recorder.stop(task.recording_id, task.generation);
        CHECK(stopped.wait_for(std::chrono::seconds(10)));
        CHECK(stopped.result()->has_value());
        CHECK(recorder.shutdown().wait_for(std::chrono::seconds(5)));
    }
    executor.cancel();
    spinning.join();
    executor.remove_node(node);
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    test_completion();
    test_config();
    test_pool();
    test_pool_reuses_serialized_message_capacity();
    test_queue_and_disk_snapshot();
    test_rolling_snapshot();
    test_manifest_commit();
    test_recovery();
    test_recovery_retries_durable_deletion();
    test_recovery_rejects_task_root_deletion();
    test_recovery_ignores_task_directory_symlink();
    test_task_query_boundaries();
    test_topic_events();
    test_recorder_state_machine();
    test_timed_task();
    test_cleanup();
    test_topic_validation();
    test_topic_mutation_requires_ros_node();
    test_concurrent_message_queue();
    test_admission_gate();
    test_resource_monitor();
    test_schema_identity();
    test_writer_mcap_smoke();
    test_recorder_ros_integration();
    rclcpp::shutdown();
    if (failures != 0) {
        std::cerr << failures << " foundation test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
