// Phase 0 ROS experiments: GenericSubscription ownership, Executor/TLS lifetime,
// independent MCAP writes and a two-process Fast DDS sequence/QoS handshake.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/serialization.hpp>
#include <rosbag2_cpp/reader.hpp>
#include <rosbag2_cpp/writer.hpp>
#include <rosbag2_storage/serialized_bag_message.hpp>
#include <std_msgs/msg/u_int64.hpp>
#ifdef ROS2_BAG_JAZZY
#include <rosbag2_cpp/message_definitions/local_message_definition_source.hpp>
#endif

#include "payload_binding.hpp"
#include "probe_memory.hpp"

namespace ros2_bag::phase0 {
namespace {
constexpr std::size_t kSamples = 100;
constexpr std::size_t kThreads = 4;
constexpr std::size_t kPoolCapacity = 256;
constexpr std::uint64_t kExpectedPayload = 42;
constexpr std::chrono::milliseconds kConsumerDelay{500};
constexpr std::chrono::milliseconds kCallbackOverlap{1};
constexpr std::int64_t kEpochNs = 1700000000000000000;
constexpr std::chrono::seconds kDeadline{20};
constexpr std::chrono::milliseconds kPoll{5};
constexpr char kTopic[] = "/ros2_bag_phase0/value";
constexpr char kType[] = "std_msgs/msg/UInt64";

struct Captured {
    MessageHandle handle;
    const std::uint8_t* buffer;
    std::size_t length;
};

std::shared_ptr<rclcpp::SerializedMessage> serialize(std::uint64_t sequence) {
    std_msgs::msg::UInt64 value;
    value.data = sequence;
    const std::shared_ptr<rclcpp::SerializedMessage> serialized =
        std::make_shared<rclcpp::SerializedMessage>();
    rclcpp::Serialization<std_msgs::msg::UInt64> serializer;
    serializer.serialize_message(&value, serialized.get());
    return serialized;
}

std::uint64_t deserialize(const rclcpp::SerializedMessage& message) {
    std_msgs::msg::UInt64 value;
    rclcpp::Serialization<std_msgs::msg::UInt64> serializer;
    serializer.deserialize_message(&message, &value);
    return value.data;
}

void ownership_probe(PoolMode mode) {
    const std::shared_ptr<PoolState> pool = std::make_shared<PoolState>(kPoolCapacity, mode);
    const std::shared_ptr<rclcpp::Node> node = std::make_shared<rclcpp::Node>("phase0_ownership");
    const rclcpp::CallbackGroup::SharedPtr group =
        node->create_callback_group(rclcpp::CallbackGroupType::Reentrant);
    rclcpp::SubscriptionOptions options;
    options.callback_group = group;
    std::mutex capture_mutex;
    std::vector<Captured> captured;
    std::set<std::thread::id> worker_ids;
    std::atomic<bool> callback_failed{false};
    const rclcpp::GenericSubscription::SharedPtr subscription = node->create_generic_subscription(
        kTopic,
        kType,
        rclcpp::QoS(kPoolCapacity).reliable(),
        [&](std::shared_ptr<rclcpp::SerializedMessage> serialized) {
            try {
                MessageHandle handle = pool->acquire();
                require(static_cast<bool>(handle), "ROS callback pool exhausted");
                const auto& bytes = serialized->get_rcl_serialized_message();
                require(
                    bytes.allocator.allocate != nullptr && bytes.allocator.deallocate != nullptr,
                    "invalid callback allocator");
                handle->payload_owner = serialized;
                handle->payload_bytes = bytes.buffer_length;
                const std::lock_guard<std::mutex> lock(capture_mutex);
                captured.push_back(Captured{std::move(handle), bytes.buffer, bytes.buffer_length});
                worker_ids.insert(std::this_thread::get_id());
            } catch (const std::exception&) {
                callback_failed.store(true);
            }
            // Force overlap so a real MultiThreadedExecutor exercises TLS caches
            // on more than one callback thread, not a synthetic executor mock.
            std::this_thread::sleep_for(kCallbackOverlap);
        },
        options);
    const rclcpp::Publisher<std_msgs::msg::UInt64>::SharedPtr publisher =
        node->create_publisher<std_msgs::msg::UInt64>(
            kTopic,
            rclcpp::QoS(kPoolCapacity).reliable());
    rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions{}, kThreads);
    executor.add_node(node);
    std::thread spinner([&executor]() { executor.spin(); });
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (publisher->get_subscription_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kPoll);
    }
    const bool matched = publisher->get_subscription_count() > 0;
    if (matched) {
        for (std::size_t index = 0; index < kSamples; ++index) {
            std_msgs::msg::UInt64 value;
            value.data = index;
            publisher->publish(value);
        }
    }
    while (std::chrono::steady_clock::now() < deadline) {
        {
            const std::lock_guard<std::mutex> lock(capture_mutex);
            if (captured.size() == kSamples) {
                break;
            }
        }
        std::this_thread::sleep_for(kPoll);
    }
    executor.cancel();
    spinner.join();
    executor.remove_node(node);
    require(matched, "DDS reliable publisher/subscriber never matched");
    require(!callback_failed.load() && captured.size() == kSamples, "lost callback messages");
    require(worker_ids.size() > 1, "multiple executor callback threads not observed");

