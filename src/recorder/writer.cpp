#include "ros2_bag/recorder/writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <mcap/reader.hpp>
#include <rosbag2_storage/serialized_bag_message.hpp>
#include <rosbag2_storage/storage_factory.hpp>
#include <rosbag2_storage/storage_interfaces/read_write_interface.hpp>
#include <rosbag2_storage/storage_options.hpp>
#include <rosbag2_storage/topic_metadata.hpp>

#if __has_include(<rosbag2_storage/message_definition.hpp>)
#include <rosbag2_storage/message_definition.hpp>
#define ROS2_BAG_HAS_ROSBAG2_MESSAGE_DEFINITION 1
#else
#define ROS2_BAG_HAS_ROSBAG2_MESSAGE_DEFINITION 0
#endif

#include "ros2_bag/recorder/schema.hpp"

namespace ros2_bag::recorder {
namespace {

void sync_path(const std::filesystem::path& path, int flags) {
    const auto descriptor = ::open(path.c_str(), flags | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open " + path.string() + " for synchronization");
    }
    const auto synchronized = ::fsync(descriptor);
    const auto close_result = ::close(descriptor);
    if (synchronized != 0 || close_result != 0) {
        throw std::runtime_error("cannot synchronize " + path.string());
    }
}

}  // namespace

class Writer::WriterState {
public:
    std::unique_ptr<rosbag2_storage::StorageFactory> storage_factory;
    std::shared_ptr<rosbag2_storage::storage_interfaces::ReadWriteInterface> storage;
    std::filesystem::path directory;
    bool open = false;
    std::unordered_map<TopicId, TopicInfo> topics;
};

void Writer::finalize_segment() {
    std::vector<std::filesystem::path> directories;
    directories.push_back(state_->directory);

    std::error_code filesystem_error;
    for (std::filesystem::recursive_directory_iterator entry(state_->directory, filesystem_error),
         end;
         !filesystem_error && entry != end;
         entry.increment(filesystem_error)) {
        if (entry->is_regular_file(filesystem_error)) {
            if (filesystem_error) {
                break;
            }
            sync_path(entry->path(), O_RDONLY);
            continue;
        }
        if (entry->is_directory(filesystem_error)) {
            if (filesystem_error) {
                break;
            }
            directories.push_back(entry->path());
        }
    }
    if (filesystem_error) {
        throw std::runtime_error(filesystem_error.message());
    }
    for (auto entry = directories.rbegin(); entry != directories.rend(); ++entry) {
        sync_path(*entry, O_RDONLY | O_DIRECTORY);
    }

    std::filesystem::path mcap_file;
    for (std::filesystem::directory_iterator entry(state_->directory, filesystem_error), end;
         !filesystem_error && entry != end;
         entry.increment(filesystem_error)) {
        if (!entry->is_regular_file(filesystem_error) || entry->path().extension() != ".mcap") {
            continue;
        }
        if (!mcap_file.empty()) {
            throw std::runtime_error("segment contains multiple MCAP files");
        }
        mcap_file = entry->path();
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
    for (const auto& topic : state_->topics) {
        const auto channel =
            std::find_if(channels.begin(), channels.end(), [&topic](const auto& candidate) {
                return candidate.second->topic == topic.second.name;
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
        if (!topic.second.schema_sha256.empty() &&
            actual_schema_sha256 != topic.second.schema_sha256) {
            reader.close();
            throw std::runtime_error(
                "closed MCAP segment schema differs from the frozen schema: expected " +
                topic.second.schema_sha256 + ", got " + actual_schema_sha256);
        }
    }
    reader.close();
}

Writer::Writer(CompressionMode compression)
: compression_(compression), state_(std::make_unique<WriterState>()) {}

Writer::~Writer() { (void)close(); }

Result<void, Error> Writer::open(const std::filesystem::path& directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_->open) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::InvalidState, Effects::None, "writer segment is already open"});
    }

    try {
        rosbag2_storage::StorageOptions options;
        options.storage_id = "mcap";
        options.max_cache_size = 0;
        if (compression_ == CompressionMode::Lz4) {
            options.storage_config_uri = resolve_mcap_config().string();
        }

        std::filesystem::create_directories(directory);
        options.uri = (directory / "data").string();

        auto storage_factory = std::make_unique<rosbag2_storage::StorageFactory>();
        auto storage = storage_factory->open_read_write(options);
        if (!storage) {
            return Result<void, Error>(
                unexpect,
                Error{
                    ErrorCode::StorageError,
                    Effects::None,
                    "failed to open rosbag2_storage_mcap"});
        }
        state_->storage_factory = std::move(storage_factory);
        state_->storage = std::move(storage);

        state_->directory = directory;
        state_->open = true;
        return Result<void, Error>();
    } catch (const std::exception& exception) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::StorageError, Effects::None, exception.what()});
    }
}

