#include "ros2_bag/recorder/manifest.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

namespace ros2_bag::recorder {
namespace {

Error storage_error(std::string message, Effects effects = Effects::None) {
    return Error{ErrorCode::StorageError, effects, std::move(message)};
}

}  // namespace

Manifest::Manifest(std::filesystem::path task_directory, ManifestData data)
: task_directory_(std::move(task_directory)), data_(std::move(data)) {}

const ManifestData& Manifest::data() const noexcept { return data_; }

const std::filesystem::path& Manifest::task_directory() const noexcept { return task_directory_; }

std::string Manifest::sanitize_error_message(std::string message) {
    constexpr std::size_t kMaxManifestErrorMessageBytes = 512;
    constexpr std::size_t prefix_maximum = kMaxManifestErrorMessageBytes - 3;
    if (message.size() <= kMaxManifestErrorMessageBytes) {
        return message;
    }

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
        if (offset + width > prefix_maximum || offset + width > message.size()) {
            break;
        }

        bool valid = true;
        for (std::size_t index = 1; index < width; ++index) {
            if ((static_cast<unsigned char>(message[offset + index]) & 0xc0U) != 0x80U) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            break;
        }
        offset += width;
    }
    return message.substr(0, offset) + "...";
}

std::string Manifest::serialize(const ManifestData& data) {
    using nlohmann::json;

    json task_config{
        {"mode", static_cast<int>(data.task.config.mode)},
        {"compression", static_cast<int>(data.task.config.compression)},
        {"timestamp_policy", static_cast<int>(data.task.config.timestamp_policy)}};

    task_config["duration_ms"] = nullptr;
    task_config["segment_duration_s"] = nullptr;
    task_config["retention_duration_s"] = nullptr;
    if (data.task.config.mode == TaskMode::Timed) {
        task_config["duration_ms"] = data.task.config.timed.duration.count();
    }
    if (data.task.config.mode == TaskMode::Rolling) {
        task_config["segment_duration_s"] = data.task.config.rolling.segment_duration.count();
        task_config["retention_duration_s"] = data.task.config.rolling.retention_duration.count();
    }

    json document{
        {"schema_version", data.schema_version},
        {"manifest_revision", data.manifest_revision},
        {"recording_id", data.task.recording_id},
        {"generation", data.task.generation},
        {"task_state", static_cast<int>(data.task.state)},
        {"task_config", std::move(task_config)},
        {"topic_events_file", data.topic_events_file},
        {"segment_history_summary",
         {{"compacted_removed_count", data.task.segment_history.removed_count},
          {"compacted_removed_bytes", data.task.segment_history.removed_bytes}}}};

    document["start_time_ns"] = nullptr;
    if (data.task.start_time_ns != 0) {
        document["start_time_ns"] = data.task.start_time_ns;
    }
    document["terminal_time_ns"] = nullptr;
    if (data.task.terminal_time_ns != 0) {
        document["terminal_time_ns"] = data.task.terminal_time_ns;
    }

    document["error"] = nullptr;
    if (data.task.error) {
        document["error"] = {
            {"code", static_cast<int>(data.task.error->code)},
            {"effects", static_cast<int>(data.task.error->effects)},
            {"message", sanitize_error_message(data.task.error->message)}};
    }

    document["secondary_errors"] = json::array();
    const auto secondary_count = std::min<std::size_t>(data.secondary_errors.size(), 8);
    for (std::size_t index = 0; index < secondary_count; ++index) {
        const auto& error = data.secondary_errors[index];
        document["secondary_errors"].push_back(
            {{"code", static_cast<int>(error.code)},
             {"effects", static_cast<int>(error.effects)},
             {"message", sanitize_error_message(error.message)}});
    }

    document["segments"] = json::array();
    for (const auto& segment : data.segments) {
        json item{
            {"segment_id", segment.segment_id},
            {"state", static_cast<int>(segment.state)},
            {"relative_path", segment.relative_path}};

        item["started_at_ns"] = nullptr;
        if (segment.started_at_ns != 0) {
            item["started_at_ns"] = segment.started_at_ns;
        }
        item["completed_at_ns"] = nullptr;
        if (segment.completed_at_ns != 0) {
            item["completed_at_ns"] = segment.completed_at_ns;
        }
        item["message_count"] = nullptr;
        if (segment.message_count) {
            item["message_count"] = *segment.message_count;
        }
        item["size_bytes"] = nullptr;
        if (segment.size_bytes) {
            item["size_bytes"] = *segment.size_bytes;
        }

        document["segments"].push_back(std::move(item));
    }

    return document.dump(2) + "\n";
}

