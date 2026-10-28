#ifndef ROS2_BAG_RECORDER_CONFIG_HPP_
#define ROS2_BAG_RECORDER_CONFIG_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/error.hpp"
#include "ros2_bag/recorder/task.hpp"

namespace ros2_bag::recorder {

struct RecorderConfig {
    std::filesystem::path output_root = "./recordings";
    CompressionMode compression = CompressionMode::Lz4;
    std::chrono::seconds rolling_segment_duration{60};
    std::chrono::seconds rolling_retention_duration{600};
    std::size_t operation_queue_capacity = 64;
    std::chrono::milliseconds native_wait_warning{30000};
    std::size_t discovery_max_results = 4096;
    std::size_t discovery_max_endpoint_profiles = 64;
    std::size_t max_active_topics = 64;
    std::size_t recent_removed_instances = 32;
    std::chrono::milliseconds publisher_visibility_max_age{5000};
    std::size_t message_queue_max_bytes = 64U * 1024U * 1024U;
    std::size_t message_queue_max_messages = 65536;
    std::size_t message_queue_batch_size = 256;
    double message_pool_capacity_factor = 1.25;
    std::size_t message_pool_max_capacity = 1000000;
    std::size_t writer_control_queue_capacity = 64;
    std::size_t writer_max_control_batch = 8;
    std::size_t snapshot_max_messages_per_topic = 1024;
    std::size_t snapshot_max_bytes_per_topic = 8U * 1024U * 1024U;
    std::size_t snapshot_max_total_bytes = 16U * 1024U * 1024U;
    std::size_t cleanup_queue_capacity = 16;
    std::chrono::seconds cleanup_warning{30};
    std::chrono::milliseconds disk_max_age{10000};
    std::uint32_t disk_consecutive_failures = 3;
    double disk_warning_percent = 80.0;
    double disk_pause_percent = 90.0;
    double disk_resume_percent = 85.0;
    std::uint64_t disk_min_free_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    bool disk_auto_resume = false;
    std::chrono::seconds disk_stable{10};
    std::size_t max_segment_records_per_task = 4096;
    std::size_t default_segment_detail_limit = 100;
    std::size_t max_segment_detail_limit = 200;
    std::size_t max_task_scan_entries = 4096;
};

Result<void, Error> validate_config(const RecorderConfig& config);
Result<std::size_t, Error> message_pool_capacity(const RecorderConfig& config);
Result<TaskConfig, Error> make_task_config(const RecorderConfig& config, const StartOptions& options);

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_CONFIG_HPP_
