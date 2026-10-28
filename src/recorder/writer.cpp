#include "ros2_bag/recorder/writer.hpp"

#include <exception>
#include <memory>
#include <utility>

#include <rclcpp/time.hpp>
#include <rosbag2_cpp/writer.hpp>
#include <rosbag2_storage/storage_options.hpp>
#include <rosbag2_storage/topic_metadata.hpp>

namespace ros2_bag::recorder {
namespace {

Error writer_error(const std::exception& exception) {
    return {ErrorCode::StorageError, Effects::ConfirmedPartial, exception.what()};
}

Error unavailable() {
    return {ErrorCode::Unavailable, Effects::None, "writer is stopping"};
}

}  // namespace

class Writer::WriterState {
public:
    std::unique_ptr<rosbag2_cpp::Writer> writer;
    bool open = false;
    std::unordered_map<TopicId, TopicInfo> topics;
};

Writer::Writer(std::size_t control_queue_capacity)
: control_queue_capacity_(control_queue_capacity), state_(std::make_unique<WriterState>()), thread_([this] { run(); }) {}

Writer::~Writer() {
    shutdown();
    if (thread_.joinable()) thread_.join();
}

bool Writer::submit(std::function<void()> command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || commands_.size() >= control_queue_capacity_) return false;
    commands_.push(std::move(command));
    condition_.notify_one();
    return true;
}

void Writer::run() {
    while (true) {
        std::function<void()> command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !commands_.empty(); });
            if (commands_.empty() && stopping_) return;
            command = std::move(commands_.front());
            commands_.pop();
        }
        command();
    }
}

Completion<void> Writer::open(std::string directory) {
    CompletionSource<void> source;
    const auto completion = source.completion();
    if (!submit([this, directory = std::move(directory), source]() mutable {
            try {
                if (state_->open) throw std::runtime_error("writer segment is already open");
                state_->writer = std::make_unique<rosbag2_cpp::Writer>();
                rosbag2_storage::StorageOptions options;
                options.uri = directory;
                options.storage_id = "mcap";
                options.max_cache_size = 0;
                state_->writer->open(options);
                state_->open = true;
                source.set_value();
            } catch (const std::exception& exception) {
                source.set_error(writer_error(exception));
            }
        })) source.set_error(unavailable());
    return completion;
}

Completion<void> Writer::register_topic(TopicInfo topic) {
    CompletionSource<void> source;
    const auto completion = source.completion();
    if (!submit([this, topic = std::move(topic), source]() mutable {
            try {
                if (!state_->open) throw std::runtime_error("writer segment is not open");
                rosbag2_storage::TopicMetadata metadata;
                metadata.name = topic.name;
                metadata.type = topic.type;
                metadata.serialization_format = "cdr";
                state_->writer->create_topic(metadata);
                state_->topics[topic.topic_id] = std::move(topic);
                source.set_value();
            } catch (const std::exception& exception) {
                source.set_error(writer_error(exception));
            }
        })) source.set_error(unavailable());
    return completion;
}

Completion<void> Writer::write(RecordedMessage message, std::string topic_name, std::string type_name) {
    CompletionSource<void> source;
    const auto completion = source.completion();
    if (!submit([this, message = std::move(message), topic_name = std::move(topic_name), type_name = std::move(type_name), source]() mutable {
            try {
                if (!state_->open) throw std::runtime_error("writer segment is not open");
                state_->writer->write(
                    std::move(message.message), topic_name, type_name,
                    rclcpp::Time(message.receive_timestamp_ns, RCL_SYSTEM_TIME));
                source.set_value();
            } catch (const std::exception& exception) {
                source.set_error(writer_error(exception));
            }
        })) source.set_error(unavailable());
    return completion;
}

Completion<void> Writer::close() {
    CompletionSource<void> source;
    const auto completion = source.completion();
    if (!submit([this, source] {
            try {
                if (state_->open) state_->writer->close();
                state_->writer.reset();
                state_->topics.clear();
                state_->open = false;
                source.set_value();
            } catch (const std::exception& exception) {
                source.set_error(writer_error(exception));
            }
        })) source.set_error(unavailable());
    return completion;
}

void Writer::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    condition_.notify_all();
}

}  // namespace ros2_bag::recorder
