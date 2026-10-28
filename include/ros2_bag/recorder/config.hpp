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
    struct Rolling {
        std::chrono::seconds segment_duration{60};
        std::chrono::seconds retention_duration{600};
    };

    struct Queue {
        std::size_t max_bytes = 64U * 1024U * 1024U;
        std::size_t max_messages = 65536;
        std::size_t batch_size = 256;
    };

    struct Pool {
        double capacity_factor = 1.25;
        std::size_t max_capacity = 1000000;
    };

    struct Snapshot {
        std::size_t max_messages_per_topic = 1024;
        std::size_t max_bytes_per_topic = 8U * 1024U * 1024U;
        std::size_t max_total_bytes = 16U * 1024U * 1024U;
    };

    struct Discovery {
        std::size_t max_results = 4096;
        std::size_t max_profiles = 64;
        std::chrono::milliseconds visibility_max_age{5000};
    };

    struct Cleanup {
        std::size_t queue_capacity = 16;
        std::chrono::seconds warning{30};
        std::chrono::seconds retry_interval{2};
    };

    struct Disk {
        std::chrono::milliseconds max_age{10000};
        std::uint32_t failure_threshold = 3;
        double warning_percent = 80.0;
        double pause_percent = 90.0;
        double resume_percent = 85.0;
        std::uint64_t min_free_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
        bool auto_resume = false;
        std::chrono::seconds stable{10};
    };

    struct History {
        std::size_t max_segment_records = 4096;
        std::size_t default_segment_limit = 100;
        std::size_t max_segment_limit = 200;
        std::size_t max_task_scan_entries = 4096;
    };

    std::filesystem::path output_root = "./recordings";
    CompressionMode compression = CompressionMode::Lz4;
    std::size_t operation_queue_capacity = 64;
    std::chrono::milliseconds native_wait_warning{30000};
    std::size_t max_active_topics = 64;
    std::size_t recent_removed_instances = 32;

    Rolling rolling;
    Queue queue;
    Pool pool;
    Snapshot snapshot;
    Discovery discovery;
    Cleanup cleanup;
    Disk disk;
    History history;
};

Result<void, Error> validate_config(const RecorderConfig& config);
Result<std::size_t, Error> message_pool_capacity(const RecorderConfig& config);
Result<TaskConfig, Error> make_task_config(
    const RecorderConfig& config,
    const StartOptions& options);

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_CONFIG_HPP_
