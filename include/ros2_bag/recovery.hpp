// Startup inspection for incomplete recording task directories.
#ifndef ROS2_BAG_RECOVERY_HPP_
#define ROS2_BAG_RECOVERY_HPP_

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ros2_bag {

struct RecoverySegmentEvidence {
    std::string path;
    bool reader_opened = false;
    std::size_t topic_metadata_count = 0;
    std::size_t message_count = 0;
    std::size_t raw_schema_count = 0;
    std::size_t raw_channel_count = 0;
    bool raw_channel_schema_links_valid = false;
    std::string raw_mcap_error;
    std::string error;
};

struct RecoveryFinding {
    std::filesystem::path task_directory;
    std::string recording_id;
    std::string recording_state;
    std::vector<std::string> incomplete_segments;
    std::vector<std::string> uncertain_operations;
    std::vector<RecoverySegmentEvidence> segment_evidence;
    std::string error;
};

// Inspects immediate task directories under output_root. It performs no cleanup
// and never changes a manifest; malformed manifests are reported as findings.
std::vector<RecoveryFinding> inspect_recovery_candidates(const std::filesystem::path& output_root);

// Completes durable DELETING cleanup intents left by a prior process. It only
// changes a segment to REMOVED after remove_all() succeeds, and is idempotent
// when the segment directory has already disappeared. Returned findings contain
// failures; callers may log them while still starting the new Recorder IDLE.
std::vector<RecoveryFinding> recover_deleting_segments(const std::filesystem::path& output_root);

// Conservatively records unexpected interrupted recordings. WRITING and
// FINALIZING segments become INCOMPLETE but are never deleted. A topic event
// operation with PREPARED and no durable terminal event is recorded as
// UNCERTAIN in recovery.json. Each retained segment is opened through the
// rosbag2 reader to record Topic metadata and readable-message evidence.
// Returned findings contain recovery write errors.
std::vector<RecoveryFinding> recover_incomplete_recordings(
    const std::filesystem::path& output_root);

}  // namespace ros2_bag

#endif  // ROS2_BAG_RECOVERY_HPP_