Result<ManifestData, Error> Manifest::deserialize(std::string_view content) {
    using nlohmann::json;

    try {
        const auto document = json::parse(content);
        ManifestData data;
        data.schema_version = document.at("schema_version").get<std::uint64_t>();
        data.manifest_revision = document.at("manifest_revision").get<std::uint64_t>();
        data.task.recording_id = document.at("recording_id").get<std::string>();
        data.task.generation = document.at("generation").get<std::uint64_t>();
        data.task.state = static_cast<TaskState>(document.at("task_state").get<int>());
        data.topic_events_file = document.at("topic_events_file").get<std::string>();

        const auto& config = document.at("task_config");
        data.task.config.mode = static_cast<TaskMode>(config.at("mode").get<int>());
        data.task.config.compression =
            static_cast<CompressionMode>(config.at("compression").get<int>());
        data.task.config.timestamp_policy =
            static_cast<TimestampPolicy>(config.at("timestamp_policy").get<int>());
        if (data.task.config.mode == TaskMode::Timed && !config.at("duration_ms").is_null()) {
            data.task.config.timed.duration =
                std::chrono::milliseconds(config.at("duration_ms").get<std::int64_t>());
        }
        if (data.task.config.mode == TaskMode::Rolling) {
            if (!config.at("segment_duration_s").is_null()) {
                data.task.config.rolling.segment_duration =
                    std::chrono::seconds(config.at("segment_duration_s").get<std::int64_t>());
            }
            if (!config.at("retention_duration_s").is_null()) {
                data.task.config.rolling.retention_duration =
                    std::chrono::seconds(config.at("retention_duration_s").get<std::int64_t>());
            }
        }

        if (!document.at("start_time_ns").is_null()) {
            data.task.start_time_ns = document.at("start_time_ns").get<std::int64_t>();
        }
        if (!document.at("terminal_time_ns").is_null()) {
            data.task.terminal_time_ns = document.at("terminal_time_ns").get<std::int64_t>();
        }

        if (!document.at("error").is_null()) {
            const auto& error = document.at("error");
            data.task.error = Error{
                static_cast<ErrorCode>(error.at("code").get<int>()),
                static_cast<Effects>(error.at("effects").get<int>()),
                error.at("message").get<std::string>()};
        }

        const auto& summary = document.at("segment_history_summary");
        data.task.segment_history.removed_count =
            summary.at("compacted_removed_count").get<std::uint64_t>();
        data.task.segment_history.removed_bytes =
            summary.at("compacted_removed_bytes").get<std::uint64_t>();

        for (const auto& error : document.at("secondary_errors")) {
            data.secondary_errors.push_back(Error{
                static_cast<ErrorCode>(error.at("code").get<int>()),
                static_cast<Effects>(error.at("effects").get<int>()),
                error.at("message").get<std::string>()});
        }

        for (const auto& item : document.at("segments")) {
            SegmentInfo segment;
            segment.segment_id = item.at("segment_id").get<std::uint64_t>();
            segment.state = static_cast<SegmentLifecycleState>(item.at("state").get<int>());
            segment.relative_path = item.at("relative_path").get<std::string>();
            if (!item.at("started_at_ns").is_null()) {
                segment.started_at_ns = item.at("started_at_ns").get<std::int64_t>();
            }
            if (!item.at("completed_at_ns").is_null()) {
                segment.completed_at_ns = item.at("completed_at_ns").get<std::int64_t>();
            }
            if (!item.at("message_count").is_null()) {
                segment.message_count = item.at("message_count").get<std::uint64_t>();
            }
            if (!item.at("size_bytes").is_null()) {
                segment.size_bytes = item.at("size_bytes").get<std::uint64_t>();
            }
            data.segments.push_back(std::move(segment));
        }

        if (data.manifest_revision == 0) {
            return Result<ManifestData, Error>(
                unexpect, storage_error("invalid manifest revision"));
        }
        return Result<ManifestData, Error>(std::move(data));
    } catch (const std::exception& exception) {
        return Result<ManifestData, Error>(unexpect, storage_error(exception.what()));
    }
}

