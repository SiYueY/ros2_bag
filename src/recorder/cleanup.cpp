#include "ros2_bag/recorder/cleanup.hpp"

#include <system_error>
#include <utility>

namespace ros2_bag::recorder {

Cleanup::Cleanup(
    std::size_t queue_capacity,
    std::chrono::seconds warning_after,
    ResultCallback callback)
: queue_capacity_(queue_capacity),
  warning_after_(warning_after),
  callback_(std::move(callback)),
  thread_([this] { run(); }) {}

Cleanup::~Cleanup() {
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Cleanup::enqueue(DeleteWork work) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || queue_.size() >= queue_capacity_) {
        ++queue_full_;
        return false;
    }
    queue_.push(std::move(work));
    condition_.notify_one();
    return true;
}

void Cleanup::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    condition_.notify_all();
}

CleanupState Cleanup::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CleanupState result;
    result.queued = queue_.size();
    result.pending = result.queued;
    if (active_) {
        ++result.pending;
    }
    if (active_) {
        result.active = CleanupState::Active{
            active_->work.segment_id,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - active_->since)};
        result.warning = result.active->elapsed >= warning_after_;
    }
    result.queue_full = queue_full_;
    result.removed = removed_;
    result.last_error = last_error_;
    return result;
}

void Cleanup::run() {
    while (true) {
        DeleteWork work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) {
                return;
            }
            work = std::move(queue_.front());
            queue_.pop();
            active_ = Active{work, std::chrono::steady_clock::now()};
        }
        std::error_code filesystem_error;
        std::filesystem::remove_all(work.path, filesystem_error);
        DeleteResult result{work, Result<void, Error>()};
        if (filesystem_error) {
            result.result = Result<void, Error>(
                unexpect,
                Error{
                    ErrorCode::StorageError,
                    Effects::ConfirmedPartial,
                    filesystem_error.message()});
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!result.result) {
                last_error_ = result.result.error();
            } else {
                ++removed_;
            }
            active_.reset();
        }
        try {
            callback_(std::move(result));
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = Error{
                ErrorCode::InternalError,
                Effects::ConfirmedPartial,
                "cleanup result callback threw"};
        }
    }
}

}  // namespace ros2_bag::recorder