    std::this_thread::sleep_for(kConsumerDelay);
    std::set<std::uint64_t> sequences;
    for (const Captured& item : captured) {
        const std::shared_ptr<const rclcpp::SerializedMessage> serialized =
            std::static_pointer_cast<const rclcpp::SerializedMessage>(item.handle->payload_owner);
        const auto& bytes = serialized->get_rcl_serialized_message();
        require(
            bytes.buffer == item.buffer && bytes.buffer_length == item.length,
            "forwarding changed payload storage");
        sequences.insert(deserialize(*serialized));
    }
    require(
        sequences.size() == kSamples && *sequences.begin() == 0 &&
            *sequences.rbegin() == kSamples - 1,
        "delayed payload content corrupt");
    captured.clear();
    pool->release_thread_cache();
    const PoolSnapshot state = pool->snapshot();
    require(
        state.in_use == 0 && state.tls_free == 0 && state.global_free == kPoolCapacity &&
            state.fallback_in_use == 0 && state.internal_errors == 0,
        "Executor TLS did not return all fixed nodes");
    std::cout << "PASS ownership mode=" << (mode == PoolMode::Tls ? "tls" : "global_mutex")
              << " samples=" << sequences.size() << " callback_threads=" << worker_ids.size()
              << " delay_ms=500 buffer_identity=preserved allocator=valid\n";
}

void register_topic(rosbag2_cpp::Writer& writer, const rosbag2_storage::TopicMetadata& topic) {
#ifdef ROS2_BAG_JAZZY
    rosbag2_cpp::LocalMessageDefinitionSource definitions;
    const rosbag2_storage::MessageDefinition definition = definitions.get_full_text(topic.type);
    require(!definition.encoded_message_definition.empty(), "empty explicit MessageDefinition");
    writer.create_topic(topic, definition);
#else
    writer.create_topic(topic);
#endif
}

void writer_probe(const std::filesystem::path& base, const std::string& storage_config) {
    const std::filesystem::path root =
        base / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    require(!std::filesystem::exists(root), "probe bag destination already exists");
    std::filesystem::create_directories(root);
    const std::shared_ptr<rclcpp::SerializedMessage> payload = serialize(kExpectedPayload);
    const std::uint8_t* original = payload->get_rcl_serialized_message().buffer;
    for (const std::string segment : {"first", "replay"}) {
        rosbag2_cpp::Writer writer;
        rosbag2_storage::StorageOptions storage;
        storage.uri = (root / segment).string();
        storage.storage_id = "mcap";
        storage.max_cache_size = 0;
        storage.storage_config_uri = storage_config;
        writer.open(storage);
        rosbag2_storage::TopicMetadata topic;
        topic.name = kTopic;
        topic.type = kType;
        topic.serialization_format = "cdr";
        register_topic(writer, topic);
        for (std::size_t index = 0; index < kSamples; ++index) {
            writer.write(
                bind_payload(payload, kEpochNs + static_cast<std::int64_t>(index), kTopic));
        }
        writer.close();
        require(
            payload->get_rcl_serialized_message().buffer == original,
            "Writer stole original payload");
        require(deserialize(*payload) == kExpectedPayload, "snapshot owner emptied by Writer");
        rosbag2_cpp::Reader reader;
        reader.open(storage);
        std::size_t count = 0;
        while (reader.has_next()) {
            const auto message = reader.read_next();
            rclcpp::SerializedMessage serialized(*message->serialized_data);
            require(deserialize(serialized) == kExpectedPayload, "MCAP payload mismatch");
#ifdef ROS2_BAG_JAZZY
            require(
                message->recv_timestamp == kEpochNs + static_cast<std::int64_t>(count),
                "MCAP receive timestamp mismatch");
            require(
                message->send_timestamp == message->recv_timestamp - 1,
                "MCAP send timestamp mismatch");
#else
            require(
                message->time_stamp == kEpochNs + static_cast<std::int64_t>(count),
                "MCAP timestamp mismatch");
#endif
            ++count;
        }
        require(count == kSamples, "MCAP count mismatch");
    }
    std::cout << "bag_root=" << root << "\n";
    std::cout << "PASS writer segments=2 messages_per_segment=100 snapshot_replay=valid\n";
}

