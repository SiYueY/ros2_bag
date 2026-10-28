#include "ros2_bag/recorder/topic.hpp"

#include <cmath>
#include <array>
#include <utility>

namespace ros2_bag::recorder {
namespace {

Error invalid(std::string message) {
    return {ErrorCode::UnsupportedQos, Effects::None, std::move(message)};
}

bool same_qos(const QosOptions& left, const QosOptions& right) noexcept {
    return left.reliability == right.reliability && left.durability == right.durability &&
           left.history == right.history && left.depth == right.depth;
}

QosOptions runtime_default_qos() {
    QosOptions qos;
    qos.mode = QosMode::Explicit;
    qos.reliability = Reliability::Reliable;
    qos.durability = Durability::Volatile;
    qos.history = History::KeepLast;
    qos.depth = 10;
    return qos;
}

const std::array<QosOptions, 8>& auto_candidates() {
    static const auto candidates = [] {
        std::array<QosOptions, 8> values{};
        std::size_t index = 0;
        for (const auto reliability : {Reliability::Reliable, Reliability::BestEffort}) {
            for (const auto durability : {Durability::TransientLocal, Durability::Volatile}) {
                for (const auto history : {History::KeepLast, History::KeepAll}) {
                    QosOptions qos;
                    qos.mode = QosMode::Explicit;
                    qos.reliability = reliability;
                    qos.durability = durability;
                    qos.history = history;
                    if (history == History::KeepLast) {
                        qos.depth = 10;
                    }
                    values[index++] = std::move(qos);
                }
            }
        }
        return values;
    }();
    return candidates;
}

}  // namespace

Result<QosOptions, Error> normalize_qos(const std::optional<QosOptions>& options) {
    if (!options || options->mode == QosMode::Auto) {
        if (options &&
            (options->reliability || options->durability || options->history || options->depth)) {
            return Result<QosOptions, Error>(
                unexpect,
                invalid("auto QoS cannot include explicit fields"));
        }
        return Result<QosOptions, Error>(runtime_default_qos());
    }
    const auto& qos = *options;
    if (!qos.reliability || !qos.durability || !qos.history) {
        return Result<QosOptions, Error>(
            unexpect,
            invalid("explicit QoS requires reliability, durability and history"));
    }
    if (*qos.history == History::KeepLast &&
        (!qos.depth || *qos.depth == 0 || *qos.depth > 10000)) {
        return Result<QosOptions, Error>(
            unexpect,
            invalid("keep-last QoS requires depth in [1, 10000]"));
    }
    if (*qos.history == History::KeepAll && qos.depth) {
        return Result<QosOptions, Error>(unexpect, invalid("keep-all QoS cannot include depth"));
    }
    return Result<QosOptions, Error>(qos);
}

bool qos_compatible(const QosOptions& publisher, const QosOptions& subscription) noexcept {
    if (!publisher.reliability || !publisher.durability || !publisher.history ||
        !subscription.reliability || !subscription.durability || !subscription.history) {
        return false;
    }
    const auto reliability_compatible = *publisher.reliability == Reliability::Reliable ||
                                        *subscription.reliability == Reliability::BestEffort;
    const auto durability_compatible = *publisher.durability == Durability::TransientLocal ||
                                       *subscription.durability == Durability::Volatile;
    return reliability_compatible && durability_compatible;
}

Result<QosSelection, Error> select_qos(
    const std::optional<QosOptions>& options,
    const std::vector<QosOptions>& publisher_profiles,
    std::size_t publisher_count,
    bool publisher_profiles_truncated) {
    const auto normalized = normalize_qos(options);
    if (!normalized) {
        return Result<QosSelection, Error>(unexpect, normalized.error());
    }
    const auto compatible_count = [&publisher_profiles](const QosOptions& candidate) {
        std::size_t count = 0;
        for (const auto& publisher : publisher_profiles) {
            if (qos_compatible(publisher, candidate)) {
                ++count;
            }
        }
        return count;
    };
    if (options && options->mode == QosMode::Explicit) {
        const auto compatible = compatible_count(normalized.value());
        return Result<QosSelection, Error>(QosSelection{
            normalized.value(),
            compatible,
            !publisher_profiles_truncated && publisher_profiles.size() == publisher_count &&
                compatible == publisher_count});
    }

    const auto runtime_default = runtime_default_qos();
    QosOptions selected = auto_candidates().front();
    std::size_t selected_count = 0;
    bool selected_is_runtime_default = false;
    for (const auto& candidate : auto_candidates()) {
        const auto compatible = compatible_count(candidate);
        const auto is_runtime_default = same_qos(candidate, runtime_default);
        if (compatible > selected_count ||
            (compatible == selected_count && is_runtime_default && !selected_is_runtime_default)) {
            selected = candidate;
            selected_count = compatible;
            selected_is_runtime_default = is_runtime_default;
        }
    }
    return Result<QosSelection, Error>(QosSelection{
        selected,
        selected_count,
        !publisher_profiles_truncated && publisher_profiles.size() == publisher_count &&
            selected_count == publisher_count});
}

Result<void, Error> validate_topic_options(const TopicOptions& options) {
    if (options.name.empty() || options.name.front() != '/' || options.type.empty()) {
        return Result<void, Error>(
            unexpect,
            Error{
                ErrorCode::InvalidArgument,
                Effects::None,
                "topic name must be absolute and type must not be empty"});
    }
    if (options.expected_hz &&
        (!std::isfinite(*options.expected_hz) || *options.expected_hz <= 0.0)) {
        return Result<void, Error>(
            unexpect,
            Error{
                ErrorCode::InvalidArgument,
                Effects::None,
                "expected_hz must be finite and positive"});
    }
    const auto qos = normalize_qos(options.qos);
    if (!qos) return Result<void, Error>(unexpect, qos.error());
    return Result<void, Error>();
}

}  // namespace ros2_bag::recorder
