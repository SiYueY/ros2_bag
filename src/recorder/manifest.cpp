#include "ros2_bag/recorder/manifest.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <system_error>

namespace ros2_bag::recorder {
namespace {

using nlohmann::json;

Error storage_error(std::string message, Effects effects = Effects::None) {
    return Error{ErrorCode::StorageError, effects, std::move(message)};
}

std::string manifest_message(std::string message) {
    constexpr std::size_t maximum = 512;
    constexpr std::size_t prefix_maximum = maximum - 3;
    if (message.size() <= maximum) return message;
    std::size_t offset = 0;
    while (offset < message.size() && offset < prefix_maximum) {
        const auto lead = static_cast<unsigned char>(message[offset]);
        std::size_t width = 1;
        if ((lead & 0x80U) == 0U) {
            width = 1;
        } else if ((lead & 0xe0U) == 0xc0U) {
            width = 2;
        } else if ((lead & 0xf0U) == 0xe0U) {
            width = 3;
        } else if ((lead & 0xf8U) == 0xf0U) {
            width = 4;
        } else {
            break;
        }
        if (offset + width > prefix_maximum || offset + width > message.size()) break;
        bool valid = true;
        for (std::size_t index = 1; index < width; ++index) {
            if ((static_cast<unsigned char>(message[offset + index]) & 0xc0U) != 0x80U) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        offset += width;
    }
    return message.substr(0, offset) + "...";
}

template <class T>
void optional_to_json(json& value, const char* name, const std::optional<T>& field) {
    value[name] = field ? json(*field) : json(nullptr);
}

template <class T>
std::optional<T> optional_from_json(const json& value, const char* name) {
    if (!value.contains(name) || value.at(name).is_null()) {
        return std::nullopt;
    }
    return value.at(name).get<T>();
}

json encode_error(const Error& error) {
    return {{"code", static_cast<int>(error.code)}, {"effects", static_cast<int>(error.effects)},
            {"message", manifest_message(error.message)}};
}

Error decode_error(const json& value) {
    return {static_cast<ErrorCode>(value.at("code").get<int>()),
            static_cast<Effects>(value.at("effects").get<int>()), value.at("message").get<std::string>()};
}

json encode_config(const TaskConfig& config) {
    json value{{"mode", static_cast<int>(config.mode)},
               {"compression", static_cast<int>(config.compression)},
               {"timestamp_policy", static_cast<int>(config.timestamp_policy)}};
    optional_to_json(value, "duration_ms", config.duration ? std::optional<std::int64_t>(config.duration->count()) : std::nullopt);
    optional_to_json(value, "segment_duration_s", config.segment_duration ? std::optional<std::int64_t>(config.segment_duration->count()) : std::nullopt);
    optional_to_json(value, "retention_duration_s", config.retention_duration ? std::optional<std::int64_t>(config.retention_duration->count()) : std::nullopt);
    return value;
}

TaskConfig decode_config(const json& value) {
    TaskConfig config;
    config.mode = static_cast<TaskMode>(value.at("mode").get<int>());
    config.compression = static_cast<CompressionMode>(value.at("compression").get<int>());
    config.timestamp_policy = static_cast<TimestampPolicy>(value.at("timestamp_policy").get<int>());
    if (const auto duration = optional_from_json<std::int64_t>(value, "duration_ms")) {
        config.duration = std::chrono::milliseconds(*duration);
    }
    if (const auto duration = optional_from_json<std::int64_t>(value, "segment_duration_s")) {
        config.segment_duration = std::chrono::seconds(*duration);
    }
    if (const auto duration = optional_from_json<std::int64_t>(value, "retention_duration_s")) {
        config.retention_duration = std::chrono::seconds(*duration);
    }
    return config;
}

json encode_segment(const SegmentInfo& segment) {
    json value{{"segment_id", segment.segment_id}, {"state", static_cast<int>(segment.state)}, {"relative_path", segment.relative_path}};
    optional_to_json(value, "started_at_ns", segment.started_at_ns);
    optional_to_json(value, "completed_at_ns", segment.completed_at_ns);
    optional_to_json(value, "message_count", segment.message_count);
    optional_to_json(value, "size_bytes", segment.size_bytes);
    return value;
}

SegmentInfo decode_segment(const json& value) {
    SegmentInfo segment;
    segment.segment_id = value.at("segment_id").get<std::uint64_t>();
    segment.state = static_cast<SegmentLifecycleState>(value.at("state").get<int>());
    segment.relative_path = value.at("relative_path").get<std::string>();
    segment.started_at_ns = optional_from_json<std::int64_t>(value, "started_at_ns");
    segment.completed_at_ns = optional_from_json<std::int64_t>(value, "completed_at_ns");
    segment.message_count = optional_from_json<std::uint64_t>(value, "message_count");
    segment.size_bytes = optional_from_json<std::uint64_t>(value, "size_bytes");
    return segment;
}

json encode(const ManifestData& data) {
    json value{{"schema_version", data.schema_version},
               {"manifest_revision", data.manifest_revision},
               {"recording_id", data.task.recording_id},
               {"generation", data.task.generation},
               {"task_state", static_cast<int>(data.task.state)},
               {"task_config", encode_config(data.task.config)},
               {"topic_events_file", data.topic_events_file},
               {"segment_history_summary", {{"compacted_removed_count", data.task.segment_history_summary.compacted_removed_count},
                                            {"compacted_removed_bytes", data.task.segment_history_summary.compacted_removed_bytes}}}};
    optional_to_json(value, "start_time_ns", data.task.start_time_ns);
    optional_to_json(value, "terminal_time_ns", data.task.terminal_time_ns);
    value["error"] = data.task.error ? encode_error(*data.task.error) : json(nullptr);
    value["secondary_errors"] = json::array();
    const auto secondary_count = std::min<std::size_t>(data.secondary_errors.size(), 8);
    for (std::size_t index = 0; index < secondary_count; ++index) {
        value["secondary_errors"].push_back(encode_error(data.secondary_errors[index]));
    }
    value["segments"] = json::array();
    for (const auto& segment : data.segments) value["segments"].push_back(encode_segment(segment));
    return value;
}

ManifestData decode(const json& value) {
    ManifestData data;
    data.schema_version = value.at("schema_version").get<std::uint64_t>();
    data.manifest_revision = value.at("manifest_revision").get<std::uint64_t>();
    data.task.recording_id = value.at("recording_id").get<std::string>();
    data.task.generation = value.at("generation").get<std::uint64_t>();
    data.task.state = static_cast<TaskState>(value.at("task_state").get<int>());
    data.task.config = decode_config(value.at("task_config"));
    data.task.start_time_ns = optional_from_json<std::int64_t>(value, "start_time_ns");
    data.task.terminal_time_ns = optional_from_json<std::int64_t>(value, "terminal_time_ns");
    if (!value.at("error").is_null()) data.task.error = decode_error(value.at("error"));
    data.topic_events_file = value.at("topic_events_file").get<std::string>();
    const auto& summary = value.at("segment_history_summary");
    data.task.segment_history_summary.compacted_removed_count = summary.at("compacted_removed_count").get<std::uint64_t>();
    data.task.segment_history_summary.compacted_removed_bytes = summary.at("compacted_removed_bytes").get<std::uint64_t>();
    for (const auto& error : value.at("secondary_errors")) data.secondary_errors.push_back(decode_error(error));
    for (const auto& segment : value.at("segments")) data.segments.push_back(decode_segment(segment));
    return data;
}

Result<void, Error> write_durable(const std::filesystem::path& path, const json& data) {
    const auto temporary = path.string() + ".tmp";
    const std::string serialized = data.dump(2) + "\n";
    const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file < 0) return Result<void, Error>(unexpect, storage_error(std::strerror(errno)));
    std::size_t offset = 0;
    while (offset < serialized.size()) {
        const auto written = ::write(file, serialized.data() + offset, serialized.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            const auto message = std::strerror(errno);
            ::close(file);
            ::unlink(temporary.c_str());
            return Result<void, Error>(unexpect, storage_error(message));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(file) != 0) {
        const auto message = std::strerror(errno);
        ::close(file);
        ::unlink(temporary.c_str());
        return Result<void, Error>(unexpect, storage_error(message));
    }
    if (::close(file) != 0) {
        ::unlink(temporary.c_str());
        return Result<void, Error>(unexpect, storage_error(std::strerror(errno)));
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const auto message = std::strerror(errno);
        ::unlink(temporary.c_str());
        return Result<void, Error>(unexpect, storage_error(message));
    }
    const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0 || ::fsync(directory) != 0) {
        if (directory >= 0) ::close(directory);
        return Result<void, Error>(unexpect, storage_error("manifest rename durability is uncertain", Effects::Possible));
    }
    ::close(directory);
    return Result<void, Error>();
}

}  // namespace

Result<Manifest, Error> Manifest::create(const std::filesystem::path& task_directory, ManifestData data) {
    std::error_code error;
    std::filesystem::create_directories(task_directory, error);
    if (error) return Result<Manifest, Error>(unexpect, storage_error(error.message()));
    if (data.manifest_revision != 1) return Result<Manifest, Error>(unexpect, storage_error("first manifest revision must be one"));
    const auto persisted = write_durable(task_directory / "manifest.json", encode(data));
    if (!persisted) {
        if (persisted.error().effects == Effects::None) {
            std::filesystem::remove_all(task_directory, error);
        }
        return Result<Manifest, Error>(unexpect, persisted.error());
    }
    return Result<Manifest, Error>(Manifest(task_directory, std::move(data)));
}

Result<Manifest, Error> Manifest::load(const std::filesystem::path& task_directory) {
    try {
        std::ifstream stream(task_directory / "manifest.json");
        if (!stream) return Result<Manifest, Error>(unexpect, storage_error("cannot open manifest"));
        json value;
        stream >> value;
        auto data = decode(value);
        if (data.manifest_revision == 0) return Result<Manifest, Error>(unexpect, storage_error("invalid manifest revision"));
        return Result<Manifest, Error>(Manifest(task_directory, std::move(data)));
    } catch (const std::exception& exception) {
        return Result<Manifest, Error>(unexpect, storage_error(exception.what()));
    }
}

Result<void, Error> Manifest::commit(ManifestData data) {
    if (data.manifest_revision != data_.manifest_revision + 1 || data_.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Result<void, Error>(unexpect, storage_error("invalid manifest revision transition"));
    }
    const auto persisted = write_durable(task_directory_ / "manifest.json", encode(data));
    if (!persisted) return persisted;
    data_ = std::move(data);
    return Result<void, Error>();
}

}  // namespace ros2_bag::recorder
