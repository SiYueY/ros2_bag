#include "ros2_bag/recorder/recorder.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <system_error>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "ros2_bag/recorder/manifest.hpp"
#include "ros2_bag/recorder/message_pool.hpp"
#include "ros2_bag/recorder/recovery.hpp"
#include "ros2_bag/recorder/topic_events.hpp"
#include "ros2_bag/recorder/writer.hpp"
#include <rclcpp/executors/single_threaded_executor.hpp>

using namespace std::chrono_literals;
namespace rec = ros2_bag::recorder;

thread_local bool fail_next_allocation = false;

void* operator new(std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
std::atomic<int> failures{0};
#define CHECK(expression)                                   \
    do {                                                    \
        if (!(expression)) {                                \
            std::cerr << __LINE__ << ": " #expression "\n"; \
            ++failures;                                     \
        }                                                   \
    } while (false)

enum class Fault {
    None,
    DiskCritical,
    DiskUnknown,
    InitialDirectorySync,
    ManifestSync,
    EventsSync,
    EventsBlock,
    RecordingCommitBlock,
    SecondManifestSync,
    ThreadCreation,
    SampleAllocation,
    EventsTerminalSync,
    WriterOpen,
    WriterRegister,
    WriterWrite,
    WriterClose
};
std::atomic<Fault> fault{Fault::None};
std::mutex barrier_mutex;
std::condition_variable barrier_condition;
bool blocked = false;
bool released = false;
std::atomic<int> event_file_syncs{0};
std::atomic<int> writer_opens{0};
std::atomic<bool> secondary_close_failure{false};
std::atomic<int> thread_fail_after{0};
std::atomic<int> task_directory_syncs{0};
std::atomic<int> manifest_file_syncs{0};

bool ends_with(const char* value, const char* suffix) {
    const auto size = std::strlen(value);
    const auto tail = std::strlen(suffix);
    return size >= tail && std::strcmp(value + size - tail, suffix) == 0;
}

template <class Predicate>
bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

template <class T>
ros2_bag::Result<T, rec::Error> await(rec::Completion<T> completion) {
    if (!completion.wait_for(5s)) {
        std::cerr << "completion timed out\n";
        std::abort();
    }
    return *completion.result();
}

rec::RecorderConfig config_for(const char* name) {
    rec::RecorderConfig config;
    config.output_root =
        std::filesystem::temp_directory_path() / (std::string("ros2_bag_correctness_") + name);
    std::filesystem::remove_all(config.output_root);
    config.queue.max_messages = 64;
    config.queue.max_bytes = 65536;
    config.snapshot.max_messages_per_topic = 8;
    config.disk.min_free_bytes = 0;
    return config;
}

rec::StartOptions options_for(const char* id = "task") {
    rec::StartOptions options;
    options.recording_id = id;
    return options;
}

void test_pool_control_block_failure() {
    rec::MessagePool pool(1);
    bool caught = false;
    fail_next_allocation = true;
    try {
        (void)pool.acquire();
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    CHECK(caught);
    CHECK(pool.state().in_use == 0);
    auto message = pool.acquire();
    CHECK(message);
    message.reset();
    pool.begin_stop();
    CHECK(pool.wait_stopped_for(100ms));
}

void test_start_disk() {
    for (const auto kind : {Fault::DiskCritical, Fault::DiskUnknown}) {
        auto config = config_for("disk");
        rec::Recorder recorder(config);
        fault.store(kind);
        const auto result = await(recorder.start(options_for()));
        fault.store(Fault::None);
        CHECK(!result);
        CHECK(
            result.error().code == (kind == Fault::DiskCritical ? rec::ErrorCode::DiskCritical
                                                                : rec::ErrorCode::DiskUnknown));
        CHECK(result.error().effects == rec::Effects::None);
        CHECK(recorder.state().recorder_state == rec::RecorderState::Idle);
        CHECK(!std::filesystem::exists(config.output_root / "task"));
        CHECK(await(recorder.shutdown()));
    }
}

void test_initial_uncertainty() {
    auto config = config_for("uncertainty");
    rec::Recorder recorder(config);
    fault.store(Fault::InitialDirectorySync);
    const auto result = await(recorder.start(options_for()));
    fault.store(Fault::None);
    CHECK(!result);
    CHECK(result.error().effects == rec::Effects::Possible);
    CHECK(recorder.state().recorder_state == rec::RecorderState::Failed);
    CHECK(std::filesystem::exists(config.output_root / "task"));
    CHECK(!await(recorder.start(options_for("next"))));
    CHECK(await(recorder.shutdown()));
}

void test_initializing_and_shutdown_saturation() {
    auto config = config_for("initializing");
    config.operation_queue_capacity = 1;
    rec::Recorder recorder(config);
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        blocked = false;
        released = false;
    }
    fault.store(Fault::EventsBlock);
    const auto start = recorder.start(options_for());
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        CHECK(barrier_condition.wait_for(lock, 3s, [] { return blocked; }));
    }
    const auto snapshot = recorder.state();
    CHECK(snapshot.recorder_state == rec::RecorderState::Initializing);
    CHECK(snapshot.task && snapshot.task->recording_id == "task");
    CHECK(!start.done());
    const auto paused = recorder.pause("task", snapshot.task->generation);
    CHECK(paused.done());
    CHECK(!await(paused) && await(paused).error().effects == rec::Effects::None);
    CHECK(!await(recorder.resume("task", snapshot.task->generation)));
    rec::TopicOptions topic;
    topic.name = "/initializing";
    topic.type = "std_msgs/msg/String";
    CHECK(!await(recorder.add_topic("task", snapshot.task->generation, topic, "add")));
    CHECK(!await(recorder.remove_topic("task", snapshot.task->generation, 1, 1, "remove")));
    const auto queued = recorder.start(options_for("queued"));
    const auto rejected = await(recorder.start(options_for("overflow")));
    CHECK(!rejected && rejected.error().code == rec::ErrorCode::ResourceExhausted);
    const auto shutdown = recorder.shutdown();
    CHECK(recorder.state().shutting_down);
    CHECK(!shutdown.done());
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        released = true;
    }
    barrier_condition.notify_all();
    CHECK(!await(start));
    CHECK(await(shutdown));
    CHECK(!await(queued));
    fault.store(Fault::None);
}

