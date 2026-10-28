#include "ros2_bag/recorder/topic_events.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>

namespace ros2_bag::recorder {
namespace {

using nlohmann::json;

Error storage_error(std::string message, Effects effects = Effects::None) {
    return {ErrorCode::StorageError, effects, std::move(message)};
}

json encode_error(const Error& error) {
    return {{"code", static_cast<int>(error.code)}, {"effects", static_cast<int>(error.effects)}, {"message", error.message}};
}

Error decode_error(const json& value) {
    return {static_cast<ErrorCode>(value.at("code").get<int>()),
            static_cast<Effects>(value.at("effects").get<int>()), value.at("message").get<std::string>()};
}

json encode_event(const TopicEvent& event) {
    json value{{"schema_version", 1},
               {"event_seq", event.event_seq},
               {"recording_id", event.recording_id},
               {"generation", event.generation},
               {"origin", "external"},
               {"operation_id", event.operation_id},
               {"topic_id", event.topic_id},
               {"topic_epoch", event.topic_epoch},
               {"type", static_cast<int>(event.type)},
               {"timestamp_ns", event.timestamp_ns}};
    value["error"] = event.error ? encode_error(*event.error) : json(nullptr);
    return value;
}

TopicEvent decode_event(const json& value) {
    if (value.at("schema_version").get<int>() != 1) throw std::runtime_error("unsupported event schema");
    TopicEvent event;
    event.event_seq = value.at("event_seq").get<std::uint64_t>();
    event.recording_id = value.at("recording_id").get<std::string>();
    event.generation = value.at("generation").get<std::uint64_t>();
    event.operation_id = value.at("operation_id").get<std::string>();
    event.topic_id = value.at("topic_id").get<TopicId>();
    event.topic_epoch = value.at("topic_epoch").get<std::uint64_t>();
    event.type = static_cast<TopicEventType>(value.at("type").get<int>());
    event.timestamp_ns = value.at("timestamp_ns").get<std::int64_t>();
    if (!value.at("error").is_null()) event.error = decode_error(value.at("error"));
    return event;
}

bool is_prepared(TopicEventType type) {
    return type == TopicEventType::AddPrepared || type == TopicEventType::RemovePrepared;
}

bool is_success(TopicEventType type) {
    return type == TopicEventType::TopicAdded || type == TopicEventType::TopicRemoved;
}

bool terminal_matches(TopicEventType prepared, TopicEventType terminal) {
    return (prepared == TopicEventType::AddPrepared &&
            (terminal == TopicEventType::TopicAdded || terminal == TopicEventType::AddFailed)) ||
           (prepared == TopicEventType::RemovePrepared &&
            (terminal == TopicEventType::TopicRemoved || terminal == TopicEventType::RemoveFailed));
}

Result<std::vector<TopicEvent>, Error> read_events(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return Result<std::vector<TopicEvent>, Error>(unexpect, storage_error("cannot open topic event file"));
    std::vector<TopicEvent> events;
    std::string line;
    std::uint64_t expected = 1;
    try {
        while (std::getline(input, line)) {
            if (line.empty()) throw std::runtime_error("empty event line");
            const auto event = decode_event(json::parse(line));
            if (event.event_seq != expected || expected == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("non-continuous event sequence");
            }
            ++expected;
            events.push_back(event);
        }
        if (!input.eof()) throw std::runtime_error("unreadable event file");
    } catch (const std::exception& exception) {
        return Result<std::vector<TopicEvent>, Error>(unexpect, storage_error(exception.what()));
    }
    return Result<std::vector<TopicEvent>, Error>(std::move(events));
}

}  // namespace

Result<TopicEvents, Error> TopicEvents::open(
    const std::filesystem::path& task_directory, std::string recording_id, std::uint64_t generation) {
    const auto path = task_directory / "topic_events.jsonl";
    if (!std::filesystem::exists(path)) {
        return Result<TopicEvents, Error>(TopicEvents(path, std::move(recording_id), generation, 1));
    }
    const auto events = read_events(path);
    if (!events) return Result<TopicEvents, Error>(unexpect, events.error());
    for (const auto& event : events.value()) {
        if (event.recording_id != recording_id || event.generation != generation) {
            return Result<TopicEvents, Error>(unexpect, storage_error("topic event identity mismatch"));
        }
    }
    const auto next = events->empty() ? 1 : events->back().event_seq + 1;
    if (next == 0) return Result<TopicEvents, Error>(unexpect, storage_error("topic event sequence exhausted"));
    return Result<TopicEvents, Error>(TopicEvents(path, std::move(recording_id), generation, next));
}

Result<std::uint64_t, Error> TopicEvents::append(TopicEvent event) {
    if (uncertainty_watermark_) {
        return Result<std::uint64_t, Error>(
            unexpect, Error{ErrorCode::EventCommitFailed, Effects::Possible, "topic event chain is uncertain"});
    }
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<std::uint64_t, Error>(unexpect, storage_error("topic event sequence exhausted"));
    }
    event.event_seq = next_sequence_;
    event.recording_id = recording_id_;
    event.generation = generation_;
    const std::string line = encode_event(event).dump() + "\n";
    const int file = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (file < 0) return Result<std::uint64_t, Error>(unexpect, storage_error(std::strerror(errno)));
    const auto written = ::write(file, line.data(), line.size());
    const bool durable = written == static_cast<ssize_t>(line.size()) && ::fsync(file) == 0;
    const int saved_errno = errno;
    ::close(file);
    if (!durable) {
        uncertainty_watermark_ = event.event_seq;
        return Result<std::uint64_t, Error>(
            unexpect, Error{ErrorCode::EventCommitFailed, Effects::Possible, std::strerror(saved_errno)});
    }
    ++next_sequence_;
    return Result<std::uint64_t, Error>(event.event_seq);
}

Result<TopicOperationState, Error> TopicEvents::operation_state(const OperationId& operation_id) const {
    const auto events = read_events(path_);
    if (!events) return Result<TopicOperationState, Error>(unexpect, events.error());
    std::optional<TopicEvent> prepared;
    for (const auto& event : events.value()) {
        if (event.operation_id != operation_id) continue;
        if (uncertainty_watermark_ && event.event_seq >= *uncertainty_watermark_) {
            return Result<TopicOperationState, Error>(TopicOperationState::Uncertain);
        }
        if (is_prepared(event.type)) {
            if (prepared) return Result<TopicOperationState, Error>(unexpect, storage_error("duplicate prepared event"));
            prepared = event;
            continue;
        }
        if (!prepared || !terminal_matches(prepared->type, event.type) || event.topic_id != prepared->topic_id ||
            event.topic_epoch != prepared->topic_epoch) {
            return Result<TopicOperationState, Error>(unexpect, storage_error("invalid topic event chain"));
        }
        return Result<TopicOperationState, Error>(
            is_success(event.type) ? TopicOperationState::ConfirmedSuccess : TopicOperationState::ConfirmedFailure);
    }
    return Result<TopicOperationState, Error>(prepared ? TopicOperationState::Uncertain : TopicOperationState::NotFound);
}

}  // namespace ros2_bag::recorder
