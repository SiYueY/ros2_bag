#include "ros2_bag/recorder/message_pool.hpp"

#include <cassert>
#include <stdexcept>

namespace ros2_bag::recorder {

MessagePool::Node::Node(std::size_t index) : index(index) {}

MessagePool::MessagePool(std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("MessagePool capacity must be positive");
    }
    nodes_.reserve(capacity);
    free_nodes_.reserve(capacity);
    for (std::size_t index = 0; index < capacity; ++index) {
        nodes_.push_back(std::make_unique<Node>(index));
        free_nodes_.push_back(capacity - index - 1);
    }
}

MessagePool::~MessagePool() {
    std::lock_guard<std::mutex> lock(mutex_);
    assert(in_use_ == 0);
    assert(active_operations_ == 0);
}

std::shared_ptr<rclcpp::SerializedMessage> MessagePool::acquire() {
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++active_operations_;
        if (lifecycle_ != Lifecycle::Running || free_nodes_.empty()) {
            ++allocation_failures_;
            --active_operations_;
            update_stopped_locked();
            return {};
        }
        index = free_nodes_.back();
        free_nodes_.pop_back();
        auto& node = *nodes_.at(index);
        assert(!node.in_use);
        node.in_use = true;
        ++in_use_;
        --active_operations_;
    }

    // shared_ptr invokes the supplied deleter if control-block allocation
    // throws. Releasing again in a catch would double-return the pool node.
    return std::shared_ptr<rclcpp::SerializedMessage>(
        std::addressof(nodes_.at(index)->message),
        [this, index](rclcpp::SerializedMessage*) { release(index); });
}

void MessagePool::begin_stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ == Lifecycle::Running) {
        lifecycle_ = Lifecycle::Stopping;
    }
    update_stopped_locked();
}

bool MessagePool::wait_stopped_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return stopped_condition_.wait_for(
        lock, timeout, [this] { return lifecycle_ == Lifecycle::Stopped; });
}

PoolState MessagePool::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PoolState{nodes_.size(), in_use_, allocation_failures_};
}

void MessagePool::release(std::size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= nodes_.size()) {
        std::terminate();
    }
    auto& node = *nodes_[index];
    if (!node.in_use) {
        std::terminate();
    }
    node.in_use = false;
    node.message.get_rcl_serialized_message().buffer_length = 0;
    free_nodes_.push_back(index);
    --in_use_;
    update_stopped_locked();
}

void MessagePool::update_stopped_locked() {
    if (lifecycle_ == Lifecycle::Stopping && in_use_ == 0 && active_operations_ == 0 &&
        free_nodes_.size() == nodes_.size()) {
        lifecycle_ = Lifecycle::Stopped;
        stopped_condition_.notify_all();
    }
}

}  // namespace ros2_bag::recorder
