// CleanupWorker implementation. Filesystem deletion stays off Coordinator threads.
#include "ros2_bag/cleanup_worker.hpp"

#include <stdexcept>
#include <utility>

namespace ros2_bag {
namespace {

bool remove_directory(const std::filesystem::path& path, std::string* error) {
    std::error_code filesystem_error;
    std::filesystem::remove_all(path, filesystem_error);
    if (filesystem_error && error != nullptr) {
        *error = filesystem_error.message();
    }
    return !filesystem_error;
}

}  // namespace

CleanupWorker::CleanupWorker(
    std::size_t queue_capacity,
    Completion completion,
    RemoveDirectory remove_directory)
: queue_capacity_(queue_capacity),
  completion_(std::move(completion)),
  remove_directory_(
      remove_directory ? std::move(remove_directory)
                       : RemoveDirectory(ros2_bag::remove_directory)) {
    if (queue_capacity_ == 0 || !completion_) {
        throw std::invalid_argument("CleanupWorker requires capacity and completion");
    }
    thread_ = std::thread(&CleanupWorker::run, this);
}

CleanupWorker::~CleanupWorker() { shutdown(); }

bool CleanupWorker::enqueue(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || pending_.size() >= queue_capacity_) {
        return false;
    }
    pending_.push(path);
    condition_.notify_one();
    return true;
}

void CleanupWorker::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::optional<std::filesystem::path> CleanupWorker::take_timed_out_path(
    std::chrono::seconds warning_after) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_path_ || active_timeout_reported_ ||
        std::chrono::steady_clock::now() - active_started_at_ < warning_after) {
        return std::nullopt;
    }
    active_timeout_reported_ = true;
    return active_path_;
}

void CleanupWorker::run() {
    for (;;) {
        std::filesystem::path path;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() { return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty()) {
                return;
            }
            path = pending_.front();
            pending_.pop();
            active_path_ = path;
            active_started_at_ = std::chrono::steady_clock::now();
            active_timeout_reported_ = false;
        }
        std::string error;
        const bool success = remove_directory_(path, &error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_path_.reset();
            active_timeout_reported_ = false;
        }
        completion_(path, success, error);
    }
}

}  // namespace ros2_bag