void test_start_commit_authority_before_shutdown() {
    auto config = config_for("start_commit_shutdown");
    rec::Recorder recorder(config);
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        blocked = false;
        released = false;
    }
    manifest_file_syncs.store(0);
    fault.store(Fault::RecordingCommitBlock);
    const auto start = recorder.start(options_for());
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        CHECK(barrier_condition.wait_for(lock, 3s, [] { return blocked; }));
    }
    const auto shutdown = recorder.shutdown();
    CHECK(!start.done());
    CHECK(!shutdown.done());
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        released = true;
    }
    barrier_condition.notify_all();
    CHECK(await(start));
    CHECK(await(shutdown));
    fault.store(Fault::None);
}

void test_initialization_failure_can_restart() {
    for (const auto kind : {Fault::EventsSync, Fault::ThreadCreation, Fault::ManifestSync}) {
        auto config = config_for("init_failure");
        rec::Recorder recorder(config);
        if (kind == Fault::ManifestSync) {
            // Fail a later revision, leaving INITIALIZING durable.
            fault.store(Fault::EventsBlock);
            {
                std::lock_guard<std::mutex> lock(barrier_mutex);
                blocked = false;
                released = false;
            }
            const auto start = recorder.start(options_for());
            {
                std::unique_lock<std::mutex> lock(barrier_mutex);
                CHECK(barrier_condition.wait_for(lock, 3s, [] { return blocked; }));
                fault.store(Fault::ManifestSync);
                released = true;
            }
            barrier_condition.notify_all();
            const auto result = await(start);
            CHECK(!result && result.error().effects == rec::Effects::ConfirmedPartial);
        } else {
            fault.store(kind);
            const auto result = await(recorder.start(options_for()));
            CHECK(!result && result.error().effects == rec::Effects::ConfirmedPartial);
        }
        fault.store(Fault::None);
        CHECK(eventually(
            [&] { return recorder.state().recorder_state == rec::RecorderState::Idle; }));
        const auto failed = rec::Manifest::load(config.output_root / "task");
        CHECK(failed && failed->data().task.state == rec::TaskState::Failed);
        const auto next = await(recorder.start(options_for("next")));
        CHECK(next);
        if (next) {
            CHECK(await(recorder.stop("next", next->generation)));
        }
        CHECK(await(recorder.shutdown()));
    }
}

void test_partial_thread_initialization() {
    for (const int started_threads : {1, 2}) {
        auto config = config_for("partial_threads");
        rec::Recorder recorder(config);
        thread_fail_after.store(started_threads);
        fault.store(Fault::ThreadCreation);
        const auto result = await(recorder.start(options_for()));
        fault.store(Fault::None);
        CHECK(!result && result.error().code == rec::ErrorCode::ResourceExhausted);
        CHECK(result.error().effects == rec::Effects::ConfirmedPartial);
        CHECK(eventually(
            [&] { return recorder.state().recorder_state == rec::RecorderState::Idle; }));
        const auto manifest = rec::Manifest::load(config.output_root / "task");
        CHECK(manifest && manifest->data().task.state == rec::TaskState::Failed);
        CHECK(await(recorder.start(options_for("next"))));
        CHECK(await(recorder.shutdown()));
    }
    thread_fail_after.store(0);
}

