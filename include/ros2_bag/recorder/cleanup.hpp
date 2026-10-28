#ifndef ROS2_BAG_RECORDER_CLEANUP_HPP_
#define ROS2_BAG_RECORDER_CLEANUP_HPP_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

#include "ros2_bag/recorder/error.hpp"
#include "ros2_bag/recorder/state.hpp"

namespace ros2_bag::recorder {

struct DeleteWork {
    std::string recording_id;
    std::uint64_t segment_id = 0;
    std::filesystem::path path;
    std::uint64_t authorized_manifest_revision = 0;
};

struct DeleteResult {
    DeleteWork work;
    std::optional<Error> error;
};

class Cleanup {
public:
    using ResultCallback = std::function<void(DeleteResult)>;

    Cleanup(
        std::size_t queue_capacity,
        std::chrono::seconds warning_after,
        ResultCallback callback);
    ~Cleanup();

    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;

    bool enqueue(DeleteWork work);
    void stop();
    CleanupState state() const;

private:
    void run();

    const std::size_t queue_capacity_;
    const std::chrono::seconds warning_after_;
    ResultCallback callback_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<DeleteWork> queue_;
    bool stopping_ = false;
    std::optional<DeleteWork> active_;
    std::optional<std::chrono::steady_clock::time_point> active_since_;
    std::uint64_t queue_full_count_ = 0;
    std::uint64_t total_removed_ = 0;
    std::optional<Error> last_error_;
    std::thread thread_;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_CLEANUP_HPP_
