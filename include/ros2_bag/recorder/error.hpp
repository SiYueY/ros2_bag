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

bool operator==(const Error& left, const Error& right);
bool operator!=(const Error& left, const Error& right);

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_ERROR_HPP_
