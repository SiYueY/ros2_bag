// TopicManager implementation. Writer calls are delegated to coordinator callbacks.
#include "ros2_bag/topic_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <cmath>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

#include <rmw/types.h>

namespace ros2_bag {
namespace {

const char* reliability_name(Reliability reliability) {
    return reliability == Reliability::kReliable ? "reliable" : "best_effort";
}

const char* durability_name(Durability durability) {
    return durability == Durability::kTransientLocal ? "transient_local" : "volatile";
}

const char* history_name(History history) {
    return history == History::kKeepAll ? "keep_all" : "keep_last";
}

bool qos_equal(const TopicQos& left, const TopicQos& right) {
    return left.reliability == right.reliability && left.durability == right.durability &&
           left.history == right.history && left.depth == right.depth;
}

std::string qos_diagnostic_details(const TopicQos& qos, const char* selection) {
    return std::string("{\"selection\":\"") + selection + "\",\"reliability\":\"" +
           reliability_name(qos.reliability) + "\",\"durability\":\"" +
           durability_name(qos.durability) + "\",\"history\":\"" + history_name(qos.history) +
           "\",\"depth\":" + std::to_string(qos.depth) + "}";
}

std::string incompatible_qos_details(const rclcpp::QOSRequestedIncompatibleQoSInfo& info) {
    return "{\"kind\":\"REQUESTED_INCOMPATIBLE_QOS\",\"total_count\":" +
           std::to_string(info.total_count) +
           ",\"total_count_change\":" + std::to_string(info.total_count_change) +
           ",\"last_policy_kind\":" + std::to_string(info.last_policy_kind) + "}";
}

}  // namespace

struct TopicManager::TopicRuntime {
    struct FrequencyBucket {
        std::int64_t slot = -1;
        std::uint64_t count = 0;
    };

    std::shared_ptr<const TopicInfo> topic_info;
    std::uint64_t topic_epoch = 0;
    mutable std::mutex state_mutex;
    std::atomic<TopicRuntimeState> state{TopicRuntimeState::kAdding};
    TaskIngress ingress;
    rclcpp::CallbackGroup::SharedPtr callback_group;
    rclcpp::GenericSubscription::SharedPtr subscription;
    std::unique_ptr<ProducerToken> producer_token;
    std::atomic<bool> ingress_open{false};
    std::atomic<std::size_t> producers_in_flight{0};
    mutable std::mutex drain_mutex;
    std::condition_variable drain_cv;
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> enqueued{0};
    std::atomic<std::uint64_t> dropped_capacity{0};
    std::atomic<std::uint64_t> dropped_enqueue_failure{0};
    std::atomic<std::uint64_t> dropped_allocation_failure{0};
    std::atomic<std::uint64_t> dropped_paused{0};
    std::atomic<std::uint64_t> dropped_stale_generation{0};
    mutable std::mutex frequency_mutex;
    std::vector<FrequencyBucket> frequency_buckets;
    std::int64_t first_frequency_slot = -1;
    std::int64_t last_evaluated_frequency_slot = -1;
    std::size_t consecutive_low_frequency_windows = 0;
    std::string last_operation_error;
    std::string operation_id;

    void set_last_operation_error(const std::string& error) {
        const std::lock_guard<std::mutex> lock(state_mutex);
        last_operation_error = error;
    }

    std::string last_error() const {
        const std::lock_guard<std::mutex> lock(state_mutex);
        return last_operation_error;
    }

    void set_subscription(rclcpp::GenericSubscription::SharedPtr value) {
        const std::lock_guard<std::mutex> lock(state_mutex);
        subscription = std::move(value);
    }

    void clear_subscription() {
        const std::lock_guard<std::mutex> lock(state_mutex);
        subscription.reset();
    }

