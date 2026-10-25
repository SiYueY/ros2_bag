// Durable task manifest serialization with atomic replacement on POSIX filesystems.
#ifndef ROS2_BAG_MANIFEST_HPP_
#define ROS2_BAG_MANIFEST_HPP_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ros2_bag {

struct ManifestSegment {
    std::uint64_t segment_id = 0;
    std::string path;
    std::string state;
    std::uint64_t completed_at_ns = 0;
};

struct RecordingManifest {
    std::string session_id = "default";
    std::string recording_id;
    std::string mode;
    std::string recording_state;
    std::string last_error;
    std::vector<std::string> secondary_errors;
    std::vector<ManifestSegment> segments;
    std::vector<std::string> cleanup_pending;
};

// Replaces manifest.json only after fully writing and fsyncing a temporary file.
// Returns false on any filesystem error and leaves the prior manifest intact.
bool write_manifest_atomic(
    const std::filesystem::path& manifest_path,
    const RecordingManifest& manifest,
    std::string* error) noexcept;

}  // namespace ros2_bag

#endif  // ROS2_BAG_MANIFEST_HPP_
