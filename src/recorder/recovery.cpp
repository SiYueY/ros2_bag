#include "ros2_bag/recorder/recovery.hpp"

#include <filesystem>
#include <limits>
#include <utility>

#include "ros2_bag/recorder/manifest.hpp"
#include "ros2_bag/recorder/topic_events.hpp"

namespace ros2_bag::recorder {
namespace {

bool terminal(TaskState state) {
    return state == TaskState::Completed || state == TaskState::Failed || state == TaskState::Incomplete;
}

Error storage_error(std::string message) {
    return {ErrorCode::StorageError, Effects::None, std::move(message)};
}

bool safe_relative_path(const std::string& path) {
    const std::filesystem::path value(path);
    if (value.empty() || value.is_absolute()) return false;
    for (const auto& component : value) {
        if (component == "..") return false;
    }
    return true;
}

}  // namespace

Result<RecoveryReport, Error> Recovery::run(const std::filesystem::path& output_root) {
    RecoveryReport report;
    std::error_code filesystem_error;
    if (!std::filesystem::exists(output_root, filesystem_error)) return Result<RecoveryReport, Error>(std::move(report));
    if (filesystem_error) return Result<RecoveryReport, Error>(unexpect, storage_error(filesystem_error.message()));
    for (const auto& entry : std::filesystem::directory_iterator(output_root, filesystem_error)) {
        if (filesystem_error) return Result<RecoveryReport, Error>(unexpect, storage_error(filesystem_error.message()));
        if (!entry.is_directory()) continue;
        const auto loaded = Manifest::load(entry.path());
        if (!loaded) {
            report.findings.push_back({entry.path().filename().string(), "manifest_unreadable", loaded.error().message});
            continue;
        }
        Manifest manifest = loaded.value();
        const auto& data = manifest.data();
        if (data.task.recording_id != entry.path().filename().string()) {
            report.findings.push_back({data.task.recording_id, "manifest_directory_identity_mismatch",
                                       "manifest recording id does not match task directory"});
            continue;
        }
        const auto events = TopicEvents::open(entry.path(), data.task.recording_id, data.task.generation);
        if (!events) {
            report.findings.push_back({data.task.recording_id, "topic_events_corrupt", events.error().message});
        }
        bool invalid_segment_path = false;
        for (const auto& segment : data.segments) {
            if (!safe_relative_path(segment.relative_path)) {
                invalid_segment_path = true;
                report.findings.push_back({data.task.recording_id, "segment_path_invalid", segment.relative_path});
            }
        }
        if (terminal(data.task.state)) continue;
        if (data.manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            report.findings.push_back({data.task.recording_id, "manifest_revision_exhausted", "manifest is read-only"});
            continue;
        }
        ManifestData incomplete = data;
        ++incomplete.manifest_revision;
        incomplete.task.state = TaskState::Incomplete;
        incomplete.task.error = Error{ErrorCode::StorageError, Effects::ConfirmedPartial, "recovery interrupted active task"};
        for (auto& segment : incomplete.segments) {
            if (segment.state == SegmentLifecycleState::Writing || segment.state == SegmentLifecycleState::Finalizing) {
                segment.state = SegmentLifecycleState::Incomplete;
            }
        }
        const auto committed = manifest.commit(incomplete);
        if (!committed) {
            report.findings.push_back({incomplete.task.recording_id, "incomplete_commit_failed", committed.error().message});
            continue;
        }
        report.findings.push_back({incomplete.task.recording_id, "task_marked_incomplete",
                                   invalid_segment_path ? "recovery did not resume prior recording; invalid segment path retained" :
                                                          "recovery did not resume prior recording"});
    }
    return Result<RecoveryReport, Error>(std::move(report));
}

}  // namespace ros2_bag::recorder
