#ifndef ROS2_BAG_RECORDER_MANIFEST_HPP_
#define ROS2_BAG_RECORDER_MANIFEST_HPP_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/task.hpp"

namespace ros2_bag::recorder {

struct ManifestData {
    std::uint64_t schema_version = 1;
    std::uint64_t manifest_revision = 0;
    TaskInfo task;
    std::string topic_events_file = "topic_events.jsonl";
    std::vector<Error> secondary_errors;
    std::vector<SegmentInfo> segments;
};

class Manifest {
public:
    static Result<Manifest, Error> create(
        const std::filesystem::path& task_directory,
        ManifestData data);
    static Result<Manifest, Error> load(const std::filesystem::path& task_directory);

    const ManifestData& data() const noexcept;
    const std::filesystem::path& task_directory() const noexcept;
    Result<void, Error> commit(ManifestData data);

private:
    Manifest(std::filesystem::path task_directory, ManifestData data);

    static std::string sanitize_error_message(std::string message);
    static std::string serialize(const ManifestData& data);
    static Result<ManifestData, Error> deserialize(std::string_view content);
    static Result<void, Error> write_durable(
        const std::filesystem::path& path,
        std::string_view content);

    std::filesystem::path task_directory_;
    ManifestData data_;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_MANIFEST_HPP_
