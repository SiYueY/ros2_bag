#ifndef ROS2_BAG_RECORDER_ADMISSION_HPP_
#define ROS2_BAG_RECORDER_ADMISSION_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

#include "ros2_bag/recorder/resource_monitor.hpp"
#include "ros2_bag/recorder/state.hpp"

namespace ros2_bag::recorder {

enum class AdmissionResult {
    Accepted,
    StaleIdentity,
    NotAccepting,
    TimedDeadline,
    DiskUnsafe,
    DiskSnapshotBusy,
};

class AdmissionGate {
public:
    void open(
        std::uint64_t generation,
        std::uint64_t topic_epoch,
        std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) noexcept;
    void close() noexcept;

    AdmissionResult admit(
        std::uint64_t generation,
        std::uint64_t topic_epoch,
        const DiskAdmissionState& disk,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    DropCounters counters() const noexcept;

private:
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> topic_epoch_{0};
    std::atomic<bool> accepting_{false};
    std::atomic<std::int64_t> deadline_steady_ns_{0};
    std::atomic<std::uint64_t> stale_identity_{0};
    std::atomic<std::uint64_t> not_accepting_{0};
    std::atomic<std::uint64_t> timed_deadline_{0};
    std::atomic<std::uint64_t> disk_unsafe_{0};
    std::atomic<std::uint64_t> disk_snapshot_busy_{0};
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_ADMISSION_HPP_