void test_topic_events_durable_and_safe() {
    const auto config = config_for("events");
    std::filesystem::create_directories(config.output_root / "task");
    event_file_syncs.store(0);
    task_directory_syncs.store(0);
    auto events = rec::TopicEvents::create(config.output_root / "task", "task", 1);
    CHECK(events);
    CHECK(!rec::TopicEvents::create(config.output_root / "task", "task", 1));
    CHECK(event_file_syncs.load() == 1);
    CHECK(task_directory_syncs.load() == 1);
    rec::TopicEvent prepared;
    prepared.operation_id = "add";
    prepared.topic_id = 1;
    prepared.topic_epoch = 1;
    fault.store(Fault::EventsSync);
    const auto appended = events->append(prepared);
    fault.store(Fault::None);
    CHECK(!appended && appended.error().effects == rec::Effects::Possible);
    CHECK(events->uncertainty_watermark() == 1);
    CHECK(events->operation_state("add").value() == rec::TopicOperationState::Uncertain);
    const auto external = config.output_root / "external";
    std::filesystem::rename(config.output_root / "task/topic_events.jsonl", external);
    std::filesystem::create_symlink(external, config.output_root / "task/topic_events.jsonl");
    CHECK(!rec::TopicEvents::open(config.output_root / "task", "task", 1));
}

void test_missing_event_evidence_is_not_created() {
    auto config = config_for("missing_events");
    rec::ManifestData data;
    data.manifest_revision = 1;
    data.task.recording_id = "task";
    data.task.generation = 1;
    data.task.state = rec::TaskState::Completed;
    const auto directory = config.output_root / "task";
    CHECK(rec::Manifest::create(directory, data));
    CHECK(!rec::TopicEvents::open(directory, "task", 1));
    {
        rec::Recorder recorder(config);
        CHECK(!recorder.topic_operation_state("task", "operation"));
        CHECK(await(recorder.shutdown()));
    }
    const auto recovery = rec::Recovery::run(config.output_root);
    CHECK(recovery);
    CHECK(std::any_of(
        recovery->findings.begin(), recovery->findings.end(),
        [](const auto& finding) { return finding.code == "topic_events_corrupt"; }));
    CHECK(!std::filesystem::exists(directory / "topic_events.jsonl"));
}

void test_add_while_paused() {
    auto config = config_for("paused_add");
    auto node = std::make_shared<rclcpp::Node>("paused_add");
    rec::Recorder recorder(config, node);
    const auto task = await(recorder.start(options_for()));
    CHECK(task);
    CHECK(await(recorder.pause("task", task->generation)));
    rec::TopicOptions topic;
    topic.name = "/paused_add";
    topic.type = "std_msgs/msg/String";
    CHECK(await(recorder.add_topic("task", task->generation, topic, "add")));
    CHECK(recorder.state().topics.front().state == rec::TopicRuntimeState::Armed);
    CHECK(await(recorder.resume("task", task->generation)));
    CHECK(recorder.state().topics.front().state == rec::TopicRuntimeState::Active);
    CHECK(await(recorder.stop("task", task->generation)));
    CHECK(await(recorder.shutdown()));
}

void test_add_while_disk_paused() {
    auto config = config_for("disk_paused_add");
    auto node = std::make_shared<rclcpp::Node>("disk_paused_add");
    rec::Recorder recorder(config, node);
    const auto task = await(recorder.start(options_for()));
    CHECK(task);
    fault.store(Fault::DiskCritical);
    CHECK(
        eventually([&] { return recorder.state().pause.disk == rec::DiskPauseReason::Critical; }));
    rec::TopicOptions topic;
    topic.name = "/disk_paused_add";
    topic.type = "std_msgs/msg/String";
    CHECK(await(recorder.add_topic("task", task->generation, topic, "add")));
    CHECK(recorder.state().topics.front().state == rec::TopicRuntimeState::Armed);
    fault.store(Fault::None);
    CHECK(await(recorder.stop("task", task->generation)));
    CHECK(await(recorder.shutdown()));
}

void test_mutation_exception_boundary() {
    auto config = config_for("exception");
    rec::Recorder recorder(config);
    fault.store(Fault::SampleAllocation);
    const auto result = await(recorder.start(options_for()));
    fault.store(Fault::None);
    CHECK(!result && result.error().code == rec::ErrorCode::ResourceExhausted);
    CHECK(result.error().effects == rec::Effects::None);
    CHECK(recorder.state().recorder_state == rec::RecorderState::Idle);
    CHECK(await(recorder.start(options_for("next"))));
    CHECK(await(recorder.shutdown()));
}

