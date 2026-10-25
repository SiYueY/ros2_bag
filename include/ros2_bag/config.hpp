// V1.0 configuration schema, defaults and validation for Recorder construction.
#ifndef ROS2_BAG_CONFIG_HPP_
#define ROS2_BAG_CONFIG_HPP_

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "ros2_bag/recorded_message_pool.hpp"

namespace ros2_bag {

enum class RecordingMode {
    kNormal,
    kTimed,
    kRolling,
};

struct RecordingConfig {
    RecordingMode mode = RecordingMode::kNormal;
    std::size_t segment_duration_sec = 60;
    std::size_t retention_duration_sec = 600;
    std::filesystem::path output_root = "./recordings";
};

struct ExecutorConfig {
    std::size_t thread_count = 4;
};

struct TopicManagerConfig {
    std::size_t discovery_timeout_ms = 2000;
    bool allow_empty_topics = false;
    std::size_t max_active_topics = 64;
    std::size_t max_distinct_topics = 256;
    std::size_t recent_removed_instances = 32;
};

struct QueueConfig {
    std::size_t max_bytes = 67108864;
    std::size_t max_messages = 65536;
    std::size_t batch_size = 256;
    std::size_t wait_timeout_ms = 20;
    std::string overflow_policy = "drop_newest";
};

struct PoolBuildConfig {
    bool enabled = true;
    double capacity_factor = 1.25;
    std::size_t max_capacity = 1000000;
    std::size_t max_cached_payload_bytes = 0;
    std::size_t max_per_node_buffer_bytes = 0;
    PoolConfig pool;
};

struct WriterConfig {
    std::size_t control_queue_capacity = 64;
    std::size_t max_control_batch = 8;
};

struct CleanupConfig {
    std::size_t queue_capacity = 16;
    std::size_t warning_timeout_sec = 30;
};

struct StorageConfig {
    std::string storage_id = "mcap";
    std::size_t max_cache_size = 0;
    std::filesystem::path config_file = "config/mcap.yaml";
};

struct SnapshotConfig {
    std::size_t max_total_bytes = 16777216;
};

struct ResourceConfig {
    bool enabled = true;
    std::size_t sample_interval_sec = 2;
};

struct DiskConfig {
    std::size_t check_interval_sec = 2;
    std::size_t max_sample_age_sec = 10;
    std::size_t unknown_after_failures = 3;
    std::size_t warning_percent = 80;
    std::size_t pause_percent = 90;
    std::size_t resume_percent = 85;
    std::size_t min_free_gb = 2;
    bool auto_resume = false;
    std::size_t resume_stable_sec = 10;
};

struct StatusConfig {
    double frequency_window_sec = 1.0;
    std::size_t frequency_bucket_ms = 10;
};

struct RecorderConfig {
    RecordingConfig recording;
    ExecutorConfig executor;
    TopicManagerConfig topics;
    QueueConfig queue;
    PoolBuildConfig pool;
    WriterConfig writer;
    CleanupConfig cleanup;
    StorageConfig storage;
    SnapshotConfig snapshot;
    ResourceConfig resource;
    DiskConfig disk;
    StatusConfig status;
};

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& message);
};

// Loads a YAML mapping with V1.0 defaults and rejects unsupported or unsafe
// values. output_root is resolved against the Recorder working directory;
// storage.config_file is resolved relative to this configuration file.
RecorderConfig load_recorder_config(const std::filesystem::path& path);

// Validates a programmatically assembled configuration before Recorder startup.
void validate_recorder_config(RecorderConfig* config);

}  // namespace ros2_bag

#endif  // ROS2_BAG_CONFIG_HPP_
