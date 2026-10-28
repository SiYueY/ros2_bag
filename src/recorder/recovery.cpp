#include "ros2_bag/recorder/recovery.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

#include "ros2_bag/recorder/manifest.hpp"
#include "ros2_bag/recorder/topic_events.hpp"

namespace ros2_bag::recorder {
namespace {

bool terminal(TaskState state) {
    return state == TaskState::Completed || state == TaskState::Failed ||
           state == TaskState::Incomplete;
}

Error storage_error(std::string message) {
    return {ErrorCode::StorageError, Effects::None, std::move(message)};
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

Result<void, Error> write_recovery_evidence(
    const std::filesystem::path& directory,
    const std::string& recording_id,
    const std::vector<RecoveryFinding>& findings) {
    nlohmann::json document;
    document["schema_version"] = 1;
    document["recording_id"] = recording_id;
    document["findings"] = nlohmann::json::array();
    for (const auto& finding : findings) {
        document["findings"].push_back({{"code", finding.code}, {"message", finding.message}});
    }
    const auto bytes = document.dump();
    const auto path = directory / "recovery.json";
    const auto temporary = directory / "recovery.json.tmp";
    const auto file =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (file < 0) {
        return Result<void, Error>(unexpect, storage_error(std::strerror(errno)));
    }
    const auto durable = write_all(file, bytes.data(), bytes.size()) && ::fsync(file) == 0;
    const auto write_error = errno;
    if (::close(file) != 0 || !durable) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Result<void, Error>(unexpect, storage_error(std::strerror(write_error)));
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const auto rename_error = errno;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Result<void, Error>(unexpect, storage_error(std::strerror(rename_error)));
    }
    const auto directory_file = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_file < 0) {
        return Result<void, Error>(unexpect, storage_error(std::strerror(errno)));
    }
    const auto directory_durable = ::fsync(directory_file) == 0;
    const auto directory_error = errno;
    const auto closed = ::close(directory_file) == 0;
    if (!directory_durable || !closed) {
        return Result<void, Error>(unexpect, storage_error(std::strerror(directory_error)));
    }
    return Result<void, Error>();
}

bool safe_relative_path(const std::string& path) {
    const std::filesystem::path value(path);
    if (value.empty() || value.is_absolute()) {
        return false;
    }
    for (const auto& component : value) {
        if (component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

}  // namespace

Result<RecoveryReport, Error> Recovery::run(const std::filesystem::path& output_root) {
    RecoveryReport report;
    std::map<std::string, std::filesystem::path> task_directories;
    std::error_code filesystem_error;
    if (!std::filesystem::exists(output_root, filesystem_error))
        return Result<RecoveryReport, Error>(std::move(report));
    if (filesystem_error)
        return Result<RecoveryReport, Error>(unexpect, storage_error(filesystem_error.message()));
    for (const auto& entry : std::filesystem::directory_iterator(output_root, filesystem_error)) {
        if (filesystem_error)
            return Result<RecoveryReport, Error>(
                unexpect,
                storage_error(filesystem_error.message()));
        if (!entry.is_directory() || entry.is_symlink()) continue;
        const auto loaded = Manifest::load(entry.path());
        if (!loaded) {
            report.findings.push_back(
                {entry.path().filename().string(), "manifest_unreadable", loaded.error().message});
            continue;
        }
        Manifest manifest = loaded.value();
        const auto& data = manifest.data();
        if (data.task.recording_id != entry.path().filename().string()) {
            report.findings.push_back(
                {data.task.recording_id,
                 "manifest_directory_identity_mismatch",
                 "manifest recording id does not match task directory"});
            continue;
        }
        task_directories.emplace(data.task.recording_id, entry.path());
        const auto events =
            TopicEvents::open(entry.path(), data.task.recording_id, data.task.generation);
        if (!events) {
            report.findings.push_back(
                {data.task.recording_id, "topic_events_corrupt", events.error().message});
        }
        bool invalid_segment_path = false;
        ManifestData recovered = data;
        bool removed_deleting_segment = false;
        for (const auto& segment : data.segments) {
            if (!safe_relative_path(segment.relative_path)) {
                invalid_segment_path = true;
                report.findings.push_back(
                    {data.task.recording_id, "segment_path_invalid", segment.relative_path});
                continue;
            }
            if (segment.state == SegmentLifecycleState::Deleting) {
                std::error_code remove_error;
                std::filesystem::remove_all(entry.path() / segment.relative_path, remove_error);
                if (remove_error) {
                    report.findings.push_back(
                        {data.task.recording_id,
                         "deleting_segment_retry_failed",
                         remove_error.message()});
                    continue;
                }
                for (auto& candidate : recovered.segments) {
                    if (candidate.segment_id == segment.segment_id) {
                        candidate.state = SegmentLifecycleState::Removed;
                        removed_deleting_segment = true;
                        break;
                    }
                }
            }
        }
        if (removed_deleting_segment) {
            if (recovered.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
                report.findings.push_back(
                    {data.task.recording_id,
                     "deleting_segment_manifest_revision_exhausted",
                     "cannot commit removal"});
            } else {
                ++recovered.manifest_revision;
                const auto committed = manifest.commit(recovered);
                if (!committed) {
                    report.findings.push_back(
                        {data.task.recording_id,
                         "deleting_segment_commit_failed",
                         committed.error().message});
                } else {
                    report.findings.push_back(
                        {data.task.recording_id,
                         "deleting_segment_removed",
                         "durable deletion recovered"});
                }
            }
        }
        const auto& current = manifest.data();
        if (terminal(current.task.state)) continue;
        if (current.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            report.findings.push_back(
                {data.task.recording_id, "manifest_revision_exhausted", "manifest is read-only"});
            continue;
        }
        ManifestData incomplete = current;
        ++incomplete.manifest_revision;
        incomplete.task.state = TaskState::Incomplete;
        incomplete.task.error = Error{
            ErrorCode::StorageError,
            Effects::ConfirmedPartial,
            "recovery interrupted active task"};
        for (auto& segment : incomplete.segments) {
            if (segment.state == SegmentLifecycleState::Writing ||
                segment.state == SegmentLifecycleState::Finalizing) {
                segment.state = SegmentLifecycleState::Incomplete;
            }
        }
        const auto committed = manifest.commit(incomplete);
        if (!committed) {
            report.findings.push_back(
                {incomplete.task.recording_id,
                 "incomplete_commit_failed",
                 committed.error().message});
            continue;
        }
        report.findings.push_back(
            {incomplete.task.recording_id,
             "task_marked_incomplete",
             invalid_segment_path
                 ? "recovery did not resume prior recording; invalid segment path retained"
                 : "recovery did not resume prior recording"});
    }
    for (const auto& entry : task_directories) {
        std::vector<RecoveryFinding> findings;
        for (const auto& finding : report.findings) {
            if (finding.recording_id == entry.first) {
                findings.push_back(finding);
            }
        }
        if (findings.empty()) {
            continue;
        }
        const auto persisted = write_recovery_evidence(entry.second, entry.first, findings);
        if (!persisted) {
            report.findings.push_back(
                {entry.first, "recovery_evidence_write_failed", persisted.error().message});
        }
    }
    return Result<RecoveryReport, Error>(std::move(report));
}

}  // namespace ros2_bag::recorder
