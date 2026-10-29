#include "ros2_bag/recorder/config.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace ros2_bag::recorder {
namespace {

Result<void, Error> invalid(std::string message) {
    return Result<void, Error>(
        unexpect, Error{ErrorCode::InvalidArgument, Effects::None, std::move(message)});
}

}  // namespace

Result<void, Error> validate_config(const RecorderConfig& config) {
    if (config.output_root.empty()) {
        return invalid("output_root must not be empty");
    }

    if (config.rolling.segment_duration.count() <= 0) {
        return invalid("rolling.segment_duration must be positive");
    }
    if (config.rolling.retention_duration.count() <= 0) {
        return invalid("rolling.retention_duration must be positive");
    }
    if (config.operation_queue_capacity == 0) {
        return invalid("operation_queue_capacity must be positive");
    }
    if (config.native_wait_warning.count() <= 0) {
        return invalid("native_wait_warning must be positive");
    }

    if (config.discovery.max_results == 0) {
        return invalid("discovery.max_results must be positive");
    }
    if (config.discovery.timeout.count() <= 0) {
        return invalid("discovery.timeout must be positive");
    }
    if (config.resource.sample_interval.count() <= 0) {
        return invalid("resource.sample_interval must be positive");
    }
    if (config.disk.check_interval.count() <= 0) {
        return invalid("disk.check_interval must be positive");
    }
    if (config.discovery.max_profiles == 0) {
        return invalid("discovery.max_profiles must be positive");
    }
    if (config.max_active_topics == 0) {
        return invalid("max_active_topics must be positive");
    }
    if (config.max_distinct_topics == 0) {
        return invalid("max_distinct_topics must be positive");
    }

    if (config.queue.max_bytes == 0) {
        return invalid("queue.max_bytes must be positive");
    }
    if (config.queue.max_messages == 0) {
        return invalid("queue.max_messages must be positive");
    }
    if (config.queue.batch_size == 0) {
        return invalid("queue.batch_size must be positive");
    }

    if (!std::isfinite(config.pool.capacity_factor)) {
        return invalid("pool.capacity_factor must be finite");
    }
    if (config.pool.capacity_factor < 1.0) {
        return invalid("pool.capacity_factor must be at least one");
    }
    if (config.pool.max_capacity == 0) {
        return invalid("pool.max_capacity must be positive");
    }

    if (config.snapshot.max_messages_per_topic == 0) {
        return invalid("snapshot.max_messages_per_topic must be positive");
    }
    if (config.snapshot.max_bytes_per_topic == 0) {
        return invalid("snapshot.max_bytes_per_topic must be positive");
    }
    if (config.snapshot.max_total_bytes == 0) {
        return invalid("snapshot.max_total_bytes must be positive");
    }

    if (config.cleanup.queue_capacity == 0) {
        return invalid("cleanup.queue_capacity must be positive");
    }
    if (config.cleanup.warning.count() <= 0) {
        return invalid("cleanup.warning must be positive");
    }
    if (config.cleanup.retry_interval.count() <= 0) {
        return invalid("cleanup.retry_interval must be positive");
    }

    if (config.disk.max_age.count() <= 0) {
        return invalid("disk.max_age must be positive");
    }
    if (config.disk.failure_threshold == 0) {
        return invalid("disk.failure_threshold must be positive");
    }
    if (config.disk.stable.count() <= 0) {
        return invalid("disk.stable must be positive");
    }

    if (config.history.max_segment_records == 0) {
        return invalid("history.max_segment_records must be positive");
    }
    if (config.history.default_segment_limit == 0) {
        return invalid("history.default_segment_limit must be positive");
    }
    if (config.history.max_segment_limit == 0) {
        return invalid("history.max_segment_limit must be positive");
    }
    if (config.history.max_segment_limit > 200) {
        return invalid("history.max_segment_limit must not exceed 200");
    }
    if (config.history.default_segment_limit > config.history.max_segment_limit) {
        return invalid("history.default_segment_limit must not exceed history.max_segment_limit");
    }
    if (config.history.max_task_scan_entries == 0) {
        return invalid("history.max_task_scan_entries must be positive");
    }

    if (config.discovery.max_results >
        std::numeric_limits<std::size_t>::max() / config.discovery.max_profiles) {
        return invalid("discovery profile capacity overflows");
    }

    if (!std::isfinite(config.disk.warning_percent)) {
        return invalid("disk.warning_percent must be finite");
    }
    if (!std::isfinite(config.disk.pause_percent)) {
        return invalid("disk.pause_percent must be finite");
    }
    if (!std::isfinite(config.disk.resume_percent)) {
        return invalid("disk.resume_percent must be finite");
    }

    if (config.disk.warning_percent < 0.0 || config.disk.warning_percent > 100.0) {
        return invalid("disk.warning_percent must be in [0, 100]");
    }
    if (config.disk.pause_percent < 0.0 || config.disk.pause_percent > 100.0) {
        return invalid("disk.pause_percent must be in [0, 100]");
    }
    if (config.disk.resume_percent < 0.0 || config.disk.resume_percent > 100.0) {
        return invalid("disk.resume_percent must be in [0, 100]");
    }

    if (config.disk.warning_percent >= config.disk.pause_percent) {
        return invalid("disk.warning_percent must be less than disk.pause_percent");
    }
    if (config.disk.resume_percent >= config.disk.pause_percent) {
        return invalid("disk.resume_percent must be less than disk.pause_percent");
    }

    const auto capacity = message_pool_capacity(config);
    if (!capacity) {
        return Result<void, Error>(unexpect, capacity.error());
    }
    return Result<void, Error>();
}
Result<std::size_t, Error> message_pool_capacity(const RecorderConfig& config) {
    if (!std::isfinite(config.pool.capacity_factor) || config.pool.capacity_factor < 1.0) {
        return Result<std::size_t, Error>(
            unexpect,
            Error{
                ErrorCode::InvalidArgument, Effects::None, "invalid message pool capacity factor"});
    }
    const long double requested = std::ceil(
        static_cast<long double>(config.queue.max_messages) * config.pool.capacity_factor);
    if (requested > static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
        return Result<std::size_t, Error>(
            unexpect,
            Error{ErrorCode::InvalidArgument, Effects::None, "message pool capacity overflows"});
    }
    const auto capacity = std::min(static_cast<std::size_t>(requested), config.pool.max_capacity);
    if (capacity == 0) {
        return Result<std::size_t, Error>(
            unexpect, Error{
                          ErrorCode::InvalidArgument, Effects::None,
                          "message pool capacity must be positive"});
    }
    return Result<std::size_t, Error>(capacity);
}

