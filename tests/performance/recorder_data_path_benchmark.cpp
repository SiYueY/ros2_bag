#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include "ros2_bag/recorder/message_queue.hpp"
#include "ros2_bag/recorder/message_pool.hpp"
#include "ros2_bag/recorder/writer.hpp"

namespace {

constexpr std::size_t kTopicCount = 10;
constexpr std::size_t kPayloadBytes = 2048;
constexpr std::size_t kBatchSize = 256;
constexpr std::size_t kPoolCapacity = 81920;

std::chrono::microseconds cpu_time() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return std::chrono::microseconds(0);
    }
    const auto user = std::chrono::seconds(usage.ru_utime.tv_sec) +
                      std::chrono::microseconds(usage.ru_utime.tv_usec);
    const auto system = std::chrono::seconds(usage.ru_stime.tv_sec) +
                        std::chrono::microseconds(usage.ru_stime.tv_usec);
    return std::chrono::duration_cast<std::chrono::microseconds>(user + system);
}

void initialize_payload(rclcpp::SerializedMessage* payload) {
    auto& serialized = payload->get_rcl_serialized_message();
    std::fill_n(serialized.buffer, kPayloadBytes, 0);
    serialized.buffer_length = kPayloadBytes;
}

bool parse_positive_argument(const char* argument, std::uint64_t* value) {
    if (argument == nullptr || argument[0] == '\0' || argument[0] == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(argument, &end, 10);
    if (errno == ERANGE || end == argument || *end != '\0') {
        return false;
    }
    if (parsed == 0 || parsed > std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::chrono::seconds duration(10);
    std::uint64_t rate_hz = 1000;
    if (argc > 3) {
        std::cerr << "usage: recorder_data_path_benchmark [duration_s] [rate_hz_per_topic]\n";
        return EXIT_FAILURE;
    }
    std::uint64_t requested_duration = duration.count();
    if (argc >= 2 && !parse_positive_argument(argv[1], &requested_duration)) {
        std::cerr << "duration_s must be a positive integer\n";
        return EXIT_FAILURE;
    }
    if (requested_duration >
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::seconds::rep>::max())) {
        std::cerr << "duration_s is too large\n";
        return EXIT_FAILURE;
    }
    duration = std::chrono::seconds(requested_duration);
    if (argc == 3 && !parse_positive_argument(argv[2], &rate_hz)) {
        std::cerr << "rate_hz_per_topic must be a positive integer\n";
        return EXIT_FAILURE;
    }
    const auto period = std::chrono::microseconds(1000000 / rate_hz);
    if (period.count() == 0) {
        std::cerr << "rate_hz is too high for microsecond scheduling\n";
        return EXIT_FAILURE;
    }

    const auto root =
        std::filesystem::temp_directory_path() / "ros2_bag_recorder_data_path_benchmark";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);

    ros2_bag::recorder::Writer writer(ros2_bag::recorder::CompressionMode::Lz4);
    const auto opened = writer.open(root / "segment_000001");
    if (!opened) {
        std::cerr << "writer open failed: " << opened.error().message << '\n';
        return EXIT_FAILURE;
    }

    std::vector<std::shared_ptr<const ros2_bag::recorder::TopicInfo>> topics;
    topics.reserve(kTopicCount);
    for (std::size_t index = 0; index < kTopicCount; ++index) {
        auto topic = std::make_shared<ros2_bag::recorder::TopicInfo>();
        topic->topic_id = index + 1;
        topic->topic_epoch = 1;
        topic->name = "/low_state_" + std::to_string(index);
        topic->type = "std_msgs/msg/String";
        const auto registered = writer.register_topic(*topic);
        if (!registered) {
            std::cerr << "topic registration failed: " << registered.error().message << '\n';
            return EXIT_FAILURE;
        }
        topics.push_back(std::move(topic));
    }

    ros2_bag::recorder::MessageQueue queue(65536, 64U * 1024U * 1024U, kTopicCount);
    ros2_bag::recorder::MessagePool pool(kPoolCapacity);
    std::atomic<bool> producers_done{false};
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> pool_exhausted{0};
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> write_failures{0};
    std::atomic<std::size_t> queue_peak{0};

    const auto started = std::chrono::steady_clock::now();
    const auto cpu_started = cpu_time();
    const auto deadline = started + duration;
    std::thread consumer([&] {
        while (!producers_done.load(std::memory_order_acquire) ||
               queue.state().messages.used != 0) {
            auto first = queue.pop_for(std::chrono::milliseconds(50));
            if (!first) {
                if (queue.closed()) {
                    return;
                }
                continue;
            }
            std::vector<ros2_bag::recorder::RecordedMessage> batch;
            batch.reserve(kBatchSize);
            batch.push_back(std::move(*first));
            auto remaining = queue.pop_batch(kBatchSize - 1);
            batch.insert(
                batch.end(),
                std::make_move_iterator(remaining.begin()),
                std::make_move_iterator(remaining.end()));
            const auto queued = queue.state().messages.used;
            auto peak = queue_peak.load(std::memory_order_relaxed);
            while (queued > peak && !queue_peak.compare_exchange_weak(
                                        peak,
                                        queued,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
            }
            const auto result = writer.write_batch(batch);
            if (!result) {
                write_failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            written.fetch_add(batch.size(), std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kTopicCount);
    for (std::size_t index = 0; index < kTopicCount; ++index) {
        producers.emplace_back([&, index] {
            rclcpp::SerializedMessage incoming(kPayloadBytes);
            initialize_payload(&incoming);
            auto next = std::chrono::steady_clock::now();
            while (next < deadline) {
                auto owned = pool.acquire();
                if (!owned) {
                    pool_exhausted.fetch_add(1, std::memory_order_relaxed);
                    dropped.fetch_add(1, std::memory_order_relaxed);
                    next += period;
                    std::this_thread::sleep_until(next);
                    continue;
                }
                try {
                    *owned = incoming;
                } catch (...) {
                    dropped.fetch_add(1, std::memory_order_relaxed);
                    next += period;
                    std::this_thread::sleep_until(next);
                    continue;
                }
                ros2_bag::recorder::RecordedMessage message;
                message.generation = 1;
                message.topic_id = index + 1;
                message.topic_epoch = 1;
                message.message = std::move(owned);
                message.payload_bytes = kPayloadBytes;
                message.receive_timestamp_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                message.topic = topics[index];
                if (queue.try_push(std::move(message))) {
                    published.fetch_add(1, std::memory_order_relaxed);
                } else {
                    dropped.fetch_add(1, std::memory_order_relaxed);
                }
                next += period;
                std::this_thread::sleep_until(next);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    producers_done.store(true, std::memory_order_release);
    queue.close();
    consumer.join();
    pool.begin_stop();
    if (!pool.wait_stopped_for(std::chrono::seconds(1))) {
        std::cerr << "message pool did not drain\n";
        return EXIT_FAILURE;
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto cpu_elapsed = cpu_time() - cpu_started;
    const auto closed = writer.close();
    if (!closed) {
        std::cerr << "writer close failed: " << closed.error().message << '\n';
        return EXIT_FAILURE;
    }

    const auto seconds = std::chrono::duration<double>(elapsed).count();
    const auto messages_per_second = static_cast<double>(written.load()) / seconds;
    const auto mebibytes_per_second =
        static_cast<double>(written.load() * kPayloadBytes) / (1024.0 * 1024.0 * seconds);
    std::cout << "duration_s=" << seconds << " rate_hz_per_topic=" << rate_hz
              << " published=" << published.load() << " written=" << written.load()
              << " drops=" << dropped.load() << " pool_exhausted=" << pool_exhausted.load()
              << " write_failures=" << write_failures.load()
              << " messages_per_s=" << messages_per_second
              << " payload_MiB_per_s=" << mebibytes_per_second
              << " queue_peak=" << queue_peak.load()
              << " cpu_s=" << std::chrono::duration<double>(cpu_elapsed).count() << '\n';

    std::filesystem::remove_all(root, filesystem_error);
    if (dropped.load() != 0 || pool_exhausted.load() != 0 || write_failures.load() != 0 ||
        written.load() != published.load()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
