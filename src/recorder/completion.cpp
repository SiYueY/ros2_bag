#include "ros2_bag/recorder/completion.hpp"

#include <utility>

namespace ros2_bag::recorder {

CompletionSource<void>::CompletionSource() {
    auto pair = Completion<void>::make_pair();
    completion_ = std::move(pair.first);
    producer_ = std::move(pair.second);
}

Completion<void> CompletionSource<void>::completion() const { return completion_; }

void CompletionSource<void>::set_value() const { producer_.complete(Result<void, Error>()); }

void CompletionSource<void>::set_error(Error error) const {
    producer_.complete(Result<void, Error>(unexpect, std::move(error)));
}

}  // namespace ros2_bag::recorder
