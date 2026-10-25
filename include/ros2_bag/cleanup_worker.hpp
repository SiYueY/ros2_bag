// Single-threaded bounded cleanup executor for completed recording segments.
#ifndef ROS2_BAG_CLEANUP_WORKER_HPP_
#define ROS2_BAG_CLEANUP_WORKER_HPP_

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

namespace ros2_bag {

// Executes segment deletion in one background thread. Completion runs on the
// worker thread and must not block or call back into CleanupWorker. enqueue()
// is thread-safe; shutdown() waits for the currently executing filesystem call.
class CleanupWorker {
public:
    using RemoveDirectory = std::function<bool(const std::filesystem::path&, std::string*)>;
    using Completion = std::function<void(const std::filesystem::path&, bool, const std::string&)>;

    CleanupWorker(
        std::size_t queue_capacity,
        Completion completion,
        RemoveDirectory remove_directory = RemoveDirectory());
    ~CleanupWorker();
    CleanupWorker(const CleanupWorker&) = delete;
    CleanupWorker& operator=(const CleanupWorker&) = delete;

    // Schedules one directory removal. Returns false if shutdown has started
    // or the bounded queue is full. No filesystem operation occurs on caller.
    bool enqueue(const std::filesystem::path& path);

    // Stops accepting work and joins the worker after the current operation.
    void shutdown();

    // Returns an active path once when its deletion has exceeded warning_after.
    // It never interrupts the filesystem call and is safe from Coordinator
    // timer threads.
    std::optional<std::filesystem::path> take_timed_out_path(std::chrono::seconds warning_after);

private:
    void run();

    const std::size_t queue_capacity_;
    Completion completion_;
    const RemoveDirectory remove_directory_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::filesystem::path> pending_;
    std::optional<std::filesystem::path> active_path_;
    std::chrono::steady_clock::time_point active_started_at_{};
    bool active_timeout_reported_ = false;
    std::thread thread_;
    bool stopping_ = false;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_CLEANUP_WORKER_HPP_
