// YAML configuration loading and safety validation for the V1.0 Recorder.
#include "ros2_bag/config.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

#include <yaml-cpp/yaml.h>

namespace ros2_bag {
namespace {

template <typename T>
void assign_if_present(const YAML::Node& parent, const char* key, T* value) {
    if (parent && parent[key]) {
        *value = parent[key].as<T>();
    }
}

RecordingMode parse_recording_mode(const std::string& mode) {
    if (mode == "normal") {
        return RecordingMode::kNormal;
    }
    if (mode == "timed") {
        return RecordingMode::kTimed;
    }
    if (mode == "rolling") {
        return RecordingMode::kRolling;
    }
    throw ConfigError("recording.mode must be normal, timed, or rolling");
}

PoolMode parse_pool_mode(const std::string& mode) {
    if (mode == "tls") {
        return PoolMode::kTls;
    }
    if (mode == "global_mutex") {
        return PoolMode::kGlobalMutex;
    }
    throw ConfigError("pool.mode must be tls or global_mutex");
}

void reject_if_not_mapping(const YAML::Node& node, const char* path) {
    if (node && !node.IsMap()) {
        throw ConfigError(std::string(path) + " must be a mapping");
    }
}

void reject_unknown_keys(
    const YAML::Node& node,
    std::initializer_list<const char*> allowed,
    const char* path) {
    if (!node) {
        return;
    }
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
            throw ConfigError(std::string(path) + " has a non-scalar key");
        }
        const std::string key = entry.first.as<std::string>();
        bool known = false;
        for (const char* candidate : allowed) {
            if (key == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw ConfigError(std::string("unsupported configuration key: ") + path + "." + key);
        }
    }
}

}  // namespace

ConfigError::ConfigError(const std::string& message) : std::runtime_error(message) {}

