// Read-only manifest inspection used before any recovery or cleanup decision.
#include "ros2_bag/recovery.hpp"

#include "ros2_bag/manifest.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#include <nlohmann/json.hpp>
#include <rosbag2_cpp/reader.hpp>
#include <rosbag2_storage/storage_options.hpp>

namespace ros2_bag {
namespace {

bool is_terminal_topic_event(const std::string& event) {
    return event == "TOPIC_ADDED" || event == "TOPIC_REMOVED" || event == "TOPIC_ADD_FAILED" ||
           event == "TOPIC_REMOVE_FAILED" || event == "TOPIC_ADD_UNCERTAIN" ||
           event == "TOPIC_REMOVE_UNCERTAIN";
}

bool is_safe_segment_name(const std::string& path) {
    const std::filesystem::path candidate(path);
    return !path.empty() && !candidate.is_absolute() && candidate.filename() == candidate &&
           path != "." && path != "..";
}

constexpr std::uint8_t kMcapSchemaOpcode = 0x03;
constexpr std::uint8_t kMcapChannelOpcode = 0x04;

bool read_exact(std::ifstream* input, char* data, std::size_t size) {
    input->read(data, static_cast<std::streamsize>(size));
    return input->gcount() == static_cast<std::streamsize>(size);
}

bool read_unsigned(std::ifstream* input, std::size_t size, std::uint64_t* value) {
    std::array<unsigned char, sizeof(std::uint64_t)> bytes{};
    if (size > bytes.size() || !read_exact(input, reinterpret_cast<char*>(bytes.data()), size)) {
        return false;
    }
    *value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        *value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return true;
}

bool skip_bytes(std::ifstream* input, std::uint64_t count) {
    const std::streamoff offset = static_cast<std::streamoff>(count);
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    input->seekg(offset, std::ios::cur);
    return static_cast<bool>(*input);
}

bool scan_mcap_file(
    const std::filesystem::path& path,
    std::set<std::uint16_t>* schema_ids,
    std::vector<std::uint16_t>* channel_schema_ids,
    std::string* error) {
    std::ifstream input(path, std::ios::binary);
    const std::array<char, 8> magic{{static_cast<char>(0x89), 'M', 'C', 'A', 'P', '0', '\r', '\n'}};
    std::array<char, 8> header{};
    if (!input || !read_exact(&input, header.data(), header.size()) || header != magic) {
        *error = "invalid MCAP magic";
        return false;
    }
    for (;;) {
        char opcode_character = 0;
        input.get(opcode_character);
        if (input.eof()) {
            return true;
        }
        if (!input) {
            *error = "cannot read MCAP record opcode";
            return false;
        }
        const std::uint8_t opcode = static_cast<std::uint8_t>(opcode_character);
        if (opcode == static_cast<std::uint8_t>(0x89)) {
            std::array<char, 7> trailer{};
            if (read_exact(&input, trailer.data(), trailer.size()) &&
                std::equal(trailer.begin(), trailer.end(), magic.begin() + 1)) {
                return true;
            }
            *error = "invalid MCAP trailer";
            return false;
        }
        std::uint64_t length = 0;
        if (!read_unsigned(&input, sizeof(std::uint64_t), &length)) {
            *error = "truncated MCAP record length";
            return false;
        }
        if (opcode == kMcapSchemaOpcode) {
            std::uint64_t schema_id = 0;
            if (length < 2 || !read_unsigned(&input, 2, &schema_id)) {
                *error = "truncated MCAP Schema record";
                return false;
            }
            schema_ids->insert(static_cast<std::uint16_t>(schema_id));
            if (!skip_bytes(&input, length - 2)) {
                *error = "truncated MCAP Schema payload";
                return false;
            }
        } else if (opcode == kMcapChannelOpcode) {
            std::uint64_t channel_id = 0;
            std::uint64_t schema_id = 0;
            if (length < 4 || !read_unsigned(&input, 2, &channel_id) ||
                !read_unsigned(&input, 2, &schema_id)) {
                *error = "truncated MCAP Channel record";
                return false;
            }
            static_cast<void>(channel_id);
            channel_schema_ids->push_back(static_cast<std::uint16_t>(schema_id));
            if (!skip_bytes(&input, length - 4)) {
                *error = "truncated MCAP Channel payload";
                return false;
            }
        } else if (!skip_bytes(&input, length)) {
            *error = "truncated MCAP record payload";
            return false;
        }
    }
}

void inspect_raw_mcap_records(
    const std::filesystem::path& segment_path,
    RecoverySegmentEvidence* evidence) {
    std::set<std::uint16_t> schema_ids;
    std::vector<std::uint16_t> channel_schema_ids;
    std::error_code filesystem_error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(segment_path, filesystem_error)) {
        if (filesystem_error) {
            evidence->raw_mcap_error = filesystem_error.message();
            return;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".mcap") {
            continue;
        }
        std::string error;
        if (!scan_mcap_file(entry.path(), &schema_ids, &channel_schema_ids, &error)) {
            evidence->raw_mcap_error = "raw MCAP scan: " + error;
            return;
        }
    }
    evidence->raw_schema_count = schema_ids.size();
    evidence->raw_channel_count = channel_schema_ids.size();
    evidence->raw_channel_schema_links_valid = !channel_schema_ids.empty();
    for (const std::uint16_t schema_id : channel_schema_ids) {
        if (schema_id != 0 && schema_ids.count(schema_id) == 0U) {
            evidence->raw_channel_schema_links_valid = false;
            return;
        }
    }
}

RecoverySegmentEvidence inspect_segment(
    const std::filesystem::path& task_directory,
    const std::string& path) {
    RecoverySegmentEvidence evidence;
    evidence.path = path;
    if (!is_safe_segment_name(path)) {
        evidence.error = "manifest segment path is unsafe";
        return evidence;
    }
    try {
        inspect_raw_mcap_records(task_directory / path, &evidence);
        rosbag2_storage::StorageOptions options;
        options.uri = (task_directory / path).string();
        options.storage_id = "mcap";
        rosbag2_cpp::Reader reader;
        reader.open(options);
        evidence.reader_opened = true;
        evidence.topic_metadata_count = reader.get_all_topics_and_types().size();
        while (reader.has_next()) {
            static_cast<void>(reader.read_next());
            ++evidence.message_count;
        }
    } catch (const std::exception& exception) {
        evidence.error = exception.what();
    }
    return evidence;
}

bool write_recovery_report(const RecoveryFinding& finding, std::string* error) {
    nlohmann::json report;
    report["schema_version"] = 1;
    report["recording_id"] = finding.recording_id;
    report["reason"] = "UNEXPECTED_TERMINATION";
    report["incomplete_segments"] = finding.incomplete_segments;
    report["uncertain_operations"] = finding.uncertain_operations;
    report["segment_evidence"] = nlohmann::json::array();
    for (const RecoverySegmentEvidence& evidence : finding.segment_evidence) {
        report["segment_evidence"].push_back(
            {{"path", evidence.path},
             {"reader_opened", evidence.reader_opened},
             {"topic_metadata_count", evidence.topic_metadata_count},
             {"message_count", evidence.message_count},
             {"raw_schema_count", evidence.raw_schema_count},
             {"raw_channel_count", evidence.raw_channel_count},
             {"raw_channel_schema_links_valid", evidence.raw_channel_schema_links_valid},
             {"raw_mcap_error",
              evidence.raw_mcap_error.empty() ? nlohmann::json(nullptr)
                                              : nlohmann::json(evidence.raw_mcap_error)},
             {"error",
              evidence.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(evidence.error)}});
    }
    const std::filesystem::path path = finding.task_directory / "recovery.json";
    const std::filesystem::path temporary = path.string() + ".tmp";
    const std::string bytes = report.dump(2) + "\n";
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        *error = std::strerror(errno);
        return false;
    }
    const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
    const int sync_result = ::fsync(descriptor);
    const int close_result = ::close(descriptor);
    if (written != static_cast<ssize_t>(bytes.size()) || sync_result != 0 || close_result != 0) {
        *error = std::strerror(errno);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        *error = filesystem_error.message();
        return false;
    }
    const int directory = ::open(finding.task_directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) {
        const int directory_sync_result = ::fsync(directory);
        ::close(directory);
        if (directory_sync_result != 0) {
            *error = std::strerror(errno);
            return false;
        }
    }
    return true;
}

