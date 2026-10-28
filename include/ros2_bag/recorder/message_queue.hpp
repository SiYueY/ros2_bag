#ifndef ROS2_BAG_RECORDER_MESSAGE_QUEUE_HPP_
#define ROS2_BAG_RECORDER_MESSAGE_QUEUE_HPP_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "ros2_bag/recorder/message.hpp"
#include "ros2_bag/recorder/state.hpp"

namespace ros2_bag::recorder {

class MessageQueue {
public:
    MessageQueue(std::size_t max_messages, std::size_t max_bytes, std::size_t max_producers);
    ~MessageQueue();

    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    bool try_push(RecordedMessage message);
    std::optional<RecordedMessage> pop_for(std::chrono::milliseconds timeout);
    std::vector<RecordedMessage> pop_batch(std::size_t maximum);
    void close();
    bool closed() const noexcept;
    QueueState state() const noexcept;

private:
    class Storage;

    static std::unique_ptr<Storage>
    create_storage(std::size_t max_messages, std::size_t max_bytes, std::size_t max_producers);

    bool reserve(std::size_t payload_bytes) noexcept;
    void release(std::size_t payload_bytes) noexcept;
    void finish_push() noexcept;

    std::unique_ptr<Storage> storage_;
    const std::size_t max_messages_;
    const std::size_t max_bytes_;
    std::atomic<std::size_t> used_messages_{0};
    std::atomic<std::size_t> used_bytes_{0};
    std::atomic<std::size_t> active_pushes_{0};
    std::atomic<bool> closed_{false};
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_MESSAGE_QUEUE_HPP_