RecorderConfig load_recorder_config(const std::filesystem::path& path) {
    const std::filesystem::path absolute_config_path = std::filesystem::absolute(path);
    const YAML::Node root = YAML::LoadFile(absolute_config_path.string());
    reject_if_not_mapping(root, "root");
    reject_unknown_keys(
        root,
        {"recording",
         "executor",
         "topics",
         "queue",
         "pool",
         "writer",
         "cleanup",
         "storage",
         "snapshot",
         "resource",
         "disk",
         "status"},
        "root");
    RecorderConfig config;

    const YAML::Node recording = root["recording"];
    const YAML::Node executor = root["executor"];
    const YAML::Node topics = root["topics"];
    const YAML::Node queue = root["queue"];
    const YAML::Node pool = root["pool"];
    const YAML::Node writer = root["writer"];
    const YAML::Node cleanup = root["cleanup"];
    const YAML::Node storage = root["storage"];
    const YAML::Node snapshot = root["snapshot"];
    const YAML::Node resource = root["resource"];
    const YAML::Node disk = root["disk"];
    const YAML::Node status = root["status"];
    reject_if_not_mapping(recording, "recording");
    reject_if_not_mapping(executor, "executor");
    reject_if_not_mapping(topics, "topics");
    reject_if_not_mapping(queue, "queue");
    reject_if_not_mapping(pool, "pool");
    reject_if_not_mapping(writer, "writer");
    reject_if_not_mapping(cleanup, "cleanup");
    reject_if_not_mapping(storage, "storage");
    reject_if_not_mapping(snapshot, "snapshot");
    reject_if_not_mapping(resource, "resource");
    reject_if_not_mapping(disk, "disk");
    reject_if_not_mapping(status, "status");
    reject_unknown_keys(
        recording,
        {"mode", "segment_duration_sec", "retention_duration_sec", "output_root"},
        "recording");
    reject_unknown_keys(executor, {"thread_count"}, "executor");
    reject_unknown_keys(
        topics,
        {"discovery_timeout_ms",
         "allow_empty_topics",
         "max_active_topics",
         "max_distinct_topics",
         "recent_removed_instances"},
        "topics");
    reject_unknown_keys(
        queue,
        {"max_bytes", "max_messages", "batch_size", "wait_timeout_ms", "overflow_policy"},
        "queue");
    reject_unknown_keys(
        pool,
        {"enabled",
         "mode",
         "capacity_factor",
         "max_capacity",
         "max_cached_payload_bytes",
         "max_per_node_buffer_bytes",
         "max_fallback_in_use",
         "local_capacity",
         "batch_size"},
        "pool");
    reject_unknown_keys(writer, {"control_queue_capacity", "max_control_batch"}, "writer");
    reject_unknown_keys(cleanup, {"queue_capacity", "warning_timeout_sec"}, "cleanup");
    reject_unknown_keys(storage, {"storage_id", "max_cache_size", "config_file"}, "storage");
    reject_unknown_keys(snapshot, {"max_total_bytes"}, "snapshot");
    reject_unknown_keys(resource, {"enabled", "sample_interval_sec"}, "resource");
    reject_unknown_keys(
        disk,
        {"check_interval_sec",
         "max_sample_age_sec",
         "unknown_after_failures",
         "warning_percent",
         "pause_percent",
         "resume_percent",
         "min_free_gb",
         "auto_resume",
         "resume_stable_sec"},
        "disk");
    reject_unknown_keys(status, {"frequency_window_sec", "frequency_bucket_ms"}, "status");

    std::string mode = "normal";
    assign_if_present(recording, "mode", &mode);
    config.recording.mode = parse_recording_mode(mode);
    std::string output_root = config.recording.output_root.string();
    assign_if_present(recording, "segment_duration_sec", &config.recording.segment_duration_sec);
    assign_if_present(
        recording,
        "retention_duration_sec",
        &config.recording.retention_duration_sec);
    assign_if_present(recording, "output_root", &output_root);
    config.recording.output_root = std::filesystem::absolute(output_root);

    assign_if_present(executor, "thread_count", &config.executor.thread_count);
    assign_if_present(topics, "discovery_timeout_ms", &config.topics.discovery_timeout_ms);
    assign_if_present(topics, "allow_empty_topics", &config.topics.allow_empty_topics);
    assign_if_present(topics, "max_active_topics", &config.topics.max_active_topics);
    assign_if_present(topics, "max_distinct_topics", &config.topics.max_distinct_topics);
    assign_if_present(topics, "recent_removed_instances", &config.topics.recent_removed_instances);
    assign_if_present(queue, "max_bytes", &config.queue.max_bytes);
    assign_if_present(queue, "max_messages", &config.queue.max_messages);
    assign_if_present(queue, "batch_size", &config.queue.batch_size);
    assign_if_present(queue, "wait_timeout_ms", &config.queue.wait_timeout_ms);
    assign_if_present(queue, "overflow_policy", &config.queue.overflow_policy);

    assign_if_present(pool, "enabled", &config.pool.enabled);
    std::string pool_mode = "tls";
    assign_if_present(pool, "mode", &pool_mode);
    config.pool.pool.mode = parse_pool_mode(pool_mode);
    assign_if_present(pool, "capacity_factor", &config.pool.capacity_factor);
    assign_if_present(pool, "max_capacity", &config.pool.max_capacity);
    assign_if_present(pool, "max_cached_payload_bytes", &config.pool.max_cached_payload_bytes);
    assign_if_present(pool, "max_per_node_buffer_bytes", &config.pool.max_per_node_buffer_bytes);
    assign_if_present(pool, "max_fallback_in_use", &config.pool.pool.max_fallback_in_use);
    assign_if_present(pool, "local_capacity", &config.pool.pool.local_capacity);
    assign_if_present(pool, "batch_size", &config.pool.pool.batch_size);

    assign_if_present(writer, "control_queue_capacity", &config.writer.control_queue_capacity);
    assign_if_present(writer, "max_control_batch", &config.writer.max_control_batch);
    assign_if_present(cleanup, "queue_capacity", &config.cleanup.queue_capacity);
    assign_if_present(cleanup, "warning_timeout_sec", &config.cleanup.warning_timeout_sec);
    std::string storage_id = config.storage.storage_id;
    std::string config_file = config.storage.config_file.string();
    assign_if_present(storage, "storage_id", &storage_id);
    assign_if_present(storage, "max_cache_size", &config.storage.max_cache_size);
    assign_if_present(storage, "config_file", &config_file);
    config.storage.storage_id = storage_id;
    const std::filesystem::path configured_storage_path(config_file);
    config.storage.config_file = configured_storage_path.is_absolute()
                                     ? configured_storage_path
                                     : absolute_config_path.parent_path() / configured_storage_path;
    assign_if_present(snapshot, "max_total_bytes", &config.snapshot.max_total_bytes);
    assign_if_present(resource, "enabled", &config.resource.enabled);
    assign_if_present(resource, "sample_interval_sec", &config.resource.sample_interval_sec);
    assign_if_present(disk, "check_interval_sec", &config.disk.check_interval_sec);
    assign_if_present(disk, "max_sample_age_sec", &config.disk.max_sample_age_sec);
    assign_if_present(disk, "unknown_after_failures", &config.disk.unknown_after_failures);
    assign_if_present(disk, "warning_percent", &config.disk.warning_percent);
    assign_if_present(disk, "pause_percent", &config.disk.pause_percent);
    assign_if_present(disk, "resume_percent", &config.disk.resume_percent);
    assign_if_present(disk, "min_free_gb", &config.disk.min_free_gb);
    assign_if_present(disk, "auto_resume", &config.disk.auto_resume);
    assign_if_present(disk, "resume_stable_sec", &config.disk.resume_stable_sec);
    assign_if_present(status, "frequency_window_sec", &config.status.frequency_window_sec);
    assign_if_present(status, "frequency_bucket_ms", &config.status.frequency_bucket_ms);
    validate_recorder_config(&config);
    return config;
}

