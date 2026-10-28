#include "ros2_bag/recorder/recovery.hpp"

#include <filesystem>
#include <limits>
#include <utility>

#include "ros2_bag/recorder/manifest.hpp"

namespace ros2_bag::recorder {
namespace {

bool terminal(TaskState state) {
    return state == TaskState::Completed || state == TaskState::Failed || state == TaskState::Incomplete;
}

Error storage_error(std::string message) {
    return {ErrorCode::StorageError, Effects::None, std::move(message)};
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
        if (terminal(manifest.data().task.state)) continue;
        if (manifest.data().manifest_revision == std::numeric_limits<std::uint64_t>::max()) {
            report.findings.push_back({manifest.data().task.recording_id, "manifest_revision_exhausted", "manifest is read-only"});
            continue;
        }
        ManifestData incomplete = manifest.data();
        ++incomplete.manifest_revision;
        incomplete.task.state = TaskState::Incomplete;
        incomplete.task.error = Error{ErrorCode::StorageError, Effects::ConfirmedPartial, "recovery interrupted active task"};
        const auto committed = manifest.commit(incomplete);
        if (!committed) {
            report.findings.push_back({incomplete.task.recording_id, "incomplete_commit_failed", committed.error().message});
            continue;
        }
        report.findings.push_back({incomplete.task.recording_id, "task_marked_incomplete", "recovery did not resume prior recording"});
    }
    return Result<RecoveryReport, Error>(std::move(report));
}

}  // namespace ros2_bag::recorder
