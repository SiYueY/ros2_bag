// Process-independent disk sampling and safety-state classification.
#ifndef ROS2_BAG_RESOURCE_MONITOR_HPP_
#define ROS2_BAG_RESOURCE_MONITOR_HPP_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>

#include "ros2_bag/config.hpp"

namespace ros2_bag {

enum class DiskProtectionState { kNormal, kWarning, kCritical, kUnknown };

struct DiskSample {
    DiskProtectionState state = DiskProtectionState::kUnknown;
    std::uint64_t total_bytes = 0;
    std::uint64_t available_bytes = 0;
    std::uint64_t used_percent = 0;
    std::uint64_t sampled_at_ns = 0;
    std::uint64_t last_attempt_at_ns = 0;
    std::size_t consecutive_failures = 0;
};

struct ProcessSample {
    bool sampling_enabled = false;
    bool valid = false;
    bool cpu_valid = false;
    std::uint64_t rss_bytes = 0;
    double cpu_percent = 0.0;
    std::uint64_t thread_count = 0;
    std::uint64_t fd_count = 0;
    std::uint64_t memory_available_bytes = 0;
    std::uint64_t cpu_count = 0;
};

// Samples the filesystem containing output_root. The caller invokes sample at
// disk.check_interval_sec; failures preserve the last known successful sample.
class ResourceMonitor {
public:
    ResourceMonitor(
        std::filesystem::path output_root,
        DiskConfig disk_config,
        ResourceConfig resource_config);
    ~ResourceMonitor();

    ResourceMonitor(const ResourceMonitor&) = delete;
    ResourceMonitor& operator=(const ResourceMonitor&) = delete;

    // Starts the single process-level worker. The worker performs process and
    // disk I/O outside the data mutex, so latest() remains usable if statvfs()
    // stalls. Calling start more than once has no effect.
    void start();
    // Stops and joins the sampling worker. Safe to call repeatedly.
    void stop();

    // Immediately performs a disk sample. This is used by the synchronous
    // start safety gate and may block in statvfs().
    DiskSample sample() noexcept;
    DiskSample latest() const noexcept;
    ProcessSample latest_process() const noexcept;

private:
    DiskSample sample_impl(bool sample_disk) noexcept;
    void worker_main();

    std::filesystem::path output_root_;
    DiskConfig disk_config_;
    ResourceConfig resource_config_;
    mutable std::mutex mutex_;
    std::mutex sampling_mutex_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::thread worker_thread_;
    bool worker_started_ = false;
    bool worker_stopping_ = false;
    DiskSample sample_;
    ProcessSample process_;
    double previous_cpu_seconds_ = 0.0;
    std::chrono::steady_clock::time_point previous_process_sample_{};
    std::chrono::steady_clock::time_point last_success_;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_RESOURCE_MONITOR_HPP_
