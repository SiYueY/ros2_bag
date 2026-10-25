// Task-scoped ProducerToken wrapper implementation.
#include "ros2_bag/recording_queue.hpp"

#include <stdexcept>
#include <utility>

namespace ros2_bag {

ProducerToken::ProducerToken(MessageQueue* queue) : queue_(queue) {
    if (queue_ == nullptr) {
        throw std::invalid_argument("ProducerToken requires a MessageQueue");
    }
    token_ = std::make_unique<moodycamel::ProducerToken>(*queue_);
}

bool ProducerToken::try_enqueue(QueuedMessage* message) {
    if (message == nullptr) {
        return false;
    }
    return queue_->try_enqueue(*token_, std::move(*message));
}

}  // namespace ros2_bag
