// ResourceMonitor Linux process telemetry and statvfs disk safety sampling.
#include "ros2_bag/resource_monitor.hpp"

#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <string>

namespace ros2_bag {
namespace {

std::uint64_t unix_now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

double usage_seconds(const rusage& usage) {
    const double user = static_cast<double>(usage.ru_utime.tv_sec) +
                        static_cast<double>(usage.ru_utime.tv_usec) / 1000000.0;
    const double system = static_cast<double>(usage.ru_stime.tv_sec) +
                          static_cast<double>(usage.ru_stime.tv_usec) / 1000000.0;
    return user + system;
}

bool parse_status_value(const std::string& line, const char* key, std::uint64_t* value) {
    const std::string prefix(key);
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    try {
        const std::size_t begin = line.find_first_of("0123456789", prefix.size());
        if (begin == std::string::npos) {
            return false;
        }
        *value = std::stoull(line.substr(begin));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

ResourceMonitor::ResourceMonitor(
    std::filesystem::path output_root,
    DiskConfig disk_config,
    ResourceConfig resource_config)
: output_root_(std::move(output_root)),
  disk_config_(disk_config),
  resource_config_(resource_config) {}

ResourceMonitor::~ResourceMonitor() { stop(); }

void ResourceMonitor::start() {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (worker_started_) {
        return;
    }
    worker_started_ = true;
    worker_stopping_ = false;
    worker_thread_ = std::thread(&ResourceMonitor::worker_main, this);
}

void ResourceMonitor::stop() {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (!worker_started_) {
            return;
        }
        if (worker_stopping_) {
            return;
        }
        worker_stopping_ = true;
    }
    worker_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_started_ = false;
}

DiskSample ResourceMonitor::sample() noexcept { return sample_impl(true); }

DiskSample ResourceMonitor::sample_impl(bool sample_disk) noexcept {
    std::lock_guard<std::mutex> sampling_lock(sampling_mutex_);
    const std::uint64_t attempt_at_ns = unix_now_ns();
    ProcessSample process;
    double previous_cpu_seconds = 0.0;
    std::chrono::steady_clock::time_point previous_process_sample;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process = process_;
        previous_cpu_seconds = previous_cpu_seconds_;
        previous_process_sample = previous_process_sample_;
    }
    if (resource_config_.enabled) {
        rusage usage{};
        const auto now = std::chrono::steady_clock::now();
        if (::getrusage(RUSAGE_SELF, &usage) == 0) {
            const double current_cpu_seconds = usage_seconds(usage);
            if (previous_process_sample != std::chrono::steady_clock::time_point{}) {
                const double elapsed_seconds =
                    std::chrono::duration<double>(now - previous_process_sample).count();
                if (elapsed_seconds > 0.0) {
                    process.cpu_percent =
                        (current_cpu_seconds - previous_cpu_seconds) / elapsed_seconds * 100.0;
                    process.cpu_valid = true;
                }
            }
            previous_cpu_seconds = current_cpu_seconds;
            previous_process_sample = now;
        }
        std::ifstream status("/proc/self/status");
        std::ifstream memory("/proc/meminfo");
        std::string line;
        std::uint64_t rss_kib = 0;
        std::uint64_t threads = 0;
        std::uint64_t memory_available_kib = 0;
        bool have_rss = false;
        bool have_threads = false;
        bool have_memory = false;
        while (std::getline(status, line)) {
            have_rss = parse_status_value(line, "VmRSS:", &rss_kib) || have_rss;
            have_threads = parse_status_value(line, "Threads:", &threads) || have_threads;
        }
        while (std::getline(memory, line)) {
            have_memory =
                parse_status_value(line, "MemAvailable:", &memory_available_kib) || have_memory;
        }
        std::error_code directory_error;
        std::uint64_t fd_count = 0;
        for (std::filesystem::directory_iterator iterator("/proc/self/fd", directory_error);
             !directory_error && iterator != std::filesystem::directory_iterator();
             iterator.increment(directory_error)) {
            ++fd_count;
        }
        const long configured_cpu_count = ::sysconf(_SC_NPROCESSORS_ONLN);
        process.sampling_enabled = true;
        process.rss_bytes = rss_kib * 1024ULL;
        process.thread_count = threads;
        process.fd_count = fd_count;
        process.memory_available_bytes = memory_available_kib * 1024ULL;
        process.cpu_count =
            configured_cpu_count > 0 ? static_cast<std::uint64_t>(configured_cpu_count) : 0;
        process.valid =
            have_rss && have_threads && have_memory && !directory_error && process.cpu_count != 0;
    } else {
        process = ProcessSample{};
    }

    struct statvfs filesystem {};
    const bool disk_success = !sample_disk || (::statvfs(output_root_.c_str(), &filesystem) == 0 &&
                                               filesystem.f_frsize != 0);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    process_ = process;
    previous_cpu_seconds_ = previous_cpu_seconds;
    previous_process_sample_ = previous_process_sample;
    if (!sample_disk) {
        return sample_;
    }
    sample_.last_attempt_at_ns = attempt_at_ns;
    if (!disk_success) {
        ++sample_.consecutive_failures;
    } else {
        const std::uint64_t block_size = static_cast<std::uint64_t>(filesystem.f_frsize);
        sample_.total_bytes = static_cast<std::uint64_t>(filesystem.f_blocks) * block_size;
        sample_.available_bytes = static_cast<std::uint64_t>(filesystem.f_bavail) * block_size;
        const std::uint64_t used_bytes = sample_.total_bytes - sample_.available_bytes;
        sample_.used_percent =
            sample_.total_bytes == 0 ? 100 : used_bytes * 100 / sample_.total_bytes;
        sample_.sampled_at_ns = sample_.last_attempt_at_ns;
        sample_.consecutive_failures = 0;
        last_success_ = now;
        const std::uint64_t minimum = disk_config_.min_free_gb * 1000ULL * 1000ULL * 1000ULL;
        if (sample_.available_bytes < minimum ||
            sample_.used_percent >= disk_config_.pause_percent) {
            sample_.state = DiskProtectionState::kCritical;
        } else if (sample_.used_percent >= disk_config_.warning_percent) {
            sample_.state = DiskProtectionState::kWarning;
        } else {
            sample_.state = DiskProtectionState::kNormal;
        }
        return sample_;
    }
    const auto age = now - last_success_;
    if (sample_.sampled_at_ns == 0 ||
        sample_.consecutive_failures >= disk_config_.unknown_after_failures ||
        age > std::chrono::seconds(disk_config_.max_sample_age_sec)) {
        sample_.state = DiskProtectionState::kUnknown;
    }
    return sample_;
}

DiskSample ResourceMonitor::latest() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    DiskSample latest = sample_;
    const auto age = std::chrono::steady_clock::now() - last_success_;
    if (latest.sampled_at_ns == 0 ||
        latest.consecutive_failures >= disk_config_.unknown_after_failures ||
        age > std::chrono::seconds(disk_config_.max_sample_age_sec)) {
        latest.state = DiskProtectionState::kUnknown;
    }
    return latest;
}

ProcessSample ResourceMonitor::latest_process() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return process_;
}

void ResourceMonitor::worker_main() {
    std::size_t disk_intervals_remaining = 0;
    const std::chrono::seconds resource_interval(resource_config_.sample_interval_sec);
    const std::size_t disk_interval_count =
        disk_config_.check_interval_sec / resource_config_.sample_interval_sec;
    for (;;) {
        const bool sample_disk = disk_intervals_remaining == 0;
        static_cast<void>(sample_impl(sample_disk));
        if (sample_disk) {
            disk_intervals_remaining = disk_interval_count - 1;
        } else {
            --disk_intervals_remaining;
        }
        std::unique_lock<std::mutex> lock(worker_mutex_);
        if (worker_cv_.wait_for(lock, resource_interval, [this]() { return worker_stopping_; })) {
            return;
        }
    }
}

}  // namespace ros2_bag
