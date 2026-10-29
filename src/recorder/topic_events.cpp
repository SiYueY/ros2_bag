#include "ros2_bag/recorder/topic_events.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace ros2_bag::recorder {
namespace {

Error storage_error(std::string message, Effects effects = Effects::None) {
    return {ErrorCode::StorageError, effects, std::move(message)};
}

bool write_all(const int file, const char* bytes, std::size_t remaining) {
    while (remaining != 0) {
        const auto written = ::write(file, bytes, remaining);
        if (written > 0) {
            bytes += written;
            remaining -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

TopicEvents::TopicEvents(
    std::filesystem::path path, std::string recording_id, std::uint64_t generation,
    std::uint64_t next_sequence)
: path_(std::move(path)),
  recording_id_(std::move(recording_id)),
  generation_(generation),
  next_sequence_(next_sequence) {}

std::uint64_t TopicEvents::uncertainty_watermark() const noexcept { return uncertainty_watermark_; }

bool TopicEvents::terminal_matches(TopicEventType prepared, TopicEventType terminal) noexcept {
    if (prepared == TopicEventType::AddPrepared) {
        return terminal == TopicEventType::TopicAdded || terminal == TopicEventType::AddFailed;
    }
    if (prepared == TopicEventType::RemovePrepared) {
        return terminal == TopicEventType::TopicRemoved || terminal == TopicEventType::RemoveFailed;
    }
    return false;
}

Result<std::vector<TopicEvent>, Error> TopicEvents::read_events(const std::filesystem::path& path) {
    std::error_code path_error;
    const auto status = std::filesystem::symlink_status(path, path_error);
    if (path_error || !std::filesystem::is_regular_file(status)) {
        return Result<std::vector<TopicEvent>, Error>(
            unexpect, storage_error("topic events is not a safe regular file"));
    }
    std::ifstream input(path);
    if (!input) {
        return Result<std::vector<TopicEvent>, Error>(
            unexpect, storage_error("cannot open topic event file"));
    }

    std::vector<TopicEvent> events;
    std::string line;
    std::uint64_t expected = 1;
    try {
        while (std::getline(input, line)) {
            if (line.empty()) {
                throw std::runtime_error("empty event line");
            }

            const auto value = nlohmann::json::parse(line);
            if (value.at("schema_version").get<int>() != 1) {
                throw std::runtime_error("unsupported event schema");
            }

            TopicEvent event;
            event.event_seq = value.at("event_seq").get<std::uint64_t>();
            event.recording_id = value.at("recording_id").get<std::string>();
            event.generation = value.at("generation").get<std::uint64_t>();
            event.operation_id = value.at("operation_id").get<std::string>();
            event.topic_id = value.at("topic_id").get<TopicId>();
            event.topic_epoch = value.at("topic_epoch").get<std::uint64_t>();
            event.type = static_cast<TopicEventType>(value.at("type").get<int>());
            event.timestamp_ns = value.at("timestamp_ns").get<std::int64_t>();
            if (!value.at("error").is_null()) {
                const auto& error = value.at("error");
                event.error = Error{
                    static_cast<ErrorCode>(error.at("code").get<int>()),
                    static_cast<Effects>(error.at("effects").get<int>()),
                    error.at("message").get<std::string>()};
            }

            if (event.event_seq != expected ||
                expected == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("non-continuous event sequence");
            }
            ++expected;
            events.push_back(std::move(event));
        }
        if (!input.eof()) {
            throw std::runtime_error("unreadable event file");
        }
    } catch (const std::exception& exception) {
        return Result<std::vector<TopicEvent>, Error>(unexpect, storage_error(exception.what()));
    }

    return Result<std::vector<TopicEvent>, Error>(std::move(events));
}

Result<void, Error> TopicEvents::validate_chain(const std::vector<TopicEvent>& events) {
    struct Operation {
        TopicEventType prepared;
        TopicId topic_id;
        std::uint64_t topic_epoch;
        bool terminal = false;
    };

    std::unordered_map<OperationId, Operation> operations;
    for (const auto& event : events) {
        if (event.operation_id.empty() || event.topic_id == 0 || event.topic_epoch == 0) {
            return Result<void, Error>(unexpect, storage_error("invalid topic event identity"));
        }

        const bool prepared = event.type == TopicEventType::AddPrepared ||
                              event.type == TopicEventType::RemovePrepared;
        if (prepared) {
            if (operations.find(event.operation_id) != operations.end()) {
                return Result<void, Error>(
                    unexpect, storage_error("duplicate topic operation prepared event"));
            }
            operations.emplace(
                event.operation_id,
                Operation{event.type, event.topic_id, event.topic_epoch, false});
            continue;
        }

        const auto operation = operations.find(event.operation_id);
        if (operation == operations.end()) {
            return Result<void, Error>(unexpect, storage_error("missing prepared topic event"));
        }
        if (operation->second.terminal) {
            return Result<void, Error>(unexpect, storage_error("duplicate terminal topic event"));
        }
        if (!terminal_matches(operation->second.prepared, event.type)) {
            return Result<void, Error>(
                unexpect, storage_error("mismatched topic event transition"));
        }
        if (operation->second.topic_id != event.topic_id) {
            return Result<void, Error>(unexpect, storage_error("topic event id changed"));
        }
        if (operation->second.topic_epoch != event.topic_epoch) {
            return Result<void, Error>(unexpect, storage_error("topic event epoch changed"));
        }
        operation->second.terminal = true;
    }

    return Result<void, Error>();
}

Result<TopicEvents, Error> TopicEvents::create(
    const std::filesystem::path& task_directory, std::string recording_id,
    std::uint64_t generation) {
    const auto path = task_directory / "topic_events.jsonl";
    const auto status = std::filesystem::symlink_status(task_directory);
    if (!std::filesystem::is_directory(status)) {
        return Result<TopicEvents, Error>(unexpect, storage_error("unsafe task directory"));
    }
    const int file = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644);
    if (file < 0) {
        return Result<TopicEvents, Error>(unexpect, storage_error("cannot open topic events"));
    }
    struct stat metadata {};
    const bool file_durable =
        ::fstat(file, &metadata) == 0 && S_ISREG(metadata.st_mode) && ::fsync(file) == 0;
    ::close(file);
    const int directory =
        ::open(task_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    const bool durable = file_durable && directory >= 0 && ::fsync(directory) == 0;
    if (directory >= 0) {
        ::close(directory);
    }
    if (!durable) {
        return Result<TopicEvents, Error>(
            unexpect, storage_error("topic event file durability is uncertain", Effects::Possible));
    }

    return Result<TopicEvents, Error>(TopicEvents(path, std::move(recording_id), generation, 1));
}

Result<TopicEvents, Error> TopicEvents::open(
    const std::filesystem::path& task_directory, std::string recording_id,
    std::uint64_t generation) {
    const auto path = task_directory / "topic_events.jsonl";
    const auto status = std::filesystem::symlink_status(task_directory);
    if (!std::filesystem::is_directory(status)) {
        return Result<TopicEvents, Error>(unexpect, storage_error("unsafe task directory"));
    }

    const auto events = read_events(path);
    if (!events) {
        return Result<TopicEvents, Error>(unexpect, events.error());
    }
    for (const auto& event : events.value()) {
        if (event.recording_id != recording_id || event.generation != generation) {
            return Result<TopicEvents, Error>(
                unexpect, storage_error("topic event identity mismatch"));
        }
    }

    const auto chain = validate_chain(events.value());
    if (!chain) {
        return Result<TopicEvents, Error>(unexpect, chain.error());
    }

    std::uint64_t next = 1;
    if (!events->empty()) {
        next = events->back().event_seq + 1;
    }
    if (next == 0) {
        return Result<TopicEvents, Error>(
            unexpect, storage_error("topic event sequence exhausted"));
    }

    return Result<TopicEvents, Error>(TopicEvents(path, std::move(recording_id), generation, next));
}

Result<std::uint64_t, Error> TopicEvents::append(TopicEvent event) {
    if (uncertainty_watermark_ != 0) {
        return Result<std::uint64_t, Error>(
            unexpect,
            Error{
                ErrorCode::EventCommitFailed, Effects::Possible, "topic event chain is uncertain"});
    }
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<std::uint64_t, Error>(
            unexpect, storage_error("topic event sequence exhausted"));
    }

    event.event_seq = next_sequence_;
    event.recording_id = recording_id_;
    event.generation = generation_;

    nlohmann::json value{
        {"schema_version", 1},
        {"event_seq", event.event_seq},
        {"recording_id", event.recording_id},
        {"generation", event.generation},
        {"origin", "external"},
        {"operation_id", event.operation_id},
        {"topic_id", event.topic_id},
        {"topic_epoch", event.topic_epoch},
        {"type", static_cast<int>(event.type)},
        {"timestamp_ns", event.timestamp_ns},
        {"error", nullptr}};
    if (event.error) {
        value["error"] = {
            {"code", static_cast<int>(event.error->code)},
            {"effects", static_cast<int>(event.error->effects)},
            {"message", event.error->message}};
    }

    const std::string line = value.dump() + "\n";
    const int file =
        ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644);
    if (file < 0) {
        return Result<std::uint64_t, Error>(unexpect, storage_error(std::strerror(errno)));
    }

    struct stat metadata {};
    const bool durable = ::fstat(file, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
                         write_all(file, line.data(), line.size()) && ::fsync(file) == 0;
    const int saved_errno = errno;
    ::close(file);
    if (!durable) {
        uncertainty_watermark_ = event.event_seq;
        return Result<std::uint64_t, Error>(
            unexpect,
            Error{ErrorCode::EventCommitFailed, Effects::Possible, std::strerror(saved_errno)});
    }

    ++next_sequence_;
    return Result<std::uint64_t, Error>(event.event_seq);
}

Result<TopicOperationState, Error> TopicEvents::operation_state(
    const OperationId& operation_id) const {
    if (!std::filesystem::exists(path_)) {
        return Result<TopicOperationState, Error>(
            unexpect, storage_error("topic event file is missing"));
    }

    const auto events = read_events(path_);
    if (!events) {
        return Result<TopicOperationState, Error>(unexpect, events.error());
    }

    std::optional<TopicEvent> prepared;
    for (const auto& event : events.value()) {
        if (event.operation_id != operation_id) {
            continue;
        }
        if (uncertainty_watermark_ != 0 && event.event_seq >= uncertainty_watermark_) {
            return Result<TopicOperationState, Error>(TopicOperationState::Uncertain);
        }

        const bool prepared_event = event.type == TopicEventType::AddPrepared ||
                                    event.type == TopicEventType::RemovePrepared;
        if (prepared_event) {
            if (prepared) {
                return Result<TopicOperationState, Error>(
                    unexpect, storage_error("duplicate prepared event"));
            }
            prepared = event;
            continue;
        }

        if (!prepared) {
            return Result<TopicOperationState, Error>(
                unexpect, storage_error("invalid topic event chain"));
        }
        if (!terminal_matches(prepared->type, event.type)) {
            return Result<TopicOperationState, Error>(
                unexpect, storage_error("invalid topic event chain"));
        }
        if (event.topic_id != prepared->topic_id || event.topic_epoch != prepared->topic_epoch) {
            return Result<TopicOperationState, Error>(
                unexpect, storage_error("invalid topic event chain"));
        }

        if (event.type == TopicEventType::TopicAdded ||
            event.type == TopicEventType::TopicRemoved) {
            return Result<TopicOperationState, Error>(TopicOperationState::ConfirmedSuccess);
        }
        return Result<TopicOperationState, Error>(TopicOperationState::ConfirmedFailure);
    }

    if (prepared) {
        return Result<TopicOperationState, Error>(TopicOperationState::Uncertain);
    }
    return Result<TopicOperationState, Error>(TopicOperationState::NotFound);
}

}  // namespace ros2_bag::recorder