void test_timed_under_mutation_pressure() {
    auto config = config_for("timed_pressure");
    rec::Recorder recorder(config);
    auto options = options_for();
    options.mode = rec::StartOptions::Timed{100ms};
    const auto task = await(recorder.start(options));
    CHECK(task);
    CHECK(await(recorder.pause("task", task->generation)));
    std::atomic<bool> stopping{false};
    std::vector<std::thread> producers;
    for (int index = 0; index < 4; ++index) {
        producers.emplace_back([&] {
            while (!stopping.load()) {
                (void)recorder.pause("task", task->generation);
            }
        });
    }
    CHECK(eventually([&] { return recorder.state().recorder_state == rec::RecorderState::Idle; }));
    stopping.store(true);
    for (auto& producer : producers) {
        producer.join();
    }
    CHECK(!await(recorder.resume("task", task->generation)));
    const auto manifest = rec::Manifest::load(config.output_root / "task");
    CHECK(manifest && manifest->data().task.state == rec::TaskState::Completed);
    CHECK(await(recorder.shutdown()));
}
void test_writer_open_and_register_failure() {
    for (const auto kind : {Fault::WriterOpen, Fault::WriterRegister}) {
        auto config = config_for("writer_open");
        auto node = std::make_shared<rclcpp::Node>("correctness_open");
        rec::Recorder recorder(config, node);
        const auto task = await(recorder.start(options_for()));
        CHECK(task);
        rec::TopicOptions topic;
        topic.name = "/correctness_open";
        topic.type = "std_msgs/msg/String";
        fault.store(kind);
        const auto result = await(recorder.add_topic("task", task->generation, topic, "add"));
        fault.store(Fault::None);
        CHECK(!result && result.error().effects == rec::Effects::ConfirmedPartial);
        CHECK(
            recorder.topic_operation_state("task", "add").value() ==
            rec::TopicOperationState::ConfirmedFailure);
        if (kind == Fault::WriterOpen) {
            CHECK(eventually(
                [&] { return recorder.state().recorder_state == rec::RecorderState::Idle; }));
            const auto manifest = rec::Manifest::load(config.output_root / "task");
            CHECK(manifest && manifest->data().task.state == rec::TaskState::Failed);
            CHECK(manifest->data().segments.size() == 1);
            CHECK(
                manifest->data().segments.front().state == rec::SegmentLifecycleState::Incomplete);
            CHECK(!await(recorder.add_topic("task", task->generation, topic, "retry")));
            CHECK(await(recorder.start(options_for("next"))));
        } else {
            CHECK(recorder.state().recorder_state == rec::RecorderState::Recording);
            CHECK(await(recorder.stop("task", task->generation)));
        }
        CHECK(await(recorder.shutdown()));
    }
}

void test_ros_safety_and_failure() {
    for (const auto kind : {Fault::WriterWrite, Fault::WriterClose, Fault::None}) {
        auto config = config_for("ros_safety");
        if (kind == Fault::None) {
            config.disk.max_age = 100ms;
        }
        auto node = std::make_shared<rclcpp::Node>("correctness_safety");
        rclcpp::executors::SingleThreadedExecutor executor;
        executor.add_node(node);
        std::thread spinning([&] { executor.spin(); });
        auto publisher = node->create_generic_publisher(
            "/correctness_safety", "std_msgs/msg/String", rclcpp::QoS(10));
        {
            rec::Recorder recorder(config, node);
            const auto task = await(recorder.start(options_for()));
            CHECK(task);
            rec::TopicOptions topic;
            topic.name = "/correctness_safety";
            topic.type = "std_msgs/msg/String";
            const auto added = await(recorder.add_topic("task", task->generation, topic, "add"));
            CHECK(added);
            rclcpp::SerializedMessage serialized(4);
            auto& bytes = serialized.get_rcl_serialized_message();
            bytes.buffer[0] = 0;
            bytes.buffer[1] = 1;
            bytes.buffer[2] = 0;
            bytes.buffer[3] = 0;
            bytes.buffer_length = 4;
            std::atomic<bool> pressure_stopping{false};
            std::vector<std::thread> pressure;
            if (kind != Fault::WriterClose) {
                for (int index = 0; index < 4; ++index) {
                    pressure.emplace_back([&] {
                        while (!pressure_stopping.load()) {
                            (void)recorder.pause("stale-task", task->generation);
                        }
                    });
                }
            }
            if (kind == Fault::WriterWrite) {
                secondary_close_failure.store(true);
            }
            if (kind == Fault::WriterClose) {
                fault.store(kind);
                const auto stopped = await(recorder.stop("task", task->generation));
                CHECK(!stopped && stopped.error().effects == rec::Effects::ConfirmedPartial);
            } else {
                if (kind == Fault::None) {
                    std::this_thread::sleep_for(150ms);
                }
                fault.store(kind);
                CHECK(eventually([&] {
                    publisher->publish(serialized);
                    const auto state = recorder.state();
                    return kind == Fault::None ? state.recorder_state == rec::RecorderState::Paused
                                               : state.recorder_state == rec::RecorderState::Idle;
                }));
            }
            fault.store(Fault::None);
            pressure_stopping.store(true);
            for (auto& thread : pressure) {
                thread.join();
            }
            // Wait for already accepted stale calls without adding pressure.
            std::this_thread::sleep_for(10ms);
            if (kind == Fault::None) {
                CHECK(recorder.state().pause.disk == rec::DiskPauseReason::Unknown);
                const auto resumed = await(recorder.resume("task", task->generation));
                CHECK(!resumed && resumed.error().code == rec::ErrorCode::DiskUnknown);
                CHECK(await(recorder.stop("task", task->generation)));
            } else {
                CHECK(eventually(
                    [&] { return recorder.state().recorder_state == rec::RecorderState::Idle; }));
                const auto manifest = rec::Manifest::load(config.output_root / "task");
                CHECK(manifest && manifest->data().task.state == rec::TaskState::Failed);
                CHECK(
                    manifest->data().segments.front().state ==
                    rec::SegmentLifecycleState::Incomplete);
                if (kind == Fault::WriterWrite) {
                    CHECK(manifest->data().task.error->message == "injected write_batch");
                    CHECK(!manifest->data().secondary_errors.empty());
                    CHECK(manifest->data().secondary_errors.front().message == "injected close");
                }
                CHECK(await(recorder.start(options_for("next"))));
            }
            CHECK(await(recorder.shutdown()));
        }
        executor.cancel();
        spinning.join();
        executor.remove_node(node);
    }
}

