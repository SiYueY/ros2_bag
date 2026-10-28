#ifndef ROS2_BAG_RECORDER_RESOURCE_MONITOR_HPP_
#define ROS2_BAG_RECORDER_RESOURCE_MONITOR_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <optional>

#include "ros2_bag/recorder/state.hpp"

namespace ros2_bag::recorder {

struct DiskAdmissionSnapshot {
    DiskSafetyState state = DiskSafetyState::Unknown;
    std::int64_t valid_until_steady_ns = 0;
};

class DiskAdmissionState {
public:
    void publish(DiskSafetyState state, std::int64_t valid_until_steady_ns) noexcept;
    std::optional<DiskAdmissionSnapshot> read(std::uint32_t maximum_retries = 3) const noexcept;

private:
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<DiskSafetyState> state_{DiskSafetyState::Unknown};
    std::atomic<std::int64_t> valid_until_steady_ns_{0};
};

struct DiskMonitorConfig {
    std::filesystem::path path;
    std::chrono::milliseconds interval{2000};
    std::chrono::milliseconds max_age{10000};
    std::uint32_t failure_threshold = 3;
    double warning_percent = 80.0;
    double pause_percent = 90.0;
    std::uint64_t min_free_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
};

class ResourceMonitor {
public:
    using SampleCallback = std::function<void(DiskSample)>;

    ResourceMonitor(DiskMonitorConfig config, DiskAdmissionState& admission, SampleCallback callback);
    ~ResourceMonitor();

    ResourceMonitor(const ResourceMonitor&) = delete;
    ResourceMonitor& operator=(const ResourceMonitor&) = delete;

    void stop();

private:
    void run();
    DiskSample sample();

    DiskMonitorConfig config_;
    DiskAdmissionState& admission_;
    SampleCallback callback_;
    std::atomic<bool> stopping_{false};
    std::mutex stop_mutex_;
    std::condition_variable stop_condition_;
    std::thread thread_;
    std::uint32_t consecutive_failures_ = 0;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_RESOURCE_MONITOR_HPP_
