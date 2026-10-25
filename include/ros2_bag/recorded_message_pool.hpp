// Process-wide fixed RecordedMessage pool with bounded fallback ownership.
#ifndef ROS2_BAG_RECORDED_MESSAGE_POOL_HPP_
#define ROS2_BAG_RECORDED_MESSAGE_POOL_HPP_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ros2_bag/recorded_message.hpp"

namespace ros2_bag {

using NodeIndex = std::uint32_t;

constexpr NodeIndex kInvalidNodeIndex = std::numeric_limits<NodeIndex>::max();

enum class PoolMode {
    kTls,
    kGlobalMutex,
};

enum class PoolRuntimeState {
    kRunning,
    kStopping,
    kStopped,
};

enum class PoolErrorCode : std::uint32_t {
    kNone,
    kInvalidNodeIndex,
    kInvalidNodeOwnership,
    kDoubleRelease,
    kInvalidFallbackOwnership,
    kReleaseAfterStopped,
    kInternalInvariantViolation,
};

// Terminates immediately after recording the first fatal internal ownership error.
// It does no allocation, I/O or coordination and is never used for exhaustion.
[[noreturn]] void fail_fast(PoolErrorCode error_code) noexcept;

class PoolState;

struct MessageDeleter {
    std::shared_ptr<PoolState> pool_state;
    NodeIndex node_index = kInvalidNodeIndex;

    // Returns an owned fixed or registered fallback message. It is noexcept and
    // supports a null argument. Internal ownership corruption terminates.
    void operator()(RecordedMessage* message) const noexcept;
};

using MessageHandle = std::unique_ptr<RecordedMessage, MessageDeleter>;

struct PoolConfig {
    PoolMode mode = PoolMode::kTls;
    std::size_t capacity = 81920;
    std::size_t max_fallback_in_use = 1024;
    std::size_t local_capacity = 64;
    std::size_t batch_size = 64;
};

struct PoolStatistics {
    PoolMode mode = PoolMode::kTls;
    std::size_t capacity = 0;
    std::size_t in_use = 0;
    std::size_t global_free = 0;
    std::size_t tls_free = 0;
    std::size_t fallback_in_use = 0;
    std::size_t fallback_registry_size = 0;
    std::size_t fallback_count = 0;
    std::size_t allocation_failure_count = 0;
    std::size_t global_lock_ops = 0;
    std::size_t tls_hits = 0;
    std::size_t tls_misses = 0;
    PoolRuntimeState runtime_state = PoolRuntimeState::kRunning;
    PoolErrorCode fatal_error = PoolErrorCode::kNone;
};

// Shared lifetime state. Public operations are thread-safe. begin_stop() rejects
// later acquires while continuing to accept every legal handle release.
class PoolState : public std::enable_shared_from_this<PoolState> {
public:
    // Creates fixed aligned storage. Throws invalid_argument or bad_alloc before
    // any worker can observe the state. The configuration is immutable afterwards.
    explicit PoolState(PoolConfig config);

    PoolState(const PoolState&) = delete;
    PoolState& operator=(const PoolState&) = delete;

    // Acquires a fixed node or bounded registered fallback. Empty means ordinary
    // exhaustion or allocation failure. Thread-safe; no exception escapes.
    MessageHandle acquire() noexcept;

    // Starts shutdown. It is idempotent and does not invalidate returned handles.
    void begin_stop() noexcept;

    // Flushes this thread's bounded TLS cache. It must be called by live managed
    // threads before a quiescent stop check; thread exit flushes automatically.
    void release_thread_cache() noexcept;

    // Transitions Stopping to Stopped only when every documented conservation
    // condition holds. Returns false when callers still own handles or caches.
    bool try_finish_stop() noexcept;

    // Returns an approximate live sample and an exact quiescent sample after all
    // managed threads have flushed. Thread-safe.
    PoolStatistics statistics() const noexcept;

    PoolRuntimeState runtime_state() const noexcept;
    PoolErrorCode fatal_error() const noexcept;

private:
    friend struct MessageDeleter;

    struct alignas(64) Node {
        RecordedMessage message;
        Node* next_free = nullptr;
        std::atomic<bool> borrowed{false};
    };

    struct LocalCache {
        std::shared_ptr<PoolState> pool_state;
        std::vector<NodeIndex> free_indices;
        bool counted = false;

        ~LocalCache();
    };

    using CacheMap = std::unordered_map<PoolState*, std::unique_ptr<LocalCache>>;
    static thread_local CacheMap caches_;

    bool enter_acquire() noexcept;
    void leave_acquire() noexcept;
    bool enter_release() noexcept;
    void leave_release() noexcept;
    LocalCache* find_or_create_local_cache() noexcept;
    void flush_local_cache(std::vector<NodeIndex>* indices) noexcept;
    void release_message(RecordedMessage* message, NodeIndex node_index) noexcept;
    void set_fatal_error(PoolErrorCode error_code) noexcept;

    const PoolConfig config_;
    std::unique_ptr<Node[]> storage_;
    mutable std::mutex free_mutex_;
    Node* free_head_ = nullptr;
    std::size_t global_free_ = 0;

    mutable std::mutex fallback_mutex_;
    std::unordered_set<RecordedMessage*> fallback_registry_;

    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    PoolRuntimeState runtime_state_ = PoolRuntimeState::kRunning;
    std::size_t in_flight_acquires_ = 0;
    std::size_t in_flight_releases_ = 0;
    std::size_t tls_cache_count_ = 0;

    std::atomic<std::size_t> in_use_{0};
    std::atomic<std::size_t> tls_free_{0};
    std::atomic<std::size_t> fallback_in_use_{0};
    std::atomic<std::size_t> fallback_count_{0};
    std::atomic<std::size_t> allocation_failure_count_{0};
    std::atomic<std::size_t> global_lock_ops_{0};
    std::atomic<std::size_t> tls_hits_{0};
    std::atomic<std::size_t> tls_misses_{0};
    std::atomic<PoolErrorCode> fatal_error_{PoolErrorCode::kNone};
};

// Process-level facade. Handles keep PoolState alive after this facade is gone.
class RecordedMessagePool {
public:
    // Builds a shared pool state. Throws as PoolState does.
    explicit RecordedMessagePool(PoolConfig config);

    // See PoolState::acquire(). Thread-safe.
    MessageHandle acquire() noexcept;
    void begin_stop() noexcept;
    void release_thread_cache() noexcept;
    bool try_finish_stop() noexcept;
    PoolStatistics statistics() const noexcept;
    std::shared_ptr<PoolState> state() const noexcept;

private:
    std::shared_ptr<PoolState> state_;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_RECORDED_MESSAGE_POOL_HPP_