void test_rolling_and_cleanup_under_pressure() {
    auto config = config_for("rolling_pressure");
    config.operation_queue_capacity = 1;
    config.cleanup.queue_capacity = 1;
    auto node = std::make_shared<rclcpp::Node>("correctness_rolling");
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);
    std::thread spinning([&] { executor.spin(); });
    auto publisher = node->create_generic_publisher(
        "/correctness_rolling", "std_msgs/msg/String", rclcpp::QoS(10));
    {
        rec::Recorder recorder(config, node);
        auto options = options_for();
        options.mode = rec::StartOptions::Rolling{};
        std::get<rec::StartOptions::Rolling>(options.mode).segment_duration = 1s;
        std::get<rec::StartOptions::Rolling>(options.mode).retention_duration = 1s;
        const auto task = await(recorder.start(options));
        CHECK(task);
        rec::TopicOptions topic;
        topic.name = "/correctness_rolling";
        topic.type = "std_msgs/msg/String";
        CHECK(await(recorder.add_topic("task", task->generation, topic, "add")));
        rclcpp::SerializedMessage serialized(4);
        auto& bytes = serialized.get_rcl_serialized_message();
        bytes.buffer[0] = 0;
        bytes.buffer[1] = 1;
        bytes.buffer[2] = 0;
        bytes.buffer[3] = 0;
        bytes.buffer_length = 4;
        std::atomic<bool> stopping{false};
        std::vector<std::thread> pressure;
        for (int index = 0; index < 4; ++index) {
            pressure.emplace_back([&] {
                while (!stopping.load()) {
                    (void)recorder.pause("wrong-task", task->generation);
                }
            });
        }
        CHECK(eventually([&] {
            publisher->publish(serialized);
            const auto state = recorder.state();
            return state.segments.current && state.segments.current->segment_id >= 3 &&
                   state.cleanup.removed != 0;
        }));
        stopping.store(true);
        for (auto& thread : pressure) {
            thread.join();
        }
        // Drain previously accepted stale calls before submitting Stop.
        rec::Completion<void> stopped;
        do {
            stopped = recorder.stop("task", task->generation);
        } while (stopped.done() && !stopped.result()->has_value() &&
                 stopped.result()->error().code == rec::ErrorCode::ResourceExhausted);
        CHECK(await(stopped));
        CHECK(await(recorder.shutdown()));
        const auto manifest = rec::Manifest::load(config.output_root / "task");
        CHECK(manifest);
        CHECK(std::any_of(
            manifest->data().segments.begin(), manifest->data().segments.end(),
            [](const auto& segment) {
                return segment.state == rec::SegmentLifecycleState::Removed;
            }));
    }
    executor.cancel();
    spinning.join();
    executor.remove_node(node);
}