Result<void, Error> Writer::register_topic(const TopicInfo& topic) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_->open) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::InvalidState, Effects::None, "writer segment is not open"});
    }
    if (!state_->storage) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::InvalidState, Effects::None, "MCAP storage is unavailable"});
    }

    const auto schema = resolve_schema(topic.type);
    if (!schema) {
        return Result<void, Error>(unexpect, schema.error());
    }
    if (!topic.schema_sha256.empty() && schema->sha256 != topic.schema_sha256) {
        return Result<void, Error>(
            unexpect,
            Error{
                ErrorCode::InvalidArgument,
                Effects::None,
                "topic schema differs from the frozen schema"});
    }

    try {
        rosbag2_storage::TopicMetadata metadata;
        metadata.name = topic.name;
        metadata.type = topic.type;
        metadata.serialization_format = "cdr";

#if ROS2_BAG_HAS_ROSBAG2_MESSAGE_DEFINITION
        if (topic.type_hash) {
            metadata.type_description_hash = *topic.type_hash;
        }
        rosbag2_storage::MessageDefinition definition;
        definition.topic_type = topic.type;
        definition.encoding = schema->encoding;
        definition.encoded_message_definition = schema->definition;
        definition.type_hash = topic.type_hash.value_or("");
        state_->storage->create_topic(metadata, definition);
#else
        state_->storage->create_topic(metadata);
#endif

        state_->topics[topic.topic_id] = topic;
        return Result<void, Error>();
    } catch (const std::exception& exception) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::StorageError, Effects::None, exception.what()});
    }
}

Result<void, Error> Writer::write_batch(const std::vector<RecordedMessage>& messages) {
    if (messages.empty()) {
        return Result<void, Error>();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_->open) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::InvalidState, Effects::None, "writer segment is not open"});
    }

    for (const auto& message : messages) {
        if (!message.message || !message.topic) {
            return Result<void, Error>(
                unexpect,
                Error{
                    ErrorCode::InternalError,
                    Effects::None,
                    "recorded message is missing serialized data or topic identity"});
        }
    }

    try {
        if (!state_->storage) {
            return Result<void, Error>(
                unexpect,
                Error{ErrorCode::InvalidState, Effects::None, "MCAP storage is unavailable"});
        }

        std::vector<std::shared_ptr<const rosbag2_storage::SerializedBagMessage>> batch;
        batch.reserve(messages.size());
        for (const auto& message : messages) {
            auto storage_message = std::make_shared<rosbag2_storage::SerializedBagMessage>();
            auto& serialized = message.message->get_rcl_serialized_message();
            storage_message->serialized_data =
                std::shared_ptr<rcutils_uint8_array_t>(message.message, &serialized);
#if ROS2_BAG_HAS_ROSBAG2_MESSAGE_DEFINITION
            storage_message->recv_timestamp = message.receive_timestamp_ns;
            storage_message->send_timestamp = message.receive_timestamp_ns;
#else
            storage_message->time_stamp = message.receive_timestamp_ns;
#endif
            storage_message->topic_name = message.topic->name;
            batch.push_back(std::move(storage_message));
        }
        state_->storage->write(batch);
        return Result<void, Error>();
    } catch (const std::exception& exception) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::StorageError, Effects::ConfirmedPartial, exception.what()});
    }
}

Result<void, Error> Writer::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_->open) {
        return Result<void, Error>();
    }

    try {
        state_->storage.reset();
        state_->storage_factory.reset();
        state_->open = false;
        finalize_segment();
        state_->topics.clear();
        state_->directory.clear();
        return Result<void, Error>();
    } catch (const std::exception& exception) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::StorageError, Effects::ConfirmedPartial, exception.what()});
    }
}

std::filesystem::path Writer::resolve_mcap_config() {
    try {
        const auto installed =
            std::filesystem::path(ament_index_cpp::get_package_share_directory("ros2_bag")) /
            "config" / "mcap.yaml";
        if (std::filesystem::is_regular_file(installed)) {
            return installed;
        }
    } catch (const std::exception&) {
    }

    const auto working = std::filesystem::current_path() / "config" / "mcap.yaml";
    if (std::filesystem::is_regular_file(working)) {
        return working;
    }

    const auto source = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                        "config" / "mcap.yaml";
    if (std::filesystem::is_regular_file(source)) {
        return source;
    }

    throw std::runtime_error("MCAP storage configuration file is unavailable");
}

}  // namespace ros2_bag::recorder