Result<void, Error> Manifest::write_durable(
    const std::filesystem::path& path, std::string_view content) {
    const auto temporary = path.string() + ".tmp";
    const auto parent_path = path.parent_path();
    auto uncertain = storage_error("manifest rename durability is uncertain", Effects::Possible);
    const int file =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (file < 0) {
        return Result<void, Error>(unexpect, storage_error(std::strerror(errno)));
    }

    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto written = ::write(file, content.data() + offset, content.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
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

    const int directory =
        ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0 || ::fsync(directory) != 0) {
        if (directory >= 0) {
            ::close(directory);
        }
        return Result<void, Error>(unexpect, std::move(uncertain));
    }
    ::close(directory);
    return Result<void, Error>();
}

Result<Manifest, Error> Manifest::create(
    const std::filesystem::path& task_directory, ManifestData data) {
    bool created = false;
    try {
        if (data.manifest_revision != 1) {
            return Result<Manifest, Error>(
                unexpect, storage_error("first manifest revision must be one"));
        }
        const auto serialized = serialize(data);
        const auto manifest_path = task_directory / "manifest.json";
        Manifest result(task_directory, std::move(data));
        // Include every ancestor: the output root itself can have been created by
        // this first Start, and syncing only its contents does not persist its name.
        std::vector<std::filesystem::path> ancestors;
        auto parent_path = std::filesystem::absolute(task_directory).parent_path();
        while (true) {
            ancestors.push_back(parent_path);
            const auto parent = parent_path.parent_path();
            if (parent == parent_path) {
                break;
            }
            parent_path = parent;
        }
        auto uncertain = storage_error("task directory durability is uncertain", Effects::Possible);
        std::error_code error;
        std::filesystem::create_directories(task_directory.parent_path(), error);
        if (!error && !std::filesystem::create_directory(task_directory, error)) {
            return Result<Manifest, Error>(
                unexpect, storage_error("task directory already exists"));
        }
        if (error) {
            return Result<Manifest, Error>(unexpect, storage_error(error.message()));
        }
        created = true;

        auto persisted = write_durable(manifest_path, serialized);
        if (!persisted) {
            if (persisted.error().effects == Effects::None) {
                std::filesystem::remove_all(task_directory, error);
            }
            return Result<Manifest, Error>(unexpect, std::move(persisted.error()));
        }
        for (const auto& ancestor : ancestors) {
            const int parent =
                ::open(ancestor.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            const bool durable = parent >= 0 && ::fsync(parent) == 0;
            if (parent >= 0) {
                ::close(parent);
            }
            if (!durable) {
                return Result<Manifest, Error>(unexpect, std::move(uncertain));
            }
        }
        return Result<Manifest, Error>(std::move(result));
    } catch (const std::bad_alloc&) {
        if (created) {
            try {
                std::error_code ignored;
                std::filesystem::remove_all(task_directory, ignored);
            } catch (...) {
                // Preserve allocation failure if orphan cleanup also fails.
            }
        }
        return Result<Manifest, Error>(
            unexpect, Error{ErrorCode::ResourceExhausted, Effects::None, {}});
    }
}

Result<Manifest, Error> Manifest::load(const std::filesystem::path& task_directory) {
    const auto path = task_directory / "manifest.json";
    std::error_code filesystem_error;
    const auto status = std::filesystem::symlink_status(path, filesystem_error);
    if (filesystem_error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        return Result<Manifest, Error>(
            unexpect, storage_error("manifest is not a safe regular file"));
    }

    std::ifstream stream(path);
    if (!stream) {
        return Result<Manifest, Error>(unexpect, storage_error("cannot open manifest"));
    }

    std::ostringstream content;
    content << stream.rdbuf();
    if (stream.bad()) {
        return Result<Manifest, Error>(unexpect, storage_error("cannot read manifest"));
    }

    auto data = deserialize(content.str());
    if (!data) {
        return Result<Manifest, Error>(unexpect, data.error());
    }
    return Result<Manifest, Error>(Manifest(task_directory, std::move(data.value())));
}

Result<void, Error> Manifest::commit(ManifestData data) {
    if (poisoned_ || data_.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
        poisoned_ = true;
        return Result<void, Error>(unexpect, storage_error("invalid manifest revision transition"));
    }
    if (data.manifest_revision != data_.manifest_revision + 1) {
        poisoned_ = true;
        return Result<void, Error>(unexpect, storage_error("invalid manifest revision transition"));
    }

    const auto serialized = serialize(data);
    const auto persisted = write_durable(task_directory_ / "manifest.json", serialized);
    if (!persisted) {
        poisoned_ = persisted.error().effects == Effects::Possible;
        return persisted;
    }
    data_ = std::move(data);
    return Result<void, Error>();
}

}  // namespace ros2_bag::recorder