void test_retention_commits_each_authorization() {
    auto config = config_for("retention_partial");
    auto node = std::make_shared<rclcpp::Node>("retention_partial");
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);
    std::thread spinning([&] { executor.spin(); });
    auto publisher = node->create_generic_publisher(
        "/retention_partial", "std_msgs/msg/String", rclcpp::QoS(10));
    {
        rec::Recorder recorder(config, node);
        auto options = options_for();
        options.mode = rec::StartOptions::Rolling{};
        std::get<rec::StartOptions::Rolling>(options.mode).segment_duration = 1s;
        std::get<rec::StartOptions::Rolling>(options.mode).retention_duration = 4s;
        const auto task = await(recorder.start(options));
        rec::TopicOptions topic;
        topic.name = "/retention_partial";
        topic.type = "std_msgs/msg/String";
        CHECK(await(recorder.add_topic("task", task->generation, topic, "add")));
        rclcpp::SerializedMessage serialized(4);
        auto& bytes = serialized.get_rcl_serialized_message();
        bytes.buffer[0] = 0;
        bytes.buffer[1] = 1;
        bytes.buffer[2] = 0;
        bytes.buffer[3] = 0;
        bytes.buffer_length = 4;
        CHECK(eventually([&] {
            publisher->publish(serialized);
            const auto state = recorder.state();
            return state.segments.current && state.segments.current->segment_id >= 3;
        }));
        CHECK(await(recorder.pause("task", task->generation)));
        {
            std::lock_guard<std::mutex> lock(barrier_mutex);
            blocked = false;
            released = false;
        }
        topic.name = "/retention_partial_second";
        fault.store(Fault::EventsBlock);
        const auto add = recorder.add_topic("task", task->generation, topic, "second");
        {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            CHECK(barrier_condition.wait_for(lock, 3s, [] { return blocked; }));
        }
        std::this_thread::sleep_for(4200ms);
        manifest_file_syncs.store(0);
        fault.store(Fault::SecondManifestSync);
        {
            std::lock_guard<std::mutex> lock(barrier_mutex);
            released = true;
        }
        barrier_condition.notify_all();
        CHECK(await(add));
        CHECK(eventually(
            [&] { return recorder.state().recorder_state == rec::RecorderState::Idle; }));
        fault.store(Fault::None);
        CHECK(await(recorder.shutdown()));
        const auto manifest = rec::Manifest::load(config.output_root / "task");
        CHECK(manifest);
        CHECK(manifest->data().segments.size() >= 3);
        CHECK(
            manifest->data().segments[0].state == rec::SegmentLifecycleState::Removed ||
            manifest->data().segments[0].state == rec::SegmentLifecycleState::Deleting);
        CHECK(manifest->data().segments[1].state == rec::SegmentLifecycleState::Complete);
        CHECK(std::filesystem::exists(config.output_root / "task/segment_000002"));
    }
    executor.cancel();
    spinning.join();
    executor.remove_node(node);
}

void test_topic_barriers_and_effects() {
    for (const bool removing : {false, true}) {
        for (const bool terminal : {false, true}) {
            auto config = config_for("event_barrier");
            auto node = std::make_shared<rclcpp::Node>("correctness_events");
            rec::Recorder recorder(config, node);
            const auto task = await(recorder.start(options_for()));
            CHECK(task);
            rec::TopicOptions topic;
            topic.name = "/correctness_events";
            topic.type = "std_msgs/msg/String";
            std::optional<rec::TopicInfo> added;
            if (removing) {
                added =
                    await(recorder.add_topic("task", task->generation, topic, "initial")).value();
            }
            event_file_syncs.store(0);
            writer_opens.store(0);
            fault.store(terminal ? Fault::EventsTerminalSync : Fault::EventsSync);
            if (removing) {
                const auto result = await(recorder.remove_topic(
                    "task", task->generation, added->topic_id, added->topic_epoch, "operation"));
                CHECK(!result && result.error().code == rec::ErrorCode::EventCommitFailed);
                CHECK(result.error().effects == rec::Effects::Possible);
                const auto state = recorder.state();
                CHECK(
                    state.topics.front().state ==
                    (terminal ? rec::TopicRuntimeState::Failed : rec::TopicRuntimeState::Active));
            } else {
                const auto result =
                    await(recorder.add_topic("task", task->generation, topic, "operation"));
                CHECK(!result && result.error().code == rec::ErrorCode::EventCommitFailed);
                CHECK(result.error().effects == rec::Effects::Possible);
                if (!terminal) {
                    CHECK(writer_opens.load() == 0);
                }
            }
            fault.store(Fault::None);
            CHECK(
                recorder.topic_operation_state("task", "operation").value() ==
                rec::TopicOperationState::Uncertain);
            topic.name = "/correctness_events_new";
            const auto rejected = await(recorder.add_topic("task", task->generation, topic, "new"));
            CHECK(!rejected && rejected.error().effects == rec::Effects::None);
            CHECK(await(recorder.stop("task", task->generation)));
            CHECK(await(recorder.shutdown()));
        }
    }
}

void test_stop_manifest_failure() {
    for (const bool uncertain : {false, true}) {
        auto config = config_for("stop_failure");
        rec::Recorder recorder(config);
        const auto task = await(recorder.start(options_for()));
        fault.store(uncertain ? Fault::InitialDirectorySync : Fault::ManifestSync);
        const auto stopped = await(recorder.stop("task", task->generation));
        fault.store(Fault::None);
        CHECK(!stopped);
        CHECK(
            stopped.error().effects ==
            (uncertain ? rec::Effects::Possible : rec::Effects::ConfirmedPartial));
        if (uncertain) {
            CHECK(recorder.state().recorder_state == rec::RecorderState::Failed);
            CHECK(!await(recorder.start(options_for("next"))));
        } else {
            CHECK(eventually(
                [&] { return recorder.state().recorder_state == rec::RecorderState::Idle; }));
            const auto manifest = rec::Manifest::load(config.output_root / "task");
            CHECK(manifest && manifest->data().task.state == rec::TaskState::Failed);
            CHECK(await(recorder.start(options_for("next"))));
        }
        CHECK(await(recorder.shutdown()));
    }
}

