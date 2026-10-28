#ifndef ROS2_BAG_RECORDER_COMPLETION_HPP_
#define ROS2_BAG_RECORDER_COMPLETION_HPP_

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/error.hpp"

namespace ros2_bag::recorder {

template <class T>
class CompletionSource;

template <class T>
class Completion {
public:
    Completion() = default;

    bool wait_for(std::chrono::milliseconds timeout) const {
        if (!state_) {
            return false;
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        return state_->condition.wait_for(lock, timeout, [this] { return state_->result.has_value(); });
    }

    bool done() const noexcept {
        if (!state_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->result.has_value();
    }

    std::optional<Result<T, Error>> result() const {
        if (!state_) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->result;
    }

private:
    struct SharedState {
        mutable std::mutex mutex;
        std::condition_variable condition;
        std::optional<Result<T, Error>> result;
    };

    explicit Completion(std::shared_ptr<SharedState> state) : state_(std::move(state)) {}

    void complete(Result<T, Error> result) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->result.has_value()) {
            return;
        }
        state_->result.emplace(std::move(result));
        state_->condition.notify_all();
    }

    static std::pair<Completion, Completion> make_pair() {
        auto state = std::make_shared<SharedState>();
        return {Completion(state), Completion(std::move(state))};
    }

    std::shared_ptr<SharedState> state_;

    template <class>
    friend class CompletionSource;
};

template <class T>
class CompletionSource {
public:
    CompletionSource() {
        auto pair = Completion<T>::make_pair();
        completion_ = std::move(pair.first);
        producer_ = std::move(pair.second);
    }

    Completion<T> completion() const { return completion_; }

    void set_value(T value) const { producer_.complete(Result<T, Error>(std::in_place, std::move(value))); }

    void set_error(Error error) const {
        producer_.complete(Result<T, Error>(unexpect, std::move(error)));
    }

private:
    Completion<T> completion_;
    Completion<T> producer_;
};

template <>
class CompletionSource<void> {
public:
    CompletionSource() {
        auto pair = Completion<void>::make_pair();
        completion_ = std::move(pair.first);
        producer_ = std::move(pair.second);
    }

    Completion<void> completion() const { return completion_; }

    void set_value() const { producer_.complete(Result<void, Error>()); }

    void set_error(Error error) const {
        producer_.complete(Result<void, Error>(unexpect, std::move(error)));
    }

private:
    Completion<void> completion_;
    Completion<void> producer_;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_COMPLETION_HPP_
