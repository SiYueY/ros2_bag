#ifndef ROS2_BAG_RECORDER_MESSAGE_POOL_HPP_
#define ROS2_BAG_RECORDER_MESSAGE_POOL_HPP_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <rclcpp/serialized_message.hpp>

#include "ros2_bag/recorder/state.hpp"

namespace ros2_bag::recorder {

class MessagePool {
public:
    explicit MessagePool(std::size_t capacity);
    ~MessagePool();

    MessagePool(const MessagePool&) = delete;
    MessagePool& operator=(const MessagePool&) = delete;

    std::shared_ptr<rclcpp::SerializedMessage> acquire();
    void begin_stop();
    bool wait_stopped_for(std::chrono::milliseconds timeout);
    PoolState state() const;

private:
    enum class Lifecycle { Running, Stopping, Stopped };

    struct Node {
        explicit Node(std::size_t index) : index(index) {}
        std::size_t index;
        rclcpp::SerializedMessage message;
        bool in_use = false;
    };

    void release(std::size_t index);
    void update_stopped_locked();

    mutable std::mutex mutex_;
    std::condition_variable stopped_condition_;
    Lifecycle lifecycle_ = Lifecycle::Running;
    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<std::size_t> free_nodes_;
    std::size_t in_use_ = 0;
    std::size_t active_operations_ = 0;
    std::uint64_t allocation_failures_ = 0;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_MESSAGE_POOL_HPP_