void test_resume_queued_past_timed_deadline() {
    auto config = config_for("resume_timed");
    auto node = std::make_shared<rclcpp::Node>("correctness_resume");
    rec::Recorder recorder(config, node);
    auto options = options_for();
    options.mode = rec::StartOptions::Timed{400ms};
    const auto task = await(recorder.start(options));
    rec::TopicOptions topic;
    topic.name = "/correctness_resume";
    topic.type = "std_msgs/msg/String";
    const auto added = await(recorder.add_topic("task", task->generation, topic, "add"));
    CHECK(added);
    CHECK(await(recorder.pause("task", task->generation)));
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        blocked = false;
        released = false;
    }
    fault.store(Fault::EventsBlock);
    const auto removed = recorder.remove_topic(
        "task", task->generation, added->topic_id, added->topic_epoch, "remove");
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        CHECK(barrier_condition.wait_for(lock, 3s, [] { return blocked; }));
    }
    const auto resumed = recorder.resume("task", task->generation);
    std::this_thread::sleep_for(500ms);
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        released = true;
    }
    barrier_condition.notify_all();
    CHECK(await(removed));
    const auto result = await(resumed);
    CHECK(!result && result.error().effects == rec::Effects::None);
    CHECK(
        result.error().code == rec::ErrorCode::InvalidState ||
        result.error().code == rec::ErrorCode::StaleRecording);
    fault.store(Fault::None);
    CHECK(await(recorder.shutdown()));
}

void test_timed_failure_can_restart() {
    auto config = config_for("timed_failure");
    rec::Recorder recorder(config);
    auto options = options_for();
    options.mode = rec::StartOptions::Timed{50ms};
    CHECK(await(recorder.start(options)));
    fault.store(Fault::ManifestSync);
    CHECK(eventually([&] { return recorder.state().recorder_state == rec::RecorderState::Idle; }));
    fault.store(Fault::None);
    const auto manifest = rec::Manifest::load(config.output_root / "task");
    CHECK(manifest && manifest->data().task.state == rec::TaskState::Failed);
    CHECK(await(recorder.start(options_for("next"))));
    CHECK(await(recorder.shutdown()));
}

}  // namespace

extern "C" int __real_fsync(int file);
extern "C" int __wrap_fsync(int file) {
    char descriptor[64];
    std::snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", file);
    char path[4096];
    const auto size = ::readlink(descriptor, path, sizeof(path) - 1);
    if (size < 0) {
        return __real_fsync(file);
    }
    path[size] = 0;
    const bool events = ends_with(path, "/topic_events.jsonl");
    const bool manifest = ends_with(path, "/manifest.json.tmp");
    const bool directory = ends_with(path, "/task");
    int event_index = 0;
    if (events) {
        event_index = ++event_file_syncs;
    }
    if (directory) {
        ++task_directory_syncs;
    }
    auto kind = fault.load();
    const int manifest_index = manifest ? ++manifest_file_syncs : 0;
    if (manifest && kind == Fault::RecordingCommitBlock && manifest_index == 2) {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        blocked = true;
        barrier_condition.notify_all();
        barrier_condition.wait(lock, [] { return released; });
    }
    if (events && kind == Fault::EventsBlock) {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        blocked = true;
        barrier_condition.notify_all();
        barrier_condition.wait(lock, [] { return released; });
    }
    if ((directory && kind == Fault::InitialDirectorySync) ||
        (events &&
         (kind == Fault::EventsSync || (kind == Fault::EventsTerminalSync && event_index == 2))) ||
        (manifest && (kind == Fault::ManifestSync ||
                      (kind == Fault::SecondManifestSync && manifest_index == 2)))) {
        fault.store(Fault::None);
        errno = EIO;
        return -1;
    }
    return __real_fsync(file);
}

extern "C" int __real_statvfs(const char* path, struct statvfs* value);
extern "C" int __wrap_statvfs(const char* path, struct statvfs* value) {
    const auto kind = fault.load();
    if (kind == Fault::SampleAllocation) {
        throw std::bad_alloc();
    }
    if (kind == Fault::DiskUnknown) {
        errno = EIO;
        return -1;
    }
    const auto result = __real_statvfs(path, value);
    if (result == 0 && kind == Fault::DiskCritical) {
        value->f_bavail = 0;
    }
    return result;
}

// Wrap the C++ thread-start boundary: pthread_create inside libstdc++ is
// dynamically linked and would bypass a link-time pthread wrapper.
void real_thread_start(std::thread*, std::unique_ptr<std::thread::_State>, void (*)()) asm(
    "__real__ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE");
void wrap_thread_start(std::thread*, std::unique_ptr<std::thread::_State>, void (*)()) asm(
    "__wrap__ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE");
