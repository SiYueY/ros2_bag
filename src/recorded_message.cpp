// RecordedMessage reset implementation. Payload destruction happens before reuse.
#include "ros2_bag/recorded_message.hpp"

namespace ros2_bag {

void reset_recorded_message(RecordedMessage* message) noexcept {
    if (message == nullptr) {
        return;
    }
    message->serialized_message.reset();
    message->topic_info.reset();
    *message = RecordedMessage{};
}

}  // namespace ros2_bag
