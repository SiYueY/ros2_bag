#ifndef ROS2_BAG_RECORDER_MESSAGE_QUEUE_HPP_
#define ROS2_BAG_RECORDER_MESSAGE_QUEUE_HPP_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "ros2_bag/recorder/message.hpp"
#include "ros2_bag/recorder/state.hpp"

namespace ros2_bag::recorder {

class MessageQueue {
public:
    MessageQueue(std::size_t max_messages, std::size_t max_bytes);

    bool try_push(RecordedMessage message);
    std::optional<RecordedMessage> pop_for(std::chrono::milliseconds timeout);
    std::vector<RecordedMessage> pop_batch(std::size_t maximum);
    void close();
    bool closed() const;
    QueueState state() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<RecordedMessage> messages_;
    std::size_t max_messages_;
    std::size_t max_bytes_;
    std::size_t used_bytes_ = 0;
    bool closed_ = false;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_MESSAGE_QUEUE_HPP_
