// Exercises the production WriterAdapter ownership boundary with actual MCAP I/O.
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rclcpp/serialization.hpp>
#include <rosbag2_cpp/reader.hpp>
#include <rosbag2_storage/storage_options.hpp>
#include <std_msgs/msg/string.hpp>

#include "ros2_bag/recorded_message.hpp"
#include "ros2_bag/topic_info.hpp"
#include "ros2_bag/writer_adapter.hpp"

namespace {

constexpr std::int64_t kBaseTimestampNs = 1700000000000000000LL;
constexpr char kTopicName[] = "/writer_adapter_lifetime";
constexpr char kTopicType[] = "std_msgs/msg/String";
constexpr double kMaxRssSlopeMibPerMinute = 1.0;

struct RunOptions {
    std::size_t message_count = 1000;
    std::size_t rate_hz = 0;
};

RunOptions parse_options(int argc, char** argv) {
    if (argc == 1) {
        return RunOptions{};
    }
    if (argc == 2) {
        const long long parsed = std::stoll(argv[1]);
        if (parsed <= 0) {
            throw std::runtime_error("message_count must be positive");
        }
        return {static_cast<std::size_t>(parsed), 0};
    }
    if (argc == 5 && std::string(argv[1]) == "--duration-sec" &&
        std::string(argv[3]) == "--rate-hz") {
        const long long duration_sec = std::stoll(argv[2]);
        const long long rate_hz = std::stoll(argv[4]);
        if (duration_sec <= 0 || rate_hz <= 0) {
            throw std::runtime_error("duration_sec and rate_hz must be positive");
        }
        const auto duration = static_cast<std::size_t>(duration_sec);
        const auto rate = static_cast<std::size_t>(rate_hz);
        if (duration > std::numeric_limits<std::size_t>::max() / rate) {
            throw std::runtime_error("duration_sec * rate_hz overflows");
        }
        return {duration * rate, rate};
    }
    throw std::runtime_error(
        "usage: writer_adapter_lifetime_test [message_count] | "
        "--duration-sec N --rate-hz N");
}

std::uint64_t current_rss_bytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t kib = 0;
        std::string unit;
        if (fields >> key >> kib >> unit && key == "VmRSS:" && unit == "kB") {
            return kib * 1024ULL;
        }
    }
    throw std::runtime_error("cannot read VmRSS from /proc/self/status");
}

double rss_slope_mib_per_minute(const std::vector<std::pair<double, std::uint64_t>>& samples) {
    if (samples.size() < 2) {
        return 0.0;
    }
    double mean_time = 0.0;
    double mean_rss = 0.0;
    for (const auto& sample : samples) {
        mean_time += sample.first;
        mean_rss += static_cast<double>(sample.second);
    }
    mean_time /= static_cast<double>(samples.size());
    mean_rss /= static_cast<double>(samples.size());
    double covariance = 0.0;
    double variance = 0.0;
    for (const auto& sample : samples) {
        const double time_delta = sample.first - mean_time;
        covariance += time_delta * (static_cast<double>(sample.second) - mean_rss);
        variance += time_delta * time_delta;
    }
    if (variance == 0.0) {
        return 0.0;
    }
    return covariance / variance * 60.0 / (1024.0 * 1024.0);
}

std::shared_ptr<rclcpp::SerializedMessage> make_payload(
    std::size_t index,
    rclcpp::Serialization<std_msgs::msg::String>* serializer) {
    auto serialized = std::make_shared<rclcpp::SerializedMessage>();
    std_msgs::msg::String source;
    source.data = "writer-adapter-" + std::to_string(index) + std::string(1024, 'x');
    serializer->serialize_message(&source, serialized.get());
    return serialized;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const RunOptions run_options = parse_options(argc, argv);
        const std::filesystem::path root = "/tmp/ros2_bag_writer_adapter_lifetime_test";
        std::error_code filesystem_error;
        std::filesystem::remove_all(root, filesystem_error);

        ros2_bag::WriterAdapter adapter;
        ros2_bag::WriterOpenOptions options;
        options.uri = root.string();
        std::string error;
        if (!adapter.open_segment(options, &error)) {
            throw std::runtime_error(error);
        }

        ros2_bag::TopicInfo topic;
        topic.id = 1;
        topic.name = kTopicName;
        topic.type = kTopicType;
        topic.serialization_format = "cdr";
        if (!adapter.register_topic(topic, &error)) {
            throw std::runtime_error(error);
        }

        const auto topic_info = std::make_shared<const ros2_bag::TopicInfo>(topic);
        rclcpp::Serialization<std_msgs::msg::String> serializer;
        std::vector<std::pair<double, std::uint64_t>> rss_samples;
        const auto start = std::chrono::steady_clock::now();
        auto next_rss_sample = start;
        for (std::size_t index = 0; index < run_options.message_count; ++index) {
            ros2_bag::RecordedMessage message;
            message.recording_generation = 1;
            message.topic_id = topic.id;
            message.topic_epoch = 1;
            message.topic_info = topic_info;
            message.serialized_message = make_payload(index, &serializer);
            message.payload_bytes =
                message.serialized_message->get_rcl_serialized_message().buffer_length;
            message.recv_timestamp_ns = kBaseTimestampNs + static_cast<std::int64_t>(index);
            message.send_timestamp_ns = message.recv_timestamp_ns;
            message.timestamp_source = ros2_bag::TimestampSource::kReceive;
            const std::weak_ptr<rclcpp::SerializedMessage> lifetime = message.serialized_message;
            if (!adapter.write(message, &error)) {
                throw std::runtime_error(error);
            }
            message.serialized_message.reset();
            if (!lifetime.expired()) {
                throw std::runtime_error("WriterAdapter retained payload after synchronous write");
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_rss_sample) {
                const double elapsed_sec = std::chrono::duration<double>(now - start).count();
                rss_samples.emplace_back(elapsed_sec, current_rss_bytes());
                next_rss_sample = now + std::chrono::seconds(1);
            }
            if (run_options.rate_hz != 0) {
                const auto period_ns =
                    1000000000LL / static_cast<std::int64_t>(run_options.rate_hz);
                std::this_thread::sleep_until(
                    start +
                    std::chrono::nanoseconds(static_cast<std::int64_t>(index + 1) * period_ns));
            }
        }
        if (!adapter.close(&error)) {
            throw std::runtime_error(error);
        }

        rosbag2_storage::StorageOptions read_options;
        read_options.uri = root.string();
        read_options.storage_id = "mcap";
        rosbag2_cpp::Reader reader;
        reader.open(read_options);
        std::size_t read_count = 0;
        while (reader.has_next()) {
            const auto bag_message = reader.read_next();
            rclcpp::SerializedMessage serialized(*bag_message->serialized_data);
            std_msgs::msg::String decoded;
            serializer.deserialize_message(&serialized, &decoded);
            const std::string expected =
                "writer-adapter-" + std::to_string(read_count) + std::string(1024, 'x');
            if (bag_message->topic_name != kTopicName || decoded.data != expected) {
                throw std::runtime_error("MCAP data consistency mismatch");
            }
            ++read_count;
        }
        if (read_count != run_options.message_count) {
            throw std::runtime_error("MCAP message count mismatch");
        }
        const double rss_slope = rss_slope_mib_per_minute(rss_samples);
        if (rss_slope > kMaxRssSlopeMibPerMinute) {
            throw std::runtime_error("RSS slope exceeds 1 MiB/min diagnostic threshold");
        }
        std::cout << "PASS messages=" << run_options.message_count << " owner_retention=per_write"
                  << " rss_slope_mib_per_min=" << rss_slope << " mcap_readback=valid\n";
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
