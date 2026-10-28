#include "ros2_bag/recorder/writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <mcap/reader.hpp>
#include <rclcpp/time.hpp>
#include <rosbag2_cpp/writer.hpp>
#include <rosbag2_storage/storage_options.hpp>
#include <rosbag2_storage/topic_metadata.hpp>

#include "ros2_bag/recorder/schema.hpp"

namespace ros2_bag::recorder {
namespace {

Error writer_error(const std::exception& exception) {
    return {ErrorCode::StorageError, Effects::ConfirmedPartial, exception.what()};
}

Error unavailable() { return {ErrorCode::Unavailable, Effects::None, "writer is stopping"}; }

void sync_path(const std::filesystem::path& path, int flags) {
    const auto descriptor = ::open(path.c_str(), flags | O_CLOEXEC);
    if (descriptor < 0)
        throw std::runtime_error("cannot open " + path.string() + " for synchronization");
    const auto synchronized = ::fsync(descriptor);
    const auto close_result = ::close(descriptor);
    if (synchronized != 0 || close_result != 0) {
        throw std::runtime_error("cannot synchronize " + path.string());
    }
}

void sync_segment(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> directories;
    directories.push_back(directory);
    std::error_code filesystem_error;
    for (std::filesystem::recursive_directory_iterator entry(directory, filesystem_error), end;
         !filesystem_error && entry != end;
         entry.increment(filesystem_error)) {
        if (entry->is_regular_file(filesystem_error)) {
            if (filesystem_error) break;
            sync_path(entry->path(), O_RDONLY);
        } else if (entry->is_directory(filesystem_error)) {
            if (filesystem_error) break;
            directories.push_back(entry->path());
        }
    }
    if (filesystem_error) throw std::runtime_error(filesystem_error.message());
    for (auto entry = directories.rbegin(); entry != directories.rend(); ++entry) {
        sync_path(*entry, O_RDONLY | O_DIRECTORY);
    }
}

void verify_segment_schema_closure(
    const std::filesystem::path& directory,
    const std::unordered_map<TopicId, TopicInfo>& topics) {
    std::filesystem::path mcap_file;
    std::error_code filesystem_error;
    for (std::filesystem::directory_iterator entry(directory, filesystem_error), end;
         !filesystem_error && entry != end;
         entry.increment(filesystem_error)) {
        if (entry->is_regular_file(filesystem_error) && entry->path().extension() == ".mcap") {
            if (!mcap_file.empty()) {
                throw std::runtime_error("segment contains multiple MCAP files");
            }
            mcap_file = entry->path();
        }
    }
    if (filesystem_error) {
        throw std::runtime_error(filesystem_error.message());
    }
    if (mcap_file.empty()) {
        throw std::runtime_error("segment MCAP file is missing");
    }
    mcap::McapReader reader;
    if (!reader.open(mcap_file.string()).ok()) {
        throw std::runtime_error("cannot open closed MCAP segment");
    }
    if (!reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok()) {
        reader.close();
        throw std::runtime_error("closed MCAP segment has no valid summary");
    }
    const auto channels = reader.channels();
    const auto schemas = reader.schemas();
    for (const auto& entry : topics) {
        const auto channel =
            std::find_if(channels.begin(), channels.end(), [&entry](const auto& candidate) {
                return candidate.second->topic == entry.second.name;
            });
        if (channel == channels.end() || channel->second->schemaId == 0U) {
            reader.close();
            throw std::runtime_error("closed MCAP segment is missing a topic schema channel");
        }
        const auto schema = schemas.find(channel->second->schemaId);
        if (schema == schemas.end() || schema->second->encoding != "ros2msg" ||
            schema->second->data.empty()) {
            reader.close();
            throw std::runtime_error("closed MCAP segment has an incomplete topic schema");
        }
        const std::string schema_definition(
            reinterpret_cast<const char*>(schema->second->data.data()),
            schema->second->data.size());
        const auto actual_schema_sha256 = schema_sha256(schema_definition);
        if (!entry.second.schema_sha256.empty() &&
            actual_schema_sha256 != entry.second.schema_sha256) {
            reader.close();
            throw std::runtime_error(
                "closed MCAP segment schema differs from the frozen schema: expected " +
                entry.second.schema_sha256 + ", got " + actual_schema_sha256);
        }
    }
    reader.close();
}

}  // namespace

class Writer::WriterState {
public:
    std::unique_ptr<rosbag2_cpp::Writer> writer;
    std::filesystem::path directory;
    bool open = false;
    std::unordered_map<TopicId, TopicInfo> topics;
};

Writer::Writer(std::size_t control_queue_capacity)
: control_queue_capacity_(control_queue_capacity),
  state_(std::make_unique<WriterState>()),
  thread_([this] { run(); }) {}

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
                state_->directory = directory;
                state_->open = true;
                source.set_value();
            } catch (const std::exception& exception) {
                source.set_error(writer_error(exception));
            }
        }))
        source.set_error(unavailable());
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
        }))
        source.set_error(unavailable());
    return completion;
}

Completion<void>
Writer::write(RecordedMessage message, std::string topic_name, std::string type_name) {
    CompletionSource<void> source;
    const auto completion = source.completion();
    if (!submit([this,
                 message = std::move(message),
                 topic_name = std::move(topic_name),
                 type_name = std::move(type_name),
                 source]() mutable {
            try {
                if (!state_->open) throw std::runtime_error("writer segment is not open");
                if (!message.message) throw std::runtime_error("serialized message is missing");
                // rosbag2 takes ownership of the serialized message passed to write().
                // Pool and Snapshot owners must therefore never be handed to it directly.
                auto storage_message =
                    std::make_shared<rclcpp::SerializedMessage>(*message.message);
                state_->writer->write(
                    std::move(storage_message),
                    topic_name,
                    type_name,
                    rclcpp::Time(message.receive_timestamp_ns, RCL_SYSTEM_TIME));
                source.set_value();
            } catch (const std::exception& exception) {
                source.set_error(writer_error(exception));
            }
        }))
        source.set_error(unavailable());
    return completion;
}

Completion<void> Writer::close() {
    CompletionSource<void> source;
    const auto completion = source.completion();
    if (!submit([this, source] {
            try {
                if (state_->open) {
                    state_->writer->close();
                    sync_segment(state_->directory);
                    verify_segment_schema_closure(state_->directory, state_->topics);
                }
                state_->writer.reset();
                state_->topics.clear();
                state_->directory.clear();
                state_->open = false;
                source.set_value();
            } catch (const std::exception& exception) {
                source.set_error(writer_error(exception));
            }
        }))
        source.set_error(unavailable());
    return completion;
}

void Writer::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    condition_.notify_all();
}

}  // namespace ros2_bag::recorder
