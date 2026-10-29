#include "ros2_bag/recorder/resource_monitor.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <limits>
#include <utility>

namespace ros2_bag::recorder {
void DiskAdmissionState::publish(
    DiskSafetyState state, std::int64_t valid_until_steady_ns) noexcept {
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    state_.store(state, std::memory_order_relaxed);
    valid_until_steady_ns_.store(valid_until_steady_ns, std::memory_order_relaxed);
    sequence_.fetch_add(1, std::memory_order_release);
}

std::optional<DiskAdmissionSnapshot> DiskAdmissionState::read(
    std::uint32_t maximum_retries) const noexcept {
    for (std::uint32_t attempt = 0; attempt < maximum_retries; ++attempt) {
        const auto first = sequence_.load(std::memory_order_acquire);
        if ((first & 1U) != 0U) {
            continue;
        }
        const DiskAdmissionSnapshot snapshot{
            state_.load(std::memory_order_relaxed),
            valid_until_steady_ns_.load(std::memory_order_relaxed)};
        const auto second = sequence_.load(std::memory_order_acquire);
        if (first == second && (second & 1U) == 0U) {
            return snapshot;
        }
    }
    return std::nullopt;
}

ResourceMonitor::ResourceMonitor(
    DiskMonitorConfig config, DiskAdmissionState& admission, SampleCallback callback)
: config_(std::move(config)),
  admission_(admission),
  callback_(std::move(callback)),
  thread_([this] { run(); }) {}

ResourceMonitor::~ResourceMonitor() {
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ResourceMonitor::stop() {
    stopping_.store(true);
    stop_condition_.notify_all();
}

void ResourceMonitor::run() {
    while (!stopping_.load()) {
        const auto value = sample();
        if (value.state != DiskSafetyState::Unknown ||
            value.consecutive_failures >= config_.failure_threshold) {
            auto valid_until = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
            if (value.state != DiskSafetyState::Unknown) {
                valid_until +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(config_.max_age).count();
            }
            admission_.publish(value.state, valid_until);
        }
        try {
            callback_(value);
        } catch (...) {
        }
        std::unique_lock<std::mutex> lock(stop_mutex_);
        stop_condition_.wait_for(lock, config_.interval, [this] { return stopping_.load(); });
    }
}

DiskSample ResourceMonitor::sample() {
    auto result = sample_now(config_);
    if (result.state == DiskSafetyState::Unknown) {
        if (consecutive_failures_ != std::numeric_limits<std::uint32_t>::max()) {
            ++consecutive_failures_;
        }
    } else {
        consecutive_failures_ = 0;
    }
    result.consecutive_failures = consecutive_failures_;
    return result;
}

DiskSample ResourceMonitor::sample_now(const DiskMonitorConfig& config) {
    DiskSample result;
    result.sampled_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    struct statvfs filesystem {};
    if (::statvfs(config.path.c_str(), &filesystem) != 0 || filesystem.f_blocks == 0) {
        result.consecutive_failures = 1;
        result.state = DiskSafetyState::Unknown;
        return result;
    }
    result.consecutive_failures = 0;
    result.available_bytes = static_cast<std::uint64_t>(filesystem.f_bavail) * filesystem.f_frsize;
    const auto used_blocks = filesystem.f_blocks - filesystem.f_bavail;
    result.used_percent =
        100.0 * static_cast<double>(used_blocks) / static_cast<double>(filesystem.f_blocks);
    if (result.available_bytes < config.min_free_bytes ||
        result.used_percent >= config.pause_percent) {
        result.state = DiskSafetyState::Critical;
    } else if (result.used_percent >= config.warning_percent) {
        result.state = DiskSafetyState::Warning;
    } else {
        result.state = DiskSafetyState::Normal;
    }
    return result;
}

}  // namespace ros2_bag::recorder
