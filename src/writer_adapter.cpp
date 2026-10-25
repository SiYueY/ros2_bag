// WriterAdapter implementation. All operations run only on the Writer thread.
#include "ros2_bag/writer_adapter.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#include <rosbag2_storage/serialized_bag_message.hpp>
#ifdef ROS2_BAG_JAZZY
#include <rosbag2_cpp/message_definitions/local_message_definition_source.hpp>
#endif

namespace ros2_bag {
namespace {

std::shared_ptr<rosbag2_storage::SerializedBagMessage> make_bag_message(
    const RecordedMessage& message) {
    const std::shared_ptr<rclcpp::SerializedMessage>& owner = message.serialized_message;
    const auto descriptor =
        std::make_shared<rcutils_uint8_array_t>(owner->get_rcl_serialized_message());
    const auto bag_message = std::make_shared<rosbag2_storage::SerializedBagMessage>();
    bag_message->serialized_data = std::shared_ptr<rcutils_uint8_array_t>(
        descriptor.get(),
        [owner, descriptor](rcutils_uint8_array_t*) {});
    bag_message->topic_name = message.topic_info->name;
#ifdef ROS2_BAG_JAZZY
    bag_message->recv_timestamp = message.recv_timestamp_ns;
    bag_message->send_timestamp = message.send_timestamp_ns;
#else
    bag_message->time_stamp = message.recv_timestamp_ns;
#endif
    return bag_message;
}

}  // namespace

WriterAdapter::WriterAdapter(WriteOverride write_override)
: write_override_(std::move(write_override)) {}

bool WriterAdapter::set_error(const std::string& message, std::string* error) noexcept {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool WriterAdapter::open_segment(const WriterOpenOptions& options, std::string* error) noexcept {
    if (open_) {
        return set_error("Writer segment is already open", error);
    }
    try {
        rosbag2_storage::StorageOptions storage;
        storage.uri = options.uri;
        storage.storage_id = options.storage_id;
        storage.max_cache_size = 0;
        storage.storage_config_uri = options.storage_config_uri;
        writer_.open(storage);
        open_ = true;
        registered_topics_.clear();
        return true;
    } catch (const std::exception& exception) {
        return set_error(exception.what(), error);
    }
}

bool WriterAdapter::register_topic(const TopicInfo& topic_info, std::string* error) noexcept {
    if (!open_) {
        return set_error("Writer segment is not open", error);
    }
    if (topic_info.name.empty() || topic_info.type.empty()) {
        return set_error("TopicMetadata requires nonempty name and type", error);
    }
    if (registered_topics_.count(topic_info.name) != 0U) {
        return true;
    }
    try {
        rosbag2_storage::TopicMetadata metadata;
        metadata.name = topic_info.name;
        metadata.type = topic_info.type;
        metadata.serialization_format = topic_info.serialization_format;
#ifdef ROS2_BAG_JAZZY
        rosbag2_cpp::LocalMessageDefinitionSource definitions;
        const rosbag2_storage::MessageDefinition definition =
            definitions.get_full_text(metadata.type);
        if (definition.encoded_message_definition.empty()) {
            return set_error("MessageDefinition is empty", error);
        }
        writer_.create_topic(metadata, definition);
#else
        writer_.create_topic(metadata);
#endif
        registered_topics_.insert(topic_info.name);
        return true;
    } catch (const std::exception& exception) {
        return set_error(exception.what(), error);
    }
}

bool WriterAdapter::write(const RecordedMessage& message, std::string* error) noexcept {
    if (!open_) {
        return set_error("Writer segment is not open", error);
    }
    if (!message.topic_info || !message.serialized_message ||
        message.timestamp_source == TimestampSource::kUnknown) {
        return set_error("RecordedMessage is not initialized", error);
    }
    if (registered_topics_.count(message.topic_info->name) == 0U) {
        return set_error("Topic is not registered in current segment", error);
    }
    const std::pair<TopicId, std::uint64_t> key(message.topic_id, message.topic_epoch);
    if (write_override_) {
        const bool succeeded = write_override_(message, error);
        const std::lock_guard<std::mutex> lock(statistics_mutex_);
        WriterTopicStatistics& statistics = statistics_[key];
        statistics.topic_id = message.topic_id;
        statistics.topic_epoch = message.topic_epoch;
        if (succeeded) {
            ++statistics.written;
        } else {
            ++statistics.write_errors;
        }
        return succeeded;
    }
    try {
        // The configured SequentialWriter path has max_cache_size=0. It calls
        // the MCAP storage writer synchronously, while the temporary descriptor
        // retains the SerializedMessage owner for the complete write() call.
        // Retaining owners until segment close would make Recorder RSS grow with
        // every message in a long segment.
        writer_.write(make_bag_message(message));
        {
            const std::lock_guard<std::mutex> lock(statistics_mutex_);
            WriterTopicStatistics& statistics = statistics_[key];
            statistics.topic_id = message.topic_id;
            statistics.topic_epoch = message.topic_epoch;
            ++statistics.written;
        }
        return true;
    } catch (const std::exception& exception) {
        {
            const std::lock_guard<std::mutex> lock(statistics_mutex_);
            WriterTopicStatistics& statistics = statistics_[key];
            statistics.topic_id = message.topic_id;
            statistics.topic_epoch = message.topic_epoch;
            ++statistics.write_errors;
        }
        return set_error(exception.what(), error);
    }
}

bool WriterAdapter::close(std::string* error) noexcept {
    if (!open_) {
        return true;
    }
    try {
        writer_.close();
        open_ = false;
        registered_topics_.clear();
        return true;
    } catch (const std::exception& exception) {
        return set_error(exception.what(), error);
    }
}

bool WriterAdapter::is_open() const noexcept { return open_; }

void WriterAdapter::reset_statistics() noexcept {
    const std::lock_guard<std::mutex> lock(statistics_mutex_);
    statistics_.clear();
}

std::vector<WriterTopicStatistics> WriterAdapter::statistics() const noexcept {
    const std::lock_guard<std::mutex> lock(statistics_mutex_);
    std::vector<WriterTopicStatistics> result;
    result.reserve(statistics_.size());
    for (const auto& item : statistics_) {
        result.push_back(item.second);
    }
    return result;
}

}  // namespace ros2_bag
