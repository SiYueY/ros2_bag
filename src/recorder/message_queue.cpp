#include "ros2_bag/recorder/message_queue.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#include <concurrentqueue.h>
#include <lightweightsemaphore.h>

namespace ros2_bag::recorder {

class MessageQueue::Storage {
public:
    Storage(std::size_t capacity, std::size_t max_producers) : queue(capacity, 0, max_producers) {}

    moodycamel::ConcurrentQueue<RecordedMessage> queue;
    moodycamel::LightweightSemaphore available;
};

MessageQueue::MessageQueue(
    std::size_t max_messages,
    std::size_t max_bytes,
    std::size_t max_producers)
: storage_(create_storage(max_messages, max_bytes, max_producers)),
  max_messages_(max_messages),
  max_bytes_(max_bytes) {}

MessageQueue::~MessageQueue() = default;

bool MessageQueue::try_push(RecordedMessage message) {
    if (closed_.load(std::memory_order_acquire)) {
        return false;
    }

    active_pushes_.fetch_add(1, std::memory_order_acq_rel);
    if (closed_.load(std::memory_order_acquire)) {
        finish_push();
        return false;
    }

    const auto payload_bytes = message.payload_bytes;
    if (!reserve(payload_bytes)) {
        finish_push();
        return false;
    }

    if (!storage_->queue.enqueue(std::move(message))) {
        release(payload_bytes);
        finish_push();
        return false;
    }

    storage_->available.signal();
    finish_push();
    return true;
}

std::optional<RecordedMessage> MessageQueue::pop_for(std::chrono::milliseconds timeout) {
    const auto timeout_us = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();

    bool available = false;
    if (timeout_us <= 0) {
        available = storage_->available.tryWait();
    } else {
        available = storage_->available.wait(timeout_us);
    }
    if (!available) {
        return std::nullopt;
    }

    RecordedMessage message;
    if (!storage_->queue.try_dequeue(message)) {
        return std::nullopt;
    }

    release(message.payload_bytes);
    return message;
}

std::vector<RecordedMessage> MessageQueue::pop_batch(std::size_t maximum) {
    std::vector<RecordedMessage> batch;
    if (maximum == 0) {
        return batch;
    }

    const auto semaphore_maximum = static_cast<std::size_t>(
        std::numeric_limits<moodycamel::LightweightSemaphore::ssize_t>::max());
    const auto permits =
        storage_->available.tryWaitMany(static_cast<moodycamel::LightweightSemaphore::ssize_t>(
            std::min(maximum, semaphore_maximum)));
    if (permits <= 0) {
        return batch;
    }

    batch.resize(static_cast<std::size_t>(permits));
    const auto count =
        storage_->queue.try_dequeue_bulk(batch.begin(), static_cast<std::size_t>(permits));
    if (count < static_cast<std::size_t>(permits) && !closed_.load(std::memory_order_acquire)) {
        storage_->available.signal(static_cast<moodycamel::LightweightSemaphore::ssize_t>(
            static_cast<std::size_t>(permits) - count));
    }
    batch.resize(count);

    std::size_t bytes = 0;
    for (const auto& message : batch) {
        bytes += message.payload_bytes;
    }
    if (count != 0) {
        used_messages_.fetch_sub(count, std::memory_order_relaxed);
        used_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }
    return batch;
}

void MessageQueue::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    while (active_pushes_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }

    storage_->available.signal();
}

bool MessageQueue::closed() const noexcept { return closed_.load(std::memory_order_acquire); }

QueueState MessageQueue::state() const noexcept {
    QueueState result;
    result.messages.used = used_messages_.load(std::memory_order_relaxed);
    result.messages.max = max_messages_;
    result.bytes.used = used_bytes_.load(std::memory_order_relaxed);
    result.bytes.max = max_bytes_;
    return result;
}

std::unique_ptr<MessageQueue::Storage> MessageQueue::create_storage(
    std::size_t max_messages,
    std::size_t max_bytes,
    std::size_t max_producers) {
    if (max_messages == 0 || max_bytes == 0 || max_producers == 0) {
        throw std::invalid_argument("MessageQueue limits must be positive");
    }
    return std::make_unique<Storage>(max_messages, max_producers);
}

bool MessageQueue::reserve(std::size_t payload_bytes) noexcept {
    if (payload_bytes > max_bytes_) {
        return false;
    }

    auto messages = used_messages_.load(std::memory_order_relaxed);
    while (true) {
        if (messages >= max_messages_) {
            return false;
        }
        if (used_messages_.compare_exchange_weak(
                messages,
                messages + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }

    auto bytes = used_bytes_.load(std::memory_order_relaxed);
    while (true) {
        if (bytes > max_bytes_ - payload_bytes) {
            used_messages_.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        if (used_bytes_.compare_exchange_weak(
                bytes,
                bytes + payload_bytes,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return true;
        }
    }
}

void MessageQueue::release(std::size_t payload_bytes) noexcept {
    used_messages_.fetch_sub(1, std::memory_order_relaxed);
    used_bytes_.fetch_sub(payload_bytes, std::memory_order_relaxed);
}

void MessageQueue::finish_push() noexcept {
    active_pushes_.fetch_sub(1, std::memory_order_release);
}

}  // namespace ros2_bag::recorder