    std::size_t publisher_count() const {
        const std::lock_guard<std::mutex> lock(state_mutex);
        return subscription ? subscription->get_publisher_count() : 0U;
    }
};

struct TopicManager::ExecutorCall {
    std::promise<TopicOperationResult> completion;
    rclcpp::TimerBase::SharedPtr timer;
};

TopicManager::TopicManager(
    rclcpp::Node::SharedPtr node,
    std::size_t max_active_topics,
    std::size_t max_distinct_topics,
    std::size_t recent_removed_instances,
    std::chrono::milliseconds producer_drain_timeout,
    double frequency_window_sec,
    std::size_t frequency_bucket_ms,
    RegisterTopic register_topic,
    PersistEvent persist_event)
: node_(std::move(node)),
  max_active_topics_(max_active_topics),
  max_distinct_topics_(max_distinct_topics),
  recent_removed_instances_(recent_removed_instances),
  producer_drain_timeout_(producer_drain_timeout),
  frequency_window_sec_(frequency_window_sec),
  frequency_bucket_ms_(frequency_bucket_ms),
  frequency_window_slots_(static_cast<std::size_t>(
      std::ceil(frequency_window_sec * 1000.0 / static_cast<double>(frequency_bucket_ms)) + 2.0)),
  register_topic_(std::move(register_topic)),
  persist_event_(std::move(persist_event)) {
    if (!node_ || max_active_topics_ == 0 || max_distinct_topics_ == 0 ||
        recent_removed_instances_ == 0 || !register_topic_ || !persist_event_ ||
        frequency_window_sec_ < 0.1 || frequency_bucket_ms_ == 0 ||
        frequency_window_slots_ > 1000) {
        throw std::invalid_argument("TopicManager requires valid dependencies");
    }
}

void TopicManager::set_task_ingress(TaskIngress ingress) {
    if (!ingress.queue || !ingress.budget || !ingress.pool_state || !ingress.open ||
        ingress.recording_generation == 0) {
        throw std::invalid_argument("TopicManager task ingress is incomplete");
    }
    const std::lock_guard<std::mutex> lock(manager_mutex_);
    if (!topics_.empty()) {
        throw std::logic_error("clear TopicManager task before replacing ingress");
    }
    ingress_ = std::move(ingress);
    next_topic_id_ = 1;
    distinct_topics_ = 0;
    historical_ids_.clear();
    historical_epochs_.clear();
    removed_topics_.clear();
    removed_total_ = 0;
}

rclcpp::QoS TopicManager::make_qos(const TopicQos& qos) {
    rclcpp::QoS ros_qos(qos.depth);
    if (qos.reliability == Reliability::kReliable) {
        ros_qos.reliable();
    } else {
        ros_qos.best_effort();
    }
    if (qos.durability == Durability::kTransientLocal) {
        ros_qos.transient_local();
    } else {
        ros_qos.durability_volatile();
    }
    if (qos.history == History::kKeepAll) {
        ros_qos.keep_all();
    } else {
        ros_qos.keep_last(qos.depth);
    }
    return ros_qos;
}

TopicQos TopicManager::adapt_qos(const TopicInfo& topic_info) const {
    if (topic_info.qos_is_explicit) {
        return topic_info.qos;
    }
    const std::vector<rclcpp::TopicEndpointInfo> publishers =
        node_->get_publishers_info_by_topic(topic_info.name);
    if (publishers.empty()) {
        return topic_info.qos;
    }
    bool all_reliable = true;
    bool all_transient_local = true;
    for (const rclcpp::TopicEndpointInfo& publisher : publishers) {
        const rmw_qos_profile_t& qos = publisher.qos_profile().get_rmw_qos_profile();
        all_reliable = all_reliable && qos.reliability == RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        all_transient_local =
            all_transient_local && qos.durability == RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
    }
    TopicQos adapted = topic_info.qos;
    adapted.reliability = all_reliable ? Reliability::kReliable : Reliability::kBestEffort;
    adapted.durability = all_transient_local ? Durability::kTransientLocal : Durability::kVolatile;
    return adapted;
}

bool TopicManager::persist(const TopicEvent& event, std::string* error) {
    return persist_event_(event, error);
}

void TopicManager::record_arrival(const std::shared_ptr<TopicRuntime>& runtime) {
    const std::int64_t slot = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count() /
                              static_cast<std::int64_t>(frequency_bucket_ms_);
    const std::size_t index = static_cast<std::size_t>(slot) % frequency_window_slots_;
    const std::lock_guard<std::mutex> lock(runtime->frequency_mutex);
    TopicRuntime::FrequencyBucket& bucket = runtime->frequency_buckets[index];
    if (bucket.slot != slot) {
        bucket.slot = slot;
        bucket.count = 0;
    }
    ++bucket.count;
    if (runtime->first_frequency_slot < 0) {
        runtime->first_frequency_slot = slot;
    }
}

void TopicManager::update_frequency_snapshot(
    const std::shared_ptr<TopicRuntime>& runtime,
    TopicSnapshot* snapshot) const {
    if (snapshot == nullptr || runtime->state.load() != TopicRuntimeState::kActive) {
        return;
    }
    const std::int64_t current_slot = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count() /
                                      static_cast<std::int64_t>(frequency_bucket_ms_);
    const std::int64_t complete_window_slots = static_cast<std::int64_t>(
        std::ceil(frequency_window_sec_ * 1000.0 / static_cast<double>(frequency_bucket_ms_)));
    const std::lock_guard<std::mutex> lock(runtime->frequency_mutex);
    if (runtime->first_frequency_slot < 0 ||
        current_slot - runtime->first_frequency_slot < complete_window_slots) {
        return;
    }
    std::uint64_t count = 0;
    const std::int64_t earliest_slot = current_slot - complete_window_slots + 1;
    for (const TopicRuntime::FrequencyBucket& bucket : runtime->frequency_buckets) {
        if (bucket.slot >= earliest_slot && bucket.slot <= current_slot) {
            count += bucket.count;
        }
    }
    snapshot->actual_hz = static_cast<double>(count) / frequency_window_sec_;
    if (!runtime->topic_info->expected_hz) {
        return;
    }
    if (runtime->last_evaluated_frequency_slot != current_slot) {
        const bool low = *snapshot->actual_hz < *runtime->topic_info->expected_hz * 0.5;
        runtime->consecutive_low_frequency_windows =
            low ? runtime->consecutive_low_frequency_windows + 1 : 0;
        runtime->last_evaluated_frequency_slot = current_slot;
    }
    snapshot->gap = runtime->consecutive_low_frequency_windows >= 2;
}

TopicOperationResult
TopicManager::add_topic(TopicInfo topic_info, const std::string& operation_id, bool armed) {
    const auto call = std::make_shared<ExecutorCall>();
    std::future<TopicOperationResult> completion = call->completion.get_future();
    const std::weak_ptr<ExecutorCall> weak_call = call;
    const auto topic = std::make_shared<TopicInfo>(std::move(topic_info));
    const std::function<void()> callback = [this, weak_call, topic, operation_id, armed]() {
        const std::shared_ptr<ExecutorCall> active_call = weak_call.lock();
        if (!active_call) {
            return;
        }
        try {
            active_call->completion.set_value(add_topic_on_executor(*topic, operation_id, armed));
        } catch (...) {
            active_call->completion.set_exception(std::current_exception());
        }
        active_call->timer->cancel();
    };
    call->timer = node_->create_wall_timer(std::chrono::milliseconds(1), callback);
    if (completion.wait_for(producer_drain_timeout_ + std::chrono::seconds(5)) !=
        std::future_status::ready) {
        call->timer->cancel();
        return {false, kInvalidTopicId, 0, "TIMEOUT", "executor did not run topic operation"};
    }
    return completion.get();
}

TopicOperationResult TopicManager::add_topic_on_executor(
    TopicInfo topic_info,
    const std::string& operation_id,
    bool armed) {
    TopicOperationResult result;
    std::shared_ptr<TopicRuntime> runtime = std::make_shared<TopicRuntime>();
    const TopicQos configured_qos = topic_info.qos;
    const char* qos_selection = "explicit";
    if (!topic_info.qos_is_explicit) {
        topic_info.qos = adapt_qos(topic_info);
        qos_selection = qos_equal(configured_qos, topic_info.qos) ? "default" : "publisher_adapted";
    }
    {
        const std::lock_guard<std::mutex> lock(manager_mutex_);
        if (!ingress_.queue || topic_info.name.empty() || topic_info.type.empty() ||
            topics_.count(topic_info.name) != 0U) {
            result.error_code = "INVALID_ARGUMENT";
            result.error_message = "topic is incomplete or already exists";
            return result;
        }
        if (topics_.size() >= max_active_topics_) {
            result.error_code = "RESOURCE_EXHAUSTED";
            result.error_message = "max_active_topics reached";
            return result;
        }
        const auto existing_id = historical_ids_.find(topic_info.name);
        if (existing_id == historical_ids_.end()) {
            if (distinct_topics_ >= max_distinct_topics_) {
                result.error_code = "RESOURCE_EXHAUSTED";
                result.error_message = "max_distinct_topics reached";
                return result;
            }
            topic_info.id = next_topic_id_++;
            historical_ids_[topic_info.name] = topic_info.id;
            historical_epochs_[topic_info.name] = 0;
            ++distinct_topics_;
        } else {
            topic_info.id = existing_id->second;
        }
        const std::uint64_t epoch = ++historical_epochs_[topic_info.name];
        runtime->topic_info = std::make_shared<const TopicInfo>(std::move(topic_info));
        runtime->topic_epoch = epoch;
        runtime->operation_id = operation_id;
        runtime->frequency_buckets.resize(frequency_window_slots_);
        runtime->ingress = ingress_;
        runtime->state.store(TopicRuntimeState::kAdding);
        topics_[runtime->topic_info->name] = runtime;
        result.topic_id = runtime->topic_info->id;
        result.topic_epoch = epoch;
    }

    std::string error;
    TopicEvent event;
    event.event = "TOPIC_ADD_PREPARED";
    event.operation_id = operation_id;
    event.topic_id = runtime->topic_info->id;
    event.topic_epoch = runtime->topic_epoch;
    event.name = runtime->topic_info->name;
    event.type = runtime->topic_info->type;
    if (!persist(event, &error)) {
        runtime->state.store(TopicRuntimeState::kFailed);
        runtime->set_last_operation_error(error);
        result.error_code = "STORAGE_UNAVAILABLE";
        result.error_message = error;
        return result;
    }
    TopicEvent diagnostic = event;
    diagnostic.event = "QOS_DIAGNOSTIC";
    diagnostic.details_json = qos_diagnostic_details(runtime->topic_info->qos, qos_selection);
    if (!persist(diagnostic, &error)) {
        runtime->state.store(TopicRuntimeState::kFailed);
        runtime->set_last_operation_error(error);
        result.error_code = "STORAGE_UNAVAILABLE";
        result.error_message = error;
        return result;
    }
    if (!register_topic_(*runtime->topic_info, &error)) {
        runtime->state.store(TopicRuntimeState::kFailed);
        runtime->set_last_operation_error(error);
        event.event = "TOPIC_ADD_FAILED";
        event.error_code = "WRITER_ERROR";
        event.error_message = error;
        static_cast<void>(persist(event, &error));
        result.error_code = "WRITER_ERROR";
        result.error_message = runtime->last_error();
        return result;
    }
    try {
        runtime->producer_token = std::make_unique<ProducerToken>(runtime->ingress.queue.get());
        runtime->callback_group =
            node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
        rclcpp::SubscriptionOptions options;
        options.callback_group = runtime->callback_group;
        options.event_callbacks.incompatible_qos_callback =
            [this, runtime](rclcpp::QOSRequestedIncompatibleQoSInfo& info) {
                TopicEvent event;
                event.event = "QOS_DIAGNOSTIC";
                event.operation_id = runtime->operation_id;
                event.topic_id = runtime->topic_info->id;
                event.topic_epoch = runtime->topic_epoch;
                event.name = runtime->topic_info->name;
                event.type = runtime->topic_info->type;
                event.details_json = incompatible_qos_details(info);
                std::string persistence_error;
                if (!persist(event, &persistence_error)) {
                    runtime->set_last_operation_error(persistence_error);
                }
            };
        const auto subscription_callback =
            [this, runtime](std::shared_ptr<rclcpp::SerializedMessage> serialized) {
                process_message(runtime, std::move(serialized));
            };
        try {
            runtime->set_subscription(node_->create_generic_subscription(
                runtime->topic_info->name,
                runtime->topic_info->type,
                make_qos(runtime->topic_info->qos),
                subscription_callback,
                options));
        } catch (const rclcpp::UnsupportedEventTypeException&) {
            // Some RMW implementations do not expose this event. The
            // subscription remains valid, and its initial QoS selection is
            // already durable in the preceding QOS_DIAGNOSTIC event.
            options.event_callbacks.incompatible_qos_callback = nullptr;
            runtime->set_subscription(node_->create_generic_subscription(
                runtime->topic_info->name,
                runtime->topic_info->type,
                make_qos(runtime->topic_info->qos),
                subscription_callback,
                options));
        }
        // Wake an executor that may currently be blocked in rmw_wait before it
        // rebuilds its wait set for this dynamically added callback group.
        runtime->callback_group->trigger_notify_guard_condition();
    } catch (const std::exception& exception) {
        runtime->state.store(TopicRuntimeState::kFailed);
        runtime->set_last_operation_error(exception.what());
        event.event = "TOPIC_ADD_FAILED";
        event.error_code = "SUBSCRIPTION_ERROR";
        event.error_message = runtime->last_error();
        static_cast<void>(persist(event, &error));
        result.error_code = "SUBSCRIPTION_ERROR";
        result.error_message = runtime->last_error();
        return result;
    }
    runtime->ingress_open.store(!armed);
    runtime->state.store(armed ? TopicRuntimeState::kArmed : TopicRuntimeState::kActive);
    event.event = "TOPIC_ADDED";
    if (!persist(event, &error)) {
        runtime->ingress_open.store(false);
        runtime->state.store(TopicRuntimeState::kFailed);
        runtime->set_last_operation_error(error);
        result.error_code = "STORAGE_UNAVAILABLE";
        result.error_message = error;
        return result;
    }
    result.ok = true;
    return result;
}

void TopicManager::process_message(
    const std::shared_ptr<TopicRuntime>& runtime,
    std::shared_ptr<rclcpp::SerializedMessage> serialized) {
    runtime->producers_in_flight.fetch_add(1);
    struct ProducerExit {
        std::shared_ptr<TopicRuntime> runtime;
        ~ProducerExit() {
            runtime->producers_in_flight.fetch_sub(1);
            runtime->drain_cv.notify_all();
        }
    } exit{runtime};
    if (!runtime->ingress_open.load() || !runtime->ingress.open->load()) {
        runtime->dropped_paused.fetch_add(1);
        return;
    }
    if (!serialized) {
        runtime->dropped_allocation_failure.fetch_add(1);
        return;
    }
    runtime->received.fetch_add(1);
    record_arrival(runtime);
    const std::size_t bytes = serialized->get_rcl_serialized_message().buffer_length;
    BudgetLease lease = runtime->ingress.budget->try_acquire(bytes);
    if (!lease.valid()) {
        runtime->dropped_capacity.fetch_add(1);
        return;
    }
    MessageHandle message = runtime->ingress.pool_state->acquire();
    if (!message) {
        runtime->dropped_allocation_failure.fetch_add(1);
        return;
    }
    message->recording_generation = runtime->ingress.recording_generation;
    message->topic_id = runtime->topic_info->id;
    message->topic_epoch = runtime->topic_epoch;
    message->topic_info = runtime->topic_info;
    message->serialized_message = std::move(serialized);
    message->payload_bytes = bytes;
    message->recv_timestamp_ns = node_->now().nanoseconds();
    message->send_timestamp_ns = message->recv_timestamp_ns;
    message->timestamp_source = TimestampSource::kReceive;
    QueuedMessage queued{std::move(message), std::move(lease)};
    if (!runtime->producer_token->try_enqueue(&queued)) {
        runtime->dropped_enqueue_failure.fetch_add(1);
        return;
    }
    runtime->enqueued.fetch_add(1);
}

TopicOperationResult TopicManager::remove_topic(
    const std::string& name,
    const std::string& operation_id) {
    const auto call = std::make_shared<ExecutorCall>();
    std::future<TopicOperationResult> completion = call->completion.get_future();
    const std::weak_ptr<ExecutorCall> weak_call = call;
    call->timer = node_->create_wall_timer(
        std::chrono::milliseconds(1),
        [this, weak_call, name, operation_id]() {
            const std::shared_ptr<ExecutorCall> active_call = weak_call.lock();
            if (!active_call) {
                return;
            }
            try {
                active_call->completion.set_value(remove_topic_on_executor(name, operation_id));
            } catch (...) {
                active_call->completion.set_exception(std::current_exception());
            }
            active_call->timer->cancel();
        });
    if (completion.wait_for(producer_drain_timeout_ + std::chrono::seconds(5)) !=
        std::future_status::ready) {
        call->timer->cancel();
        return {false, kInvalidTopicId, 0, "TIMEOUT", "executor did not run topic operation"};
    }
    return completion.get();
}

TopicOperationResult TopicManager::remove_topic_on_executor(
    const std::string& name,
    const std::string& operation_id) {
    TopicOperationResult result;
    std::shared_ptr<TopicRuntime> runtime;
    {
        const std::lock_guard<std::mutex> lock(manager_mutex_);
        const auto found = topics_.find(name);
        if (found == topics_.end()) {
            result.error_code = "NOT_FOUND";
            result.error_message = "topic is not active";
            return result;
        }
        runtime = found->second;
        runtime->state.store(TopicRuntimeState::kRemoving);
        runtime->ingress_open.store(false);
        result.topic_id = runtime->topic_info->id;
        result.topic_epoch = runtime->topic_epoch;
    }
    std::string error;
    TopicEvent event;
    event.event = "TOPIC_REMOVE_PREPARED";
    event.operation_id = operation_id;
    event.topic_id = runtime->topic_info->id;
    event.topic_epoch = runtime->topic_epoch;
    event.name = runtime->topic_info->name;
    event.type = runtime->topic_info->type;
    if (!persist(event, &error)) {
        runtime->state.store(TopicRuntimeState::kActive);
        runtime->ingress_open.store(true);
        runtime->set_last_operation_error(error);
        result.error_code = "STORAGE_UNAVAILABLE";
        result.error_message = error;
        return result;
    }
    {
        std::unique_lock<std::mutex> lock(runtime->drain_mutex);
        if (!runtime->drain_cv.wait_for(lock, producer_drain_timeout_, [&runtime]() {
                return runtime->producers_in_flight.load() == 0;
            })) {
            runtime->state.store(TopicRuntimeState::kFailed);
            runtime->set_last_operation_error("producer drain timed out");
            event.event = "TOPIC_REMOVE_FAILED";
            event.error_code = "TIMEOUT";
            event.error_message = runtime->last_error();
            static_cast<void>(persist(event, &error));
            result.error_code = "TIMEOUT";
            result.error_message = runtime->last_error();
            return result;
        }
    }
    // Subscription lifetime may change from another Recorder control thread while an
    // executor is blocked in rmw_wait. Notify first so it rebuilds the wait
    // set before the entity's final shared owner is released.
    if (runtime->callback_group) {
        runtime->callback_group->trigger_notify_guard_condition();
    }
    node_->get_node_base_interface()->get_notify_guard_condition().trigger();
    runtime->clear_subscription();
    runtime->producer_token.reset();
    event.event = "TOPIC_REMOVED";
    if (!persist(event, &error)) {
        runtime->state.store(TopicRuntimeState::kFailed);
        runtime->set_last_operation_error(error);
        result.error_code = "STORAGE_UNAVAILABLE";
        result.error_message = error;
        return result;
    }
    runtime->state.store(TopicRuntimeState::kRemoved);
    {
        const std::lock_guard<std::mutex> lock(manager_mutex_);
        topics_.erase(name);
        RemovedTopicSnapshot removed;
        removed.topic_info = *runtime->topic_info;
        removed.topic_epoch = runtime->topic_epoch;
        removed.removed_at_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        removed.final_state = "REMOVED";
        removed_topics_.push_back(std::move(removed));
        ++removed_total_;
        if (removed_topics_.size() > recent_removed_instances_) {
            removed_topics_.erase(removed_topics_.begin());
        }
    }
    result.ok = true;
    return result;
}

void TopicManager::set_ingress_open(bool open) {
    std::vector<std::shared_ptr<TopicRuntime>> runtimes;
    {
        const std::lock_guard<std::mutex> lock(manager_mutex_);
        if (ingress_.open) {
            ingress_.open->store(open);
        }
        for (const auto& item : topics_) {
            runtimes.push_back(item.second);
        }
    }
    for (const auto& runtime : runtimes) {
        if (runtime->state.load() == TopicRuntimeState::kActive ||
            runtime->state.load() == TopicRuntimeState::kArmed) {
            runtime->ingress_open.store(open);
            runtime->state.store(open ? TopicRuntimeState::kActive : TopicRuntimeState::kArmed);
        }
    }
}

bool TopicManager::close_ingress_and_drain() {
    std::vector<std::shared_ptr<TopicRuntime>> runtimes;
    {
        const std::lock_guard<std::mutex> lock(manager_mutex_);
        if (ingress_.open) {
            ingress_.open->store(false);
        }
        for (const auto& item : topics_) {
            item.second->ingress_open.store(false);
            runtimes.push_back(item.second);
        }
    }
    for (const std::shared_ptr<TopicRuntime>& runtime : runtimes) {
        std::unique_lock<std::mutex> lock(runtime->drain_mutex);
        if (!runtime->drain_cv.wait_for(lock, producer_drain_timeout_, [&runtime]() {
                return runtime->producers_in_flight.load() == 0;
            })) {
            return false;
        }
    }
    return true;
}

void TopicManager::clear_task() {
    struct ClearTaskCall {
        std::promise<void> completion;
        rclcpp::TimerBase::SharedPtr timer;
    };
    const auto call = std::make_shared<ClearTaskCall>();
    std::future<void> completion = call->completion.get_future();
    const std::weak_ptr<ClearTaskCall> weak_call = call;
    call->timer = node_->create_wall_timer(std::chrono::milliseconds(1), [this, weak_call]() {
        const std::shared_ptr<ClearTaskCall> active_call = weak_call.lock();
        if (!active_call) {
            return;
        }
        try {
            clear_task_on_executor();
            active_call->completion.set_value();
        } catch (...) {
            active_call->completion.set_exception(std::current_exception());
        }
        active_call->timer->cancel();
    });
    if (completion.wait_for(producer_drain_timeout_ + std::chrono::seconds(5)) !=
        std::future_status::ready) {
        call->timer->cancel();
        return;
    }
    completion.get();
}

void TopicManager::clear_task_on_executor() {
    std::vector<std::string> names;
    {
        const std::lock_guard<std::mutex> lock(manager_mutex_);
        for (const auto& item : topics_) {
            names.push_back(item.first);
        }
    }
    set_ingress_open(false);
    for (const std::string& name : names) {
        static_cast<void>(remove_topic_on_executor(name, "shutdown"));
    }
    const std::lock_guard<std::mutex> lock(manager_mutex_);
    topics_.clear();
    ingress_ = TaskIngress{};
}

std::vector<TopicSnapshot> TopicManager::snapshots() const {
    std::vector<std::shared_ptr<TopicRuntime>> runtimes;
    {
        const std::lock_guard<std::mutex> lock(manager_mutex_);
        for (const auto& item : topics_) {
            runtimes.push_back(item.second);
        }
    }
    std::vector<TopicSnapshot> results;
    results.reserve(runtimes.size());
    for (const auto& runtime : runtimes) {
        TopicSnapshot snapshot;
        snapshot.topic_info = *runtime->topic_info;
        snapshot.topic_epoch = runtime->topic_epoch;
        snapshot.state = runtime->state.load();
        snapshot.matched_publishers = runtime->publisher_count();
        snapshot.counters.received = runtime->received.load();
        snapshot.counters.enqueued = runtime->enqueued.load();
        snapshot.counters.dropped_capacity = runtime->dropped_capacity.load();
        snapshot.counters.dropped_enqueue_failure = runtime->dropped_enqueue_failure.load();
        snapshot.counters.dropped_allocation_failure = runtime->dropped_allocation_failure.load();
        snapshot.counters.dropped_paused = runtime->dropped_paused.load();
        snapshot.counters.dropped_stale_generation = runtime->dropped_stale_generation.load();
        snapshot.last_operation_error = runtime->last_error();
        update_frequency_snapshot(runtime, &snapshot);
        results.push_back(std::move(snapshot));
    }
    return results;
}

std::vector<RemovedTopicSnapshot> TopicManager::removed_snapshots() const {
    const std::lock_guard<std::mutex> lock(manager_mutex_);
    return removed_topics_;
}

std::uint64_t TopicManager::removed_total() const noexcept {
    const std::lock_guard<std::mutex> lock(manager_mutex_);
    return removed_total_;
}

void TopicManager::record_generation_violation(TopicId topic_id, std::uint64_t topic_epoch) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    for (const auto& item : topics_) {
        const std::shared_ptr<TopicRuntime>& runtime = item.second;
        if (runtime->topic_info->id == topic_id && runtime->topic_epoch == topic_epoch) {
            runtime->dropped_stale_generation.fetch_add(1);
            return;
        }
    }
}

}  // namespace ros2_bag
