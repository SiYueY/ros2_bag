// POSIX implementation of atomic manifest replacement.
#include "ros2_bag/manifest.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

namespace ros2_bag {
namespace {

bool set_error(const std::string& value, std::string* error) noexcept {
    if (error != nullptr) *error = value;
    return false;
}

}  // namespace

bool write_manifest_atomic(
    const std::filesystem::path& manifest_path,
    const RecordingManifest& manifest,
    std::string* error) noexcept {
    try {
        nlohmann::json document;
        document["schema_version"] = 1;
        document["session_id"] = manifest.session_id;
        document["recording_id"] = manifest.recording_id;
        document["mode"] = manifest.mode;
        document["recording_state"] = manifest.recording_state;
        document["timestamp_policy"] = "receive";
        document["topic_events_file"] = "topic_events.jsonl";
        document["last_error"] = manifest.last_error.empty() ? nlohmann::json(nullptr)
                                                             : nlohmann::json(manifest.last_error);
        document["secondary_errors"] = manifest.secondary_errors;
        document["cleanup_pending"] = manifest.cleanup_pending;
        document["segments"] = nlohmann::json::array();
        for (const ManifestSegment& segment : manifest.segments) {
            document["segments"].push_back(
                {{"segment_id", segment.segment_id},
                 {"path", segment.path},
                 {"state", segment.state},
                 {"completed_at_ns", segment.completed_at_ns}});
        }
        const std::filesystem::path temporary = manifest_path.string() + ".tmp";
        const std::string bytes = document.dump(2) + "\n";
        const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (descriptor < 0) return set_error(std::strerror(errno), error);
        const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
        if (written != static_cast<ssize_t>(bytes.size()) || ::fsync(descriptor) != 0 ||
            ::close(descriptor) != 0) {
            const std::string message = std::strerror(errno);
            ::close(descriptor);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return set_error(message, error);
        }
        if (::rename(temporary.c_str(), manifest_path.c_str()) != 0)
            return set_error(std::strerror(errno), error);
        const int directory = ::open(manifest_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (directory >= 0) {
            const int result = ::fsync(directory);
            ::close(directory);
            if (result != 0) return set_error(std::strerror(errno), error);
        }
        return true;
    } catch (const std::exception& exception) {
        return set_error(exception.what(), error);
    }
}

}  // namespace ros2_bag
