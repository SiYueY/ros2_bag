#include "ros2_bag/recorder/topic.hpp"

#include <cmath>
#include <utility>

namespace ros2_bag::recorder {
namespace {

Error invalid(std::string message) {
    return {ErrorCode::UnsupportedQos, Effects::None, std::move(message)};
}

}  // namespace

Result<QosOptions, Error> normalize_qos(const std::optional<QosOptions>& options) {
    if (!options || options->mode == QosMode::Auto) {
        if (options && (options->reliability || options->durability || options->history || options->depth)) {
            return Result<QosOptions, Error>(unexpect, invalid("auto QoS cannot include explicit fields"));
        }
        QosOptions automatic;
        automatic.mode = QosMode::Explicit;
        automatic.reliability = Reliability::Reliable;
        automatic.durability = Durability::Volatile;
        automatic.history = History::KeepLast;
        automatic.depth = 10;
        return Result<QosOptions, Error>(std::move(automatic));
    }
    const auto& qos = *options;
    if (!qos.reliability || !qos.durability || !qos.history) {
        return Result<QosOptions, Error>(unexpect, invalid("explicit QoS requires reliability, durability and history"));
    }
    if (*qos.history == History::KeepLast && (!qos.depth || *qos.depth == 0 || *qos.depth > 10000)) {
        return Result<QosOptions, Error>(unexpect, invalid("keep-last QoS requires depth in [1, 10000]"));
    }
    if (*qos.history == History::KeepAll && qos.depth) {
        return Result<QosOptions, Error>(unexpect, invalid("keep-all QoS cannot include depth"));
    }
    return Result<QosOptions, Error>(qos);
}

Result<void, Error> validate_topic_options(const TopicOptions& options) {
    if (options.name.empty() || options.name.front() != '/' || options.type.empty()) {
        return Result<void, Error>(
            unexpect, Error{ErrorCode::InvalidArgument, Effects::None, "topic name must be absolute and type must not be empty"});
    }
    if (options.expected_hz && (!std::isfinite(*options.expected_hz) || *options.expected_hz <= 0.0)) {
        return Result<void, Error>(
            unexpect, Error{ErrorCode::InvalidArgument, Effects::None, "expected_hz must be finite and positive"});
    }
    const auto qos = normalize_qos(options.qos);
    if (!qos) return Result<void, Error>(unexpect, qos.error());
    return Result<void, Error>();
}

}  // namespace ros2_bag::recorder
