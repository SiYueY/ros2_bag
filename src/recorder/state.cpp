#include "ros2_bag/recorder/state.hpp"

namespace ros2_bag::recorder {

bool PauseState::paused() const noexcept { return user_paused || disk != DiskPauseReason::None; }

}  // namespace ros2_bag::recorder