void rewrite_custom_probe(
    const std::string& source,
    const std::string& destination,
    const std::string& storage_config) {
    require(!std::filesystem::exists(destination), "custom probe refuses to overwrite output");
    rosbag2_storage::StorageOptions input;
    input.uri = source;
    input.storage_id = "mcap";
    rosbag2_cpp::Reader reader;
    reader.open(input);
    rosbag2_storage::StorageOptions output;
    output.uri = destination;
    output.storage_id = "mcap";
    output.max_cache_size = 0;
    output.storage_config_uri = storage_config;
    rosbag2_cpp::Writer writer;
    writer.open(output);
    for (const rosbag2_storage::TopicMetadata& topic : reader.get_all_topics_and_types()) {
        register_topic(writer, topic);
    }
    std::size_t count = 0;
    while (reader.has_next()) {
        writer.write(reader.read_next());
        ++count;
    }
    writer.close();
    require(count == 20, "custom fixture count mismatch");
    std::cout << "PASS C++ custom WriterAdapter registration/writes count=" << count << '\n';
}

void transport_probe(bool publish) {
    const std::shared_ptr<rclcpp::Node> node = std::make_shared<rclcpp::Node>(
        publish ? "phase0_transport_publisher" : "phase0_transport_subscriber");
    const rclcpp::QoS qos = rclcpp::QoS(kPoolCapacity).reliable().transient_local();
    if (publish) {
        const auto publisher = node->create_publisher<std_msgs::msg::UInt64>(kTopic, qos);
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (publisher->get_subscription_count() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            rclcpp::spin_some(node);
            std::this_thread::sleep_for(kPoll);
        }
        require(publisher->get_subscription_count() > 0, "transport QoS/discovery not matched");
        for (std::size_t index = 0; index < kSamples; ++index) {
            std_msgs::msg::UInt64 value;
            value.data = index;
            publisher->publish(value);
            std::this_thread::sleep_for(kPoll);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "PASS transport publisher count=100 matched=true\n";
        return;
    }
    std::set<std::uint64_t> sequences;
    std::vector<std::shared_ptr<rclcpp::SerializedMessage>> received;
    const auto subscription = node->create_generic_subscription(
        kTopic,
        kType,
        qos,
        [&](std::shared_ptr<rclcpp::SerializedMessage> message) {
            if (sequences.insert(deserialize(*message)).second) {
                received.push_back(std::move(message));
            }
        });
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (sequences.size() < kSamples && std::chrono::steady_clock::now() < deadline) {
        rclcpp::spin_some(node);
        std::this_thread::sleep_for(kPoll);
    }
    require(
        sequences.size() == kSamples && *sequences.begin() == 0 &&
            *sequences.rbegin() == kSamples - 1,
        "transport did not receive all unique sequences");
    // Persist actual received CDR, not regenerated publisher values. A unique
    // directory keeps repeated network experiments independent of earlier bags.
    const std::filesystem::path bag =
        std::filesystem::temp_directory_path() /
        ("phase0-dds-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    rosbag2_storage::StorageOptions storage;
    storage.uri = bag.string();
    storage.storage_id = "mcap";
    rosbag2_cpp::Writer writer;
    writer.open(storage);
    rosbag2_storage::TopicMetadata topic;
    topic.name = kTopic;
    topic.type = kType;
    topic.serialization_format = "cdr";
    register_topic(writer, topic);
    for (std::size_t index = 0; index < received.size(); ++index) {
        writer.write(
            bind_payload(received[index], kEpochNs + static_cast<std::int64_t>(index), kTopic));
    }
    writer.close();
    rosbag2_cpp::Reader reader;
    reader.open(storage);
    std::set<std::uint64_t> persisted;
    std::size_t count = 0;
    while (reader.has_next()) {
        const auto message = reader.read_next();
        rclcpp::SerializedMessage payload(*message->serialized_data);
        persisted.insert(deserialize(payload));
        ++count;
    }
    require(count == kSamples && persisted == sequences, "DDS MCAP count/value mismatch");
    std::cout << "PASS transport MCAP received_messages=" << count << " bag=" << bag << '\n';
    std::cout << "PASS transport subscriber unique_sequences=100 qos=reliable,transient_local\n";
}
}  // namespace
}  // namespace ros2_bag::phase0

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    int result = 0;
    try {
        const std::string mode = argc > 1 ? argv[1] : "ownership";
        if (mode == "ownership") {
            ros2_bag::phase0::ownership_probe(ros2_bag::phase0::PoolMode::Tls);
            ros2_bag::phase0::ownership_probe(ros2_bag::phase0::PoolMode::GlobalMutex);
        } else if (mode == "writer" && argc == 4) {
            ros2_bag::phase0::writer_probe(argv[2], argv[3]);
        } else if (mode == "rewrite-custom" && argc == 5) {
            ros2_bag::phase0::rewrite_custom_probe(argv[2], argv[3], argv[4]);
        } else if (mode == "publish" || mode == "subscribe") {
            ros2_bag::phase0::transport_probe(mode == "publish");
        } else {
            throw std::runtime_error(
                "usage: phase0_ros_probe ownership|writer OUTPUT CONFIG|rewrite-custom INPUT "
                "OUTPUT CONFIG|publish|subscribe");
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    rclcpp::shutdown();
    return result;
}
