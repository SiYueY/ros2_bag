#ifndef ROS2_BAG_RECORDER_ERROR_HPP_
#define ROS2_BAG_RECORDER_ERROR_HPP_

#include <string>

namespace ros2_bag::recorder {

enum class ErrorCode {
    InvalidArgument,
    InvalidState,
    AlreadyPaused,
    NotFound,
    RecordingExists,
    StaleRecording,
    StaleTopicEpoch,
    TopicTypeConflict,
    UnsupportedQos,
    TypeUnavailable,
    SchemaUnavailable,
    ResourceExhausted,
    DiskCritical,
    DiskUnknown,
    StorageError,
    EventCommitFailed,
    DrainTimeout,
    Unavailable,
    InternalError,
};

enum class Effects { None, Possible, ConfirmedPartial };

struct Error {
    ErrorCode code = ErrorCode::InternalError;
    Effects effects = Effects::None;
    std::string message;
};

inline bool operator==(const Error& left, const Error& right) {
    return left.code == right.code && left.effects == right.effects && left.message == right.message;
}

inline bool operator!=(const Error& left, const Error& right) { return !(left == right); }

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_ERROR_HPP_
