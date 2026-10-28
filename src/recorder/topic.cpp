#include "ros2_bag/recorder/topic.hpp"

#include <cmath>

namespace ros2_bag::recorder {

Result<QosOptions, Error> normalize_qos(const QosOptions& options) {
    if (std::holds_alternative<QosOptions::Auto>(options.mode)) {
        QosOptions normalized;
        normalized.mode = QosOptions::Explicit{};
        return Result<QosOptions, Error>(std::move(normalized));
    }

    const auto& explicit_qos = std::get<QosOptions::Explicit>(options.mode);
    if (explicit_qos.history == History::KeepLast &&
        (explicit_qos.depth == 0 || explicit_qos.depth > kMaxQosDepth)) {
        return Result<QosOptions, Error>(
            unexpect,
            Error{
                ErrorCode::UnsupportedQos,
                Effects::None,
                "keep-last QoS requires depth in [1, 10000]"});
    }
    if (explicit_qos.history == History::KeepAll && explicit_qos.depth != 0) {
        return Result<QosOptions, Error>(
            unexpect,
            Error{ErrorCode::UnsupportedQos, Effects::None, "keep-all QoS cannot include depth"});
    }
    return Result<QosOptions, Error>(options);
}

bool qos_compatible(const QosOptions& publisher, const QosOptions& subscription) noexcept {
    const auto* publisher_qos = std::get_if<QosOptions::Explicit>(&publisher.mode);
    const auto* subscription_qos = std::get_if<QosOptions::Explicit>(&subscription.mode);
    if (!publisher_qos || !subscription_qos) {
        return false;
    }

    const auto reliability_compatible = publisher_qos->reliability == Reliability::Reliable ||
                                        subscription_qos->reliability == Reliability::BestEffort;
    const auto durability_compatible = publisher_qos->durability == Durability::TransientLocal ||
                                       subscription_qos->durability == Durability::Volatile;
    return reliability_compatible && durability_compatible;
}

Result<QosSelection, Error> select_qos(
    const QosOptions& options,
    const std::vector<QosOptions>& publisher_profiles,
    std::size_t publisher_count,
    bool profiles_truncated) {
    const auto normalized = normalize_qos(options);
    if (!normalized) {
        return Result<QosSelection, Error>(unexpect, normalized.error());
    }

    if (std::holds_alternative<QosOptions::Explicit>(options.mode)) {
        std::size_t compatible = 0;
        for (const auto& publisher : publisher_profiles) {
            if (qos_compatible(publisher, normalized.value())) {
                ++compatible;
            }
        }
        return Result<QosSelection, Error>(QosSelection{
            normalized.value(),
            compatible,
            !profiles_truncated && publisher_profiles.size() == publisher_count &&
                compatible == publisher_count});
    }

    QosOptions selected;
    std::size_t selected_count = 0;
    bool selected_is_runtime_default = false;

    for (const auto reliability : {Reliability::Reliable, Reliability::BestEffort}) {
        for (const auto durability : {Durability::TransientLocal, Durability::Volatile}) {
            for (const auto history : {History::KeepLast, History::KeepAll}) {
                QosOptions::Explicit explicit_qos;
                explicit_qos.reliability = reliability;
                explicit_qos.durability = durability;
                explicit_qos.history = history;
                explicit_qos.depth = 0;
                if (history == History::KeepLast) {
                    explicit_qos.depth = 10;
                }

                QosOptions candidate;
                candidate.mode = explicit_qos;

                std::size_t compatible = 0;
                for (const auto& publisher : publisher_profiles) {
                    if (qos_compatible(publisher, candidate)) {
                        ++compatible;
                    }
                }

                const bool runtime_default =
                    reliability == Reliability::Reliable && durability == Durability::Volatile &&
                    history == History::KeepLast && explicit_qos.depth == 10;
                if (compatible > selected_count) {
                    selected = std::move(candidate);
                    selected_count = compatible;
                    selected_is_runtime_default = runtime_default;
                    continue;
                }
                if (compatible == selected_count && runtime_default &&
                    !selected_is_runtime_default) {
                    selected = std::move(candidate);
                    selected_is_runtime_default = true;
                }
            }
        }
    }

    return Result<QosSelection, Error>(QosSelection{
        selected,
        selected_count,
        !profiles_truncated && publisher_profiles.size() == publisher_count &&
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
    if (!qos) {
        return Result<void, Error>(unexpect, qos.error());
    }
    return Result<void, Error>();
}

}  // namespace ros2_bag::recorder