void wrap_thread_start(
    std::thread* thread, std::unique_ptr<std::thread::_State> state, void (*entry)()) {
    if (fault.load() == Fault::ThreadCreation && thread_fail_after.fetch_sub(1) <= 0) {
        fault.store(Fault::None);
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    real_thread_start(thread, std::move(state), entry);
}

ros2_bag::Result<void, rec::Error>
real_open(rec::Writer* writer, const std::filesystem::path& path) asm(
    "__real__ZN8ros2_bag8recorder6Writer4openERKNSt10filesystem7__cxx114pathE");
ros2_bag::Result<void, rec::Error>
wrap_open(rec::Writer* writer, const std::filesystem::path& path) asm(
    "__wrap__ZN8ros2_bag8recorder6Writer4openERKNSt10filesystem7__cxx114pathE");
ros2_bag::Result<void, rec::Error> wrap_open(
    rec::Writer* writer, const std::filesystem::path& path) {
    ++writer_opens;
    if (fault.load() == Fault::WriterOpen) {
        fault.store(Fault::None);
        return ros2_bag::make_unexpected(
            rec::Error{rec::ErrorCode::StorageError, rec::Effects::None, "injected open"});
    }
    return real_open(writer, path);
}

ros2_bag::Result<void, rec::Error>
real_register_topic(rec::Writer* writer, const rec::TopicInfo& topic) asm(
    "__real__ZN8ros2_bag8recorder6Writer14register_topicERKNS0_9TopicInfoE");
ros2_bag::Result<void, rec::Error>
wrap_register_topic(rec::Writer* writer, const rec::TopicInfo& topic) asm(
    "__wrap__ZN8ros2_bag8recorder6Writer14register_topicERKNS0_9TopicInfoE");
ros2_bag::Result<void, rec::Error> wrap_register_topic(
    rec::Writer* writer, const rec::TopicInfo& topic) {
    if (fault.load() == Fault::WriterRegister) {
        fault.store(Fault::None);
        return ros2_bag::make_unexpected(rec::Error{
            rec::ErrorCode::StorageError, rec::Effects::None, "injected register_topic"});
    }
    return real_register_topic(writer, topic);
}

ros2_bag::Result<void, rec::Error>
real_write_batch(rec::Writer* writer, const std::vector<rec::RecordedMessage>& messages) asm(
    "__real__ZN8ros2_bag8recorder6Writer11write_batchERKSt6vectorINS0_15RecordedMessageESaIS3_EE");
ros2_bag::Result<void, rec::Error>
wrap_write_batch(rec::Writer* writer, const std::vector<rec::RecordedMessage>& messages) asm(
    "__wrap__ZN8ros2_bag8recorder6Writer11write_batchERKSt6vectorINS0_15RecordedMessageESaIS3_EE");
ros2_bag::Result<void, rec::Error> wrap_write_batch(
    rec::Writer* writer, const std::vector<rec::RecordedMessage>& messages) {
    if (fault.load() == Fault::WriterWrite) {
        fault.store(secondary_close_failure.exchange(false) ? Fault::WriterClose : Fault::None);
        return ros2_bag::make_unexpected(
            rec::Error{rec::ErrorCode::StorageError, rec::Effects::None, "injected write_batch"});
    }
    return real_write_batch(writer, messages);
}

ros2_bag::Result<void, rec::Error> real_close(rec::Writer* writer) asm(
    "__real__ZN8ros2_bag8recorder6Writer5closeEv");
ros2_bag::Result<void, rec::Error> wrap_close(rec::Writer* writer) asm(
    "__wrap__ZN8ros2_bag8recorder6Writer5closeEv");
ros2_bag::Result<void, rec::Error> wrap_close(rec::Writer* writer) {
    if (fault.load() == Fault::WriterClose) {
        fault.store(Fault::None);
        (void)real_close(writer);
        return ros2_bag::make_unexpected(rec::Error{
            rec::ErrorCode::StorageError, rec::Effects::ConfirmedPartial, "injected close"});
    }
    return real_close(writer);
}

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    test_pool_control_block_failure();
    test_start_disk();
    test_initial_uncertainty();
    test_initializing_and_shutdown_saturation();
    test_start_commit_authority_before_shutdown();
    test_initialization_failure_can_restart();
    test_partial_thread_initialization();
    test_topic_events_durable_and_safe();
    test_missing_event_evidence_is_not_created();
    test_add_while_paused();
    test_add_while_disk_paused();
    test_mutation_exception_boundary();
    test_timed_under_mutation_pressure();
    test_timed_failure_can_restart();
    test_resume_queued_past_timed_deadline();
    test_topic_barriers_and_effects();
    test_stop_manifest_failure();
    test_writer_open_and_register_failure();
    test_ros_safety_and_failure();
    test_rolling_and_cleanup_under_pressure();
    test_retention_commits_each_authorization();
    rclcpp::shutdown();
    return failures.load() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
