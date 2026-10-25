// Formal PoolState implementation. The pool never owns serialized CDR buffers.
#include "ros2_bag/recorded_message_pool.hpp"

#include <cstdlib>
#include <new>
#include <stdexcept>
#include <utility>

namespace ros2_bag {

thread_local PoolState::CacheMap PoolState::caches_;

[[noreturn]] void fail_fast(PoolErrorCode error_code) noexcept {
    static_cast<void>(error_code);
    std::abort();
}

PoolState::LocalCache::~LocalCache() {
    pool_state->flush_local_cache(&free_indices);
    if (!counted) {
        return;
    }
    const std::lock_guard<std::mutex> lock(pool_state->lifecycle_mutex_);
    --pool_state->tls_cache_count_;
    pool_state->lifecycle_cv_.notify_all();
}

PoolState::PoolState(PoolConfig config)
: config_(config), storage_(std::make_unique<Node[]>(config.capacity)) {
    if (config_.capacity == 0 || config_.capacity >= kInvalidNodeIndex ||
        (config_.mode == PoolMode::kTls &&
         (config_.local_capacity == 0 || config_.batch_size == 0))) {
        throw std::invalid_argument("invalid RecordedMessagePool configuration");
    }
    fallback_registry_.reserve(config_.max_fallback_in_use);
    for (std::size_t index = 0; index < config_.capacity; ++index) {
        storage_[index].next_free = free_head_;
        free_head_ = &storage_[index];
    }
    global_free_ = config_.capacity;
}

bool PoolState::enter_acquire() noexcept {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (runtime_state_ != PoolRuntimeState::kRunning) {
        return false;
    }
    ++in_flight_acquires_;
    return true;
}

void PoolState::leave_acquire() noexcept {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    --in_flight_acquires_;
    lifecycle_cv_.notify_all();
}

bool PoolState::enter_release() noexcept {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (runtime_state_ == PoolRuntimeState::kStopped) {
        return false;
    }
    ++in_flight_releases_;
    return true;
}

void PoolState::leave_release() noexcept {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    --in_flight_releases_;
    lifecycle_cv_.notify_all();
}

PoolState::LocalCache* PoolState::find_or_create_local_cache() noexcept {
    try {
        const auto found = caches_.find(this);
        if (found != caches_.end()) {
            return found->second.get();
        }
        auto cache = std::make_unique<LocalCache>();
        cache->pool_state = shared_from_this();
        cache->free_indices.reserve(config_.local_capacity);
        const auto inserted = caches_.emplace(this, std::move(cache));
        inserted.first->second->counted = true;
        {
            const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            ++tls_cache_count_;
        }
        return inserted.first->second.get();
    } catch (const std::bad_alloc&) {
        allocation_failure_count_.fetch_add(1);
        return nullptr;
    }
}

MessageHandle PoolState::acquire() noexcept {
    if (!enter_acquire()) {
        return MessageHandle{};
    }
    struct AcquireExit {
        PoolState* pool_state;
        ~AcquireExit() { pool_state->leave_acquire(); }
    } exit{this};

    Node* node = nullptr;
    if (config_.mode == PoolMode::kTls) {
        LocalCache* cache = find_or_create_local_cache();
        if (cache == nullptr) {
            return MessageHandle{};
        }
        if (!cache->free_indices.empty()) {
            const NodeIndex index = cache->free_indices.back();
            cache->free_indices.pop_back();
            tls_free_.fetch_sub(1);
            tls_hits_.fetch_add(1);
            node = &storage_[index];
        } else {
            tls_misses_.fetch_add(1);
            const std::lock_guard<std::mutex> lock(free_mutex_);
            global_lock_ops_.fetch_add(1);
            while (free_head_ != nullptr && cache->free_indices.size() < config_.batch_size) {
                Node* next = free_head_;
                free_head_ = next->next_free;
                const NodeIndex index = static_cast<NodeIndex>(next - storage_.get());
                cache->free_indices.push_back(index);
                --global_free_;
                tls_free_.fetch_add(1);
            }
            if (!cache->free_indices.empty()) {
                const NodeIndex index = cache->free_indices.back();
                cache->free_indices.pop_back();
                tls_free_.fetch_sub(1);
                node = &storage_[index];
            }
        }
    } else {
        const std::lock_guard<std::mutex> lock(free_mutex_);
        global_lock_ops_.fetch_add(1);
        if (free_head_ != nullptr) {
            node = free_head_;
            free_head_ = node->next_free;
            --global_free_;
        }
    }

    if (node != nullptr) {
        if (node->borrowed.exchange(true)) {
            set_fatal_error(PoolErrorCode::kInternalInvariantViolation);
            fail_fast(PoolErrorCode::kInternalInvariantViolation);
        }
        in_use_.fetch_add(1);
        const NodeIndex index = static_cast<NodeIndex>(node - storage_.get());
        return MessageHandle(&node->message, MessageDeleter{shared_from_this(), index});
    }

    std::unique_ptr<RecordedMessage> fallback;
    try {
        fallback = std::make_unique<RecordedMessage>();
    } catch (const std::bad_alloc&) {
        allocation_failure_count_.fetch_add(1);
        return MessageHandle{};
    }

    try {
        const std::lock_guard<std::mutex> lock(fallback_mutex_);
        if (fallback_registry_.size() >= config_.max_fallback_in_use) {
            return MessageHandle{};
        }
        const auto inserted = fallback_registry_.emplace(fallback.get());
        if (!inserted.second) {
            set_fatal_error(PoolErrorCode::kInternalInvariantViolation);
            fail_fast(PoolErrorCode::kInternalInvariantViolation);
        }
        fallback_in_use_.fetch_add(1);
        fallback_count_.fetch_add(1);
    } catch (const std::bad_alloc&) {
        allocation_failure_count_.fetch_add(1);
        return MessageHandle{};
    }
    return MessageHandle(fallback.release(), MessageDeleter{shared_from_this(), kInvalidNodeIndex});
}

void PoolState::flush_local_cache(std::vector<NodeIndex>* indices) noexcept {
    if (indices == nullptr || indices->empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(free_mutex_);
    global_lock_ops_.fetch_add(1);
    for (const NodeIndex index : *indices) {
        Node& node = storage_[index];
        node.next_free = free_head_;
        free_head_ = &node;
        ++global_free_;
    }
    tls_free_.fetch_sub(indices->size());
    indices->clear();
}

void PoolState::release_thread_cache() noexcept { caches_.erase(this); }

void PoolState::set_fatal_error(PoolErrorCode error_code) noexcept {
    PoolErrorCode expected = PoolErrorCode::kNone;
    fatal_error_.compare_exchange_strong(expected, error_code);
}

void PoolState::release_message(RecordedMessage* message, NodeIndex node_index) noexcept {
    if (message == nullptr) {
        return;
    }
    if (!enter_release()) {
        set_fatal_error(PoolErrorCode::kReleaseAfterStopped);
        fail_fast(PoolErrorCode::kReleaseAfterStopped);
    }
    struct ReleaseExit {
        PoolState* pool_state;
        ~ReleaseExit() { pool_state->leave_release(); }
    } exit{this};

    if (node_index == kInvalidNodeIndex) {
        bool registered = false;
        {
            const std::lock_guard<std::mutex> lock(fallback_mutex_);
            const auto found = fallback_registry_.find(message);
            if (found != fallback_registry_.end()) {
                fallback_registry_.erase(found);
                fallback_in_use_.fetch_sub(1);
                registered = true;
            }
        }
        if (!registered) {
            set_fatal_error(PoolErrorCode::kInvalidFallbackOwnership);
            fail_fast(PoolErrorCode::kInvalidFallbackOwnership);
        }
        const std::unique_ptr<RecordedMessage> owner(message);
        reset_recorded_message(owner.get());
        return;
    }
    if (node_index >= config_.capacity) {
        set_fatal_error(PoolErrorCode::kInvalidNodeIndex);
        fail_fast(PoolErrorCode::kInvalidNodeIndex);
    }
    Node& node = storage_[node_index];
    if (&node.message != message) {
        set_fatal_error(PoolErrorCode::kInvalidNodeOwnership);
        fail_fast(PoolErrorCode::kInvalidNodeOwnership);
    }
    if (!node.borrowed.exchange(false)) {
        set_fatal_error(PoolErrorCode::kDoubleRelease);
        fail_fast(PoolErrorCode::kDoubleRelease);
    }
    reset_recorded_message(message);
    in_use_.fetch_sub(1);

    const bool cache_enabled =
        config_.mode == PoolMode::kTls && runtime_state() == PoolRuntimeState::kRunning;
    const auto found = caches_.find(this);
    if (cache_enabled && found != caches_.end() &&
        found->second->free_indices.size() < config_.local_capacity) {
        found->second->free_indices.push_back(node_index);
        tls_free_.fetch_add(1);
        return;
    }
    const std::lock_guard<std::mutex> lock(free_mutex_);
    global_lock_ops_.fetch_add(1);
    node.next_free = free_head_;
    free_head_ = &node;
    ++global_free_;
}

void MessageDeleter::operator()(RecordedMessage* message) const noexcept {
    if (message == nullptr) {
        return;
    }
    if (!pool_state) {
        fail_fast(PoolErrorCode::kInvalidNodeOwnership);
    }
    pool_state->release_message(message, node_index);
}

void PoolState::begin_stop() noexcept {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (runtime_state_ == PoolRuntimeState::kRunning) {
        runtime_state_ = PoolRuntimeState::kStopping;
        lifecycle_cv_.notify_all();
    }
}

bool PoolState::try_finish_stop() noexcept {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (runtime_state_ != PoolRuntimeState::kStopping || in_flight_acquires_ != 0 ||
        in_flight_releases_ != 0 || tls_cache_count_ != 0) {
        return false;
    }
    lifecycle_lock.unlock();
    const std::lock_guard<std::mutex> free_lock(free_mutex_);
    const std::lock_guard<std::mutex> fallback_lock(fallback_mutex_);
    lifecycle_lock.lock();
    if (in_flight_acquires_ != 0 || in_flight_releases_ != 0 || tls_cache_count_ != 0 ||
        in_use_.load() != 0 || fallback_in_use_.load() != 0 || !fallback_registry_.empty() ||
        tls_free_.load() != 0 || global_free_ != config_.capacity) {
        return false;
    }
    runtime_state_ = PoolRuntimeState::kStopped;
    lifecycle_cv_.notify_all();
    return true;
}

PoolStatistics PoolState::statistics() const noexcept {
    PoolStatistics result;
    result.mode = config_.mode;
    {
        const std::lock_guard<std::mutex> lock(free_mutex_);
        result.global_free = global_free_;
    }
    {
        const std::lock_guard<std::mutex> lock(fallback_mutex_);
        result.fallback_registry_size = fallback_registry_.size();
    }
    {
        const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        result.runtime_state = runtime_state_;
    }
    result.capacity = config_.capacity;
    result.in_use = in_use_.load();
    result.tls_free = tls_free_.load();
    result.fallback_in_use = fallback_in_use_.load();
    result.fallback_count = fallback_count_.load();
    result.allocation_failure_count = allocation_failure_count_.load();
    result.global_lock_ops = global_lock_ops_.load();
    result.tls_hits = tls_hits_.load();
    result.tls_misses = tls_misses_.load();
    result.fatal_error = fatal_error_.load();
    return result;
}

PoolRuntimeState PoolState::runtime_state() const noexcept {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return runtime_state_;
}

PoolErrorCode PoolState::fatal_error() const noexcept { return fatal_error_.load(); }

RecordedMessagePool::RecordedMessagePool(PoolConfig config)
: state_(std::make_shared<PoolState>(std::move(config))) {}

MessageHandle RecordedMessagePool::acquire() noexcept { return state_->acquire(); }

void RecordedMessagePool::begin_stop() noexcept { state_->begin_stop(); }

void RecordedMessagePool::release_thread_cache() noexcept { state_->release_thread_cache(); }

bool RecordedMessagePool::try_finish_stop() noexcept { return state_->try_finish_stop(); }

PoolStatistics RecordedMessagePool::statistics() const noexcept { return state_->statistics(); }

std::shared_ptr<PoolState> RecordedMessagePool::state() const noexcept { return state_; }

}  // namespace ros2_bag
