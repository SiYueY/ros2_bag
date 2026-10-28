#include "ros2_bag/recorder/admission.hpp"

namespace ros2_bag::recorder {
namespace {

std::int64_t steady_ns(std::chrono::steady_clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

}  // namespace

void AdmissionGate::open(
    std::uint64_t generation,
    std::uint64_t topic_epoch,
    std::optional<std::chrono::steady_clock::time_point> deadline) noexcept {
    generation_.store(generation, std::memory_order_release);
    topic_epoch_.store(topic_epoch, std::memory_order_release);
    deadline_steady_ns_.store(deadline ? steady_ns(*deadline) : 0, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
}

void AdmissionGate::close() noexcept { accepting_.store(false, std::memory_order_release); }

AdmissionResult AdmissionGate::admit(
    std::uint64_t generation,
    std::uint64_t topic_epoch,
    const DiskAdmissionState& disk,
    std::chrono::steady_clock::time_point now) noexcept {
    if (generation_.load(std::memory_order_acquire) != generation ||
        topic_epoch_.load(std::memory_order_acquire) != topic_epoch) {
        stale_identity_.fetch_add(1, std::memory_order_relaxed);
        return AdmissionResult::StaleIdentity;
    }
    if (!accepting_.load(std::memory_order_acquire)) {
        not_accepting_.fetch_add(1, std::memory_order_relaxed);
        return AdmissionResult::NotAccepting;
    }
    const auto now_ns = steady_ns(now);
    const auto deadline = deadline_steady_ns_.load(std::memory_order_acquire);
    if (deadline != 0 && now_ns >= deadline) {
        accepting_.store(false, std::memory_order_release);
        timed_deadline_.fetch_add(1, std::memory_order_relaxed);
        return AdmissionResult::TimedDeadline;
    }
    const auto snapshot = disk.read();
    if (!snapshot) {
        disk_snapshot_busy_.fetch_add(1, std::memory_order_relaxed);
        return AdmissionResult::DiskSnapshotBusy;
    }
    if (snapshot->state == DiskSafetyState::Critical ||
        snapshot->state == DiskSafetyState::Unknown || now_ns > snapshot->valid_until_steady_ns) {
        accepting_.store(false, std::memory_order_release);
        disk_unsafe_.fetch_add(1, std::memory_order_relaxed);
        return AdmissionResult::DiskUnsafe;
    }
    return AdmissionResult::Accepted;
}

DropCounters AdmissionGate::counters() const noexcept {
    DropCounters result;
    result.stale_identity = stale_identity_.load(std::memory_order_relaxed);
    result.not_accepting = not_accepting_.load(std::memory_order_relaxed);
    result.timed_deadline = timed_deadline_.load(std::memory_order_relaxed);
    result.disk_unsafe = disk_unsafe_.load(std::memory_order_relaxed);
    result.disk_snapshot_busy = disk_snapshot_busy_.load(std::memory_order_relaxed);
    return result;
}

}  // namespace ros2_bag::recorder
