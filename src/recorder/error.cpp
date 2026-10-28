#include "ros2_bag/recorder/error.hpp"

namespace ros2_bag::recorder {

bool operator==(const Error& left, const Error& right) {
    return left.code == right.code && left.effects == right.effects &&
           left.message == right.message;
}

bool operator!=(const Error& left, const Error& right) { return !(left == right); }

}  // namespace ros2_bag::recorder
