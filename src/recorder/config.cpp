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
        unexpect,
        Error{ErrorCode::InvalidArgument, Effects::None, std::move(message)});
}

}  // namespace

Result<void, Error> validate_config(const RecorderConfig& config) {
    if (config.output_root.empty()) {
        return invalid("output_root must not be empty");
    }
    if (config.rolling_segment_duration.count() <= 0 ||
        config.rolling_retention_duration.count() <= 0 || config.operation_queue_capacity == 0 ||
        config.native_wait_warning.count() <= 0 || config.discovery_max_results == 0 ||
        config.discovery_max_endpoint_profiles == 0 || config.max_active_topics == 0 ||
        config.message_queue_max_bytes == 0 || config.message_queue_max_messages == 0 ||
        config.message_queue_batch_size == 0 || config.writer_control_queue_capacity == 0 ||
        config.writer_max_control_batch == 0 || config.snapshot_max_messages_per_topic == 0 ||
        config.snapshot_max_bytes_per_topic == 0 || config.snapshot_max_total_bytes == 0 ||
        config.cleanup_queue_capacity == 0 || config.cleanup_warning.count() <= 0 ||
        config.cleanup_retry_interval.count() <= 0 || config.disk_max_age.count() <= 0 ||
        config.disk_consecutive_failures == 0 || config.disk_stable.count() <= 0 ||
        config.max_segment_records_per_task == 0 || config.default_segment_detail_limit == 0 ||
        config.max_segment_detail_limit == 0 || config.max_segment_detail_limit > 200 ||
        config.default_segment_detail_limit > config.max_segment_detail_limit ||
        config.max_task_scan_entries == 0) {
        return invalid("all Recorder capacities and durations must be positive");
    }
    if (!std::isfinite(config.message_pool_capacity_factor) ||
        config.message_pool_capacity_factor < 1.0 || config.message_pool_max_capacity == 0) {
        return invalid("message pool capacity factor must be finite and at least one");
    }
    if (config.discovery_max_results >
        std::numeric_limits<std::size_t>::max() / config.discovery_max_endpoint_profiles) {
        return invalid("discovery profile capacity overflows");
    }
    if (!std::isfinite(config.disk_warning_percent) || !std::isfinite(config.disk_pause_percent) ||
        !std::isfinite(config.disk_resume_percent) || config.disk_warning_percent < 0.0 ||
        config.disk_resume_percent < 0.0 || config.disk_pause_percent > 100.0 ||
        !(config.disk_warning_percent < config.disk_pause_percent) ||
        !(config.disk_resume_percent < config.disk_pause_percent)) {
        return invalid("disk thresholds must satisfy warning < pause and resume < pause");
    }
    const auto capacity = message_pool_capacity(config);
    if (!capacity) {
        return Result<void, Error>(unexpect, capacity.error());
    }
    return Result<void, Error>();
}

Result<std::size_t, Error> message_pool_capacity(const RecorderConfig& config) {
    if (!std::isfinite(config.message_pool_capacity_factor) ||
        config.message_pool_capacity_factor < 1.0) {
        return Result<std::size_t, Error>(
            unexpect,
            Error{
                ErrorCode::InvalidArgument,
                Effects::None,
                "invalid message pool capacity factor"});
    }
    const long double requested = std::ceil(
        static_cast<long double>(config.message_queue_max_messages) *
        config.message_pool_capacity_factor);
    if (requested > static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
        return Result<std::size_t, Error>(
            unexpect,
            Error{ErrorCode::InvalidArgument, Effects::None, "message pool capacity overflows"});
    }
    const auto capacity =
        std::min(static_cast<std::size_t>(requested), config.message_pool_max_capacity);
    if (capacity == 0) {
        return Result<std::size_t, Error>(
            unexpect,
            Error{
                ErrorCode::InvalidArgument,
                Effects::None,
                "message pool capacity must be positive"});
    }
    return Result<std::size_t, Error>(capacity);
}

Result<TaskConfig, Error> make_task_config(
    const RecorderConfig& config,
    const StartOptions& options) {
    if (options.recording_id.empty()) {
        return Result<TaskConfig, Error>(
            unexpect,
            Error{ErrorCode::InvalidArgument, Effects::None, "recording_id must not be empty"});
    }

    TaskConfig task_config;
    task_config.mode = options.mode;
    task_config.compression = config.compression;
    switch (options.mode) {
        case TaskMode::Normal:
            if (options.duration || options.segment_duration || options.retention_duration) {
                return Result<TaskConfig, Error>(
                    unexpect,
                    Error{
                        ErrorCode::InvalidArgument,
                        Effects::None,
                        "normal mode does not accept durations"});
            }
            break;
        case TaskMode::Timed:
            if (!options.duration || options.duration->count() <= 0 || options.segment_duration ||
                options.retention_duration) {
                return Result<TaskConfig, Error>(
                    unexpect,
                    Error{
                        ErrorCode::InvalidArgument,
                        Effects::None,
                        "timed mode requires only a positive duration"});
            }
            task_config.duration = options.duration;
            break;
        case TaskMode::Rolling:
            if (options.duration ||
                (options.segment_duration && options.segment_duration->count() <= 0) ||
                (options.retention_duration && options.retention_duration->count() <= 0)) {
                return Result<TaskConfig, Error>(
                    unexpect,
                    Error{
                        ErrorCode::InvalidArgument,
                        Effects::None,
                        "invalid rolling mode durations"});
            }
            task_config.segment_duration =
                options.segment_duration.value_or(config.rolling_segment_duration);
            task_config.retention_duration =
                options.retention_duration.value_or(config.rolling_retention_duration);
            break;
    }
    return Result<TaskConfig, Error>(std::move(task_config));
}

}  // namespace ros2_bag::recorder