Result<TaskConfig, Error> make_task_config(
    const RecorderConfig& config, const StartOptions& options) {
    if (options.recording_id.empty()) {
        return Result<TaskConfig, Error>(
            unexpect,
            Error{ErrorCode::InvalidArgument, Effects::None, "recording_id must not be empty"});
    }

    TaskConfig task_config;
    task_config.compression = config.compression;

    if (std::holds_alternative<StartOptions::Normal>(options.mode)) {
        task_config.mode = TaskMode::Normal;
        return Result<TaskConfig, Error>(std::move(task_config));
    }

    if (const auto* timed = std::get_if<StartOptions::Timed>(&options.mode)) {
        if (timed->duration.count() <= 0) {
            return Result<TaskConfig, Error>(
                unexpect, Error{
                              ErrorCode::InvalidArgument, Effects::None,
                              "timed mode requires a positive duration"});
        }
        task_config.mode = TaskMode::Timed;
        task_config.timed.duration = timed->duration;
        return Result<TaskConfig, Error>(std::move(task_config));
    }

    const auto& rolling = std::get<StartOptions::Rolling>(options.mode);
    if ((rolling.segment_duration && rolling.segment_duration->count() <= 0) ||
        (rolling.retention_duration && rolling.retention_duration->count() <= 0)) {
        return Result<TaskConfig, Error>(
            unexpect,
            Error{ErrorCode::InvalidArgument, Effects::None, "invalid rolling mode durations"});
    }
    task_config.mode = TaskMode::Rolling;
    task_config.rolling.segment_duration =
        rolling.segment_duration.value_or(config.rolling.segment_duration);
    task_config.rolling.retention_duration =
        rolling.retention_duration.value_or(config.rolling.retention_duration);
    return Result<TaskConfig, Error>(std::move(task_config));
}
}  // namespace ros2_bag::recorder
