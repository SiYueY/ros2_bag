#include "ros2_bag/recorder/message_queue.hpp"

#include <stdexcept>
#include <utility>

namespace ros2_bag::recorder {

MessageQueue::MessageQueue(std::size_t max_messages, std::size_t max_bytes)
: max_messages_(max_messages), max_bytes_(max_bytes) {
    if (max_messages == 0 || max_bytes == 0) {
        throw std::invalid_argument("MessageQueue limits must be positive");
    }
}

bool MessageQueue::try_push(RecordedMessage message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || message.payload_bytes > max_bytes_ || messages_.size() >= max_messages_ ||
        used_bytes_ > max_bytes_ - message.payload_bytes) {
        return false;
    }
    used_bytes_ += message.payload_bytes;
    messages_.push_back(std::move(message));
    condition_.notify_one();
    return true;
}

std::optional<RecordedMessage> MessageQueue::pop_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, timeout, [this] { return closed_ || !messages_.empty(); });
    if (messages_.empty()) {
        return std::nullopt;
    }
    RecordedMessage message = std::move(messages_.front());
    messages_.pop_front();
    used_bytes_ -= message.payload_bytes;
    return message;
}

std::vector<RecordedMessage> MessageQueue::pop_batch(std::size_t maximum) {
    std::vector<RecordedMessage> batch;
    if (maximum == 0) {
        return batch;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    batch.reserve(std::min(maximum, messages_.size()));
    while (!messages_.empty() && batch.size() < maximum) {
        RecordedMessage message = std::move(messages_.front());
        messages_.pop_front();
        used_bytes_ -= message.payload_bytes;
        batch.push_back(std::move(message));
    }
    return batch;
}

void MessageQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    condition_.notify_all();
}

bool MessageQueue::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

QueueState MessageQueue::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return QueueState{messages_.size(), max_messages_, used_bytes_, max_bytes_};
}

}  // namespace ros2_bag::recorder
