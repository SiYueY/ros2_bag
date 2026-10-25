// Move-only MPSC recording queue and task-scoped producer token wrapper.
#ifndef ROS2_BAG_RECORDING_QUEUE_HPP_
#define ROS2_BAG_RECORDING_QUEUE_HPP_

#include <cstddef>
#include <memory>

#include <blockingconcurrentqueue.h>

#include "ros2_bag/queue_budget.hpp"
#include "ros2_bag/recorded_message_pool.hpp"

namespace ros2_bag {

struct QueuedMessage {
    MessageHandle message;
    BudgetLease budget;
};

using MessageQueue = moodycamel::BlockingConcurrentQueue<QueuedMessage>;

// Binds one producer thread to one task queue. This object is move-only and must
// be destroyed before its associated MessageQueue. It is not thread-safe.
class ProducerToken {
public:
    explicit ProducerToken(MessageQueue* queue);

    ProducerToken(const ProducerToken&) = delete;
    ProducerToken& operator=(const ProducerToken&) = delete;
    ProducerToken(ProducerToken&&) noexcept = default;
    ProducerToken& operator=(ProducerToken&&) noexcept = default;

    // Attempts a move-only enqueue. On failure, message still owns both RAII
    // objects and cleans up when it leaves scope.
    bool try_enqueue(QueuedMessage* message);

private:
    MessageQueue* queue_;
    std::unique_ptr<moodycamel::ProducerToken> token_;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_RECORDING_QUEUE_HPP_