bool append_uncertain_events(
    const std::filesystem::path& event_path,
    const std::string& recording_id,
    std::uint64_t next_sequence,
    const std::vector<nlohmann::json>& prepared_events,
    std::string* error) {
    if (prepared_events.empty()) {
        return true;
    }
    const int descriptor = ::open(event_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (descriptor < 0) {
        *error = std::strerror(errno);
        return false;
    }
    for (const nlohmann::json& prepared : prepared_events) {
        nlohmann::json uncertain;
        const bool add = prepared.value("event", "") == "TOPIC_ADD_PREPARED";
        uncertain["schema_version"] = 1;
        uncertain["event"] = add ? "TOPIC_ADD_UNCERTAIN" : "TOPIC_REMOVE_UNCERTAIN";
        uncertain["event_seq"] = next_sequence++;
        uncertain["recording_id"] = recording_id;
        uncertain["operation_id"] = prepared.value("operation_id", "");
        uncertain["topic_id"] = prepared.value("topic_id", 0ULL);
        uncertain["topic_epoch"] = prepared.value("topic_epoch", 0ULL);
        uncertain["name"] = prepared.value("name", "");
        uncertain["type"] = prepared.value("type", "");
        uncertain["error_code"] = "UNEXPECTED_TERMINATION";
        uncertain["error_message"] = "operation has PREPARED but no durable terminal event";
        uncertain["details"] = {{"recovery", true}};
        uncertain["at_ns"] =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        const std::string line = uncertain.dump() + "\n";
        const ssize_t written = ::write(descriptor, line.data(), line.size());
        if (written != static_cast<ssize_t>(line.size())) {
            *error = std::strerror(errno);
            ::close(descriptor);
            return false;
        }
    }
    const int sync_result = ::fsync(descriptor);
    const int close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        *error = std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace

std::vector<RecoveryFinding> inspect_recovery_candidates(const std::filesystem::path& output_root) {
    std::vector<RecoveryFinding> findings;
    std::error_code filesystem_error;
    if (!std::filesystem::exists(output_root, filesystem_error)) return findings;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(output_root, filesystem_error)) {
        if (filesystem_error || !entry.is_directory()) break;
        const std::filesystem::path manifest_path = entry.path() / "manifest.json";
        if (!std::filesystem::exists(manifest_path, filesystem_error)) continue;
        RecoveryFinding finding;
        finding.task_directory = entry.path();
        try {
            std::ifstream input(manifest_path);
            nlohmann::json manifest;
            input >> manifest;
            if (manifest.value("schema_version", 0) != 1) {
                finding.error = "unsupported manifest schema_version";
            } else {
                finding.recording_id = manifest.value("recording_id", "");
                finding.recording_state = manifest.value("recording_state", "");
                for (const nlohmann::json& segment : manifest.at("segments")) {
                    const std::string state = segment.value("state", "");
                    if (state != "COMPLETE" && state != "REMOVED") {
                        finding.incomplete_segments.push_back(segment.value("path", ""));
                    }
                }
                if (finding.recording_state == "COMPLETED" && finding.incomplete_segments.empty())
                    continue;
            }
        } catch (const std::exception& exception) {
            finding.error = exception.what();
        }
        findings.push_back(std::move(finding));
    }
    return findings;
}

std::vector<RecoveryFinding> recover_deleting_segments(const std::filesystem::path& output_root) {
    std::vector<RecoveryFinding> failures;
    std::error_code filesystem_error;
    if (!std::filesystem::exists(output_root, filesystem_error)) {
        return failures;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(output_root, filesystem_error)) {
        if (filesystem_error || !entry.is_directory()) {
            break;
        }
        const std::filesystem::path manifest_path = entry.path() / "manifest.json";
        if (!std::filesystem::exists(manifest_path, filesystem_error)) {
            continue;
        }
        RecoveryFinding failure;
        failure.task_directory = entry.path();
        try {
            std::ifstream input(manifest_path);
            nlohmann::json document;
            input >> document;
            RecordingManifest manifest;
            manifest.session_id = document.value("session_id", "default");
            manifest.recording_id = document.value("recording_id", "");
            manifest.mode = document.value("mode", "");
            manifest.recording_state = document.value("recording_state", "");
            if (!document.at("last_error").is_null()) {
                manifest.last_error = document.at("last_error").get<std::string>();
            }
            manifest.secondary_errors =
                document.value("secondary_errors", std::vector<std::string>{});
            manifest.cleanup_pending =
                document.value("cleanup_pending", std::vector<std::string>{});
            for (const nlohmann::json& item : document.at("segments")) {
                ManifestSegment segment;
                segment.segment_id = item.value("segment_id", 0ULL);
                segment.path = item.value("path", "");
                segment.state = item.value("state", "");
                segment.completed_at_ns = item.value("completed_at_ns", 0ULL);
                if (segment.state == "DELETING") {
                    if (!is_safe_segment_name(segment.path)) {
                        failure.error = "manifest segment path is unsafe";
                        failure.incomplete_segments.push_back(segment.path);
                        manifest.segments.push_back(std::move(segment));
                        continue;
                    }
                    std::error_code remove_error;
                    std::filesystem::remove_all(entry.path() / segment.path, remove_error);
                    if (remove_error) {
                        failure.error = remove_error.message();
                        failure.incomplete_segments.push_back(segment.path);
                    } else {
                        segment.state = "REMOVED";
                        manifest.cleanup_pending.erase(
                            std::remove(
                                manifest.cleanup_pending.begin(),
                                manifest.cleanup_pending.end(),
                                segment.path),
                            manifest.cleanup_pending.end());
                    }
                }
                manifest.segments.push_back(std::move(segment));
            }
            std::string error;
            if (failure.error.empty() && !write_manifest_atomic(manifest_path, manifest, &error)) {
                failure.error = error;
            }
            if (!failure.error.empty()) {
                failure.recording_id = manifest.recording_id;
                failure.recording_state = manifest.recording_state;
                failures.push_back(std::move(failure));
            }
        } catch (const std::exception& exception) {
            failure.error = exception.what();
            failures.push_back(std::move(failure));
        }
    }
    return failures;
}

std::vector<RecoveryFinding> recover_incomplete_recordings(
    const std::filesystem::path& output_root) {
    std::vector<RecoveryFinding> failures;
    std::error_code filesystem_error;
    if (!std::filesystem::exists(output_root, filesystem_error)) {
        return failures;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(output_root, filesystem_error)) {
        if (filesystem_error || !entry.is_directory()) {
            break;
        }
        const std::filesystem::path manifest_path = entry.path() / "manifest.json";
        if (!std::filesystem::exists(manifest_path, filesystem_error)) {
            continue;
        }
        RecoveryFinding finding;
        finding.task_directory = entry.path();
        try {
            std::ifstream input(manifest_path);
            nlohmann::json document;
            input >> document;
            if (document.value("schema_version", 0) != 1) {
                finding.error = "unsupported manifest schema_version";
                failures.push_back(std::move(finding));
                continue;
            }
            RecordingManifest manifest;
            manifest.session_id = document.value("session_id", "default");
            manifest.recording_id = document.value("recording_id", "");
            manifest.mode = document.value("mode", "");
            manifest.recording_state = document.value("recording_state", "");
            if (!document.at("last_error").is_null()) {
                manifest.last_error = document.at("last_error").get<std::string>();
            }
            manifest.secondary_errors =
                document.value("secondary_errors", std::vector<std::string>{});
            manifest.cleanup_pending =
                document.value("cleanup_pending", std::vector<std::string>{});
            finding.recording_id = manifest.recording_id;
            finding.recording_state = manifest.recording_state;
            for (const nlohmann::json& item : document.at("segments")) {
                ManifestSegment segment;
                segment.segment_id = item.value("segment_id", 0ULL);
                segment.path = item.value("path", "");
                segment.state = item.value("state", "");
                segment.completed_at_ns = item.value("completed_at_ns", 0ULL);
                if (segment.state == "WRITING" || segment.state == "FINALIZING") {
                    segment.state = "INCOMPLETE";
                    finding.incomplete_segments.push_back(segment.path);
                }
                manifest.segments.push_back(std::move(segment));
            }
            std::map<std::string, nlohmann::json> prepared_operations;
            std::uint64_t max_event_sequence = 0;
            const std::filesystem::path event_path = entry.path() / "topic_events.jsonl";
            std::ifstream events(event_path);
            for (std::string line; std::getline(events, line);) {
                const nlohmann::json event = nlohmann::json::parse(line);
                const std::string operation_id = event.value("operation_id", "");
                if (operation_id.empty()) {
                    continue;
                }
                max_event_sequence =
                    std::max(max_event_sequence, event.value<std::uint64_t>("event_seq", 0));
                const std::string event_name = event.value("event", "");
                if (event_name == "TOPIC_ADD_PREPARED" || event_name == "TOPIC_REMOVE_PREPARED") {
                    prepared_operations[operation_id] = event;
                } else if (is_terminal_topic_event(event_name)) {
                    prepared_operations.erase(operation_id);
                }
            }
            std::vector<nlohmann::json> uncertain_events;
            for (const auto& item : prepared_operations) {
                finding.uncertain_operations.push_back(item.first);
                uncertain_events.push_back(item.second);
            }
            for (const std::string& path : finding.incomplete_segments) {
                finding.segment_evidence.push_back(inspect_segment(entry.path(), path));
            }
            if (finding.incomplete_segments.empty() && finding.uncertain_operations.empty()) {
                continue;
            }
            manifest.recording_state = "FAILED";
            manifest.last_error = "UNEXPECTED_TERMINATION";
            std::string error;
            if (!append_uncertain_events(
                    event_path,
                    manifest.recording_id,
                    max_event_sequence + 1,
                    uncertain_events,
                    &error) ||
                !write_manifest_atomic(manifest_path, manifest, &error) ||
                !write_recovery_report(finding, &error)) {
                finding.error = error;
                failures.push_back(std::move(finding));
            }
        } catch (const std::exception& exception) {
            finding.error = exception.what();
            failures.push_back(std::move(finding));
        }
    }
    return failures;
}

}  // namespace ros2_bag