void validate_recorder_config(RecorderConfig* config) {
    if (config == nullptr) {
        throw ConfigError("RecorderConfig must not be null");
    }
    if (config->queue.max_bytes == 0 || config->queue.max_messages == 0 ||
        config->queue.batch_size == 0 || config->executor.thread_count == 0 ||
        config->recording.segment_duration_sec == 0 ||
        config->recording.retention_duration_sec == 0 ||
        config->writer.control_queue_capacity == 0 || config->writer.max_control_batch == 0 ||
        config->cleanup.queue_capacity == 0 || config->cleanup.warning_timeout_sec == 0 ||
        config->resource.sample_interval_sec == 0 || config->snapshot.max_total_bytes == 0 ||
        config->status.frequency_bucket_ms == 0) {
        throw ConfigError("positive Recorder capacity or interval is required");
    }
    if (config->queue.overflow_policy != "drop_newest") {
        throw ConfigError("V1.0 supports only queue.overflow_policy=drop_newest");
    }
    if (config->pool.capacity_factor < 1.0 || config->pool.capacity_factor > 2.0 ||
        config->pool.max_capacity == 0 || config->pool.max_capacity > 1000000) {
        throw ConfigError("pool capacity configuration is out of range");
    }
    if (config->pool.max_cached_payload_bytes != 0 || config->pool.max_per_node_buffer_bytes != 0) {
        throw ConfigError("Model A requires zero Pool payload cache capacity");
    }
    const double calculated =
        std::ceil(static_cast<double>(config->queue.max_messages) * config->pool.capacity_factor);
    if (calculated > static_cast<double>(config->pool.max_capacity) ||
        calculated >= static_cast<double>(kInvalidNodeIndex) ||
        calculated > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        throw ConfigError("calculated pool capacity is invalid");
    }
    config->pool.pool.capacity = static_cast<std::size_t>(calculated);
#ifdef ROS2_BAG_PRODUCTION_BUILD
    if (!config->pool.enabled) {
        throw ConfigError("production Recorder requires pool.enabled=true");
    }
#endif
    if (config->pool.pool.mode == PoolMode::kTls &&
        (config->pool.pool.local_capacity == 0 || config->pool.pool.batch_size == 0)) {
        throw ConfigError("TLS Pool requires local_capacity and batch_size");
    }
    if (config->storage.storage_id != "mcap" || config->storage.max_cache_size != 0) {
        throw ConfigError("V1.0 requires mcap storage with max_cache_size=0");
    }
    if (config->disk.check_interval_sec < config->resource.sample_interval_sec ||
        config->disk.check_interval_sec % config->resource.sample_interval_sec != 0 ||
        config->disk.max_sample_age_sec <= config->disk.check_interval_sec ||
        config->disk.unknown_after_failures == 0 ||
        config->disk.warning_percent >= config->disk.pause_percent ||
        config->disk.resume_percent >= config->disk.pause_percent) {
        throw ConfigError("disk safety configuration is invalid");
    }
    if (config->status.frequency_window_sec < 0.1 || config->status.frequency_bucket_ms == 0 ||
        std::ceil(
            config->status.frequency_window_sec * 1000.0 /
            static_cast<double>(config->status.frequency_bucket_ms)) +
                2.0 >
            1000.0) {
        throw ConfigError("status frequency or capacity configuration is invalid");
    }
    if (config->recording.output_root.empty()) {
        throw ConfigError("recording.output_root must not be empty");
    }
}

}  // namespace ros2_bag
