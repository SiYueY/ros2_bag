// Phase 0 memory experiments only. This is not the Phase 1 production pool.
// Shared probe state deliberately outlives handles and thread-local caches.
#ifndef ROS2_BAG_TESTS_SUPPORT_PROBE_MEMORY_HPP_
#define ROS2_BAG_TESTS_SUPPORT_PROBE_MEMORY_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include "check.hpp"

namespace ros2_bag::phase0 {

// Type-erased owner is probe-only: both synthetic and ROS payloads can be held.
struct RecordedMessage {
    std::shared_ptr<const void> payload_owner;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::size_t payload_bytes = 0;
};

struct alignas(64) Node {
    RecordedMessage message;
    Node* next_free = nullptr;
    std::atomic<bool> borrowed{false};
};

enum class PoolMode { Tls, GlobalMutex };

inline constexpr std::size_t kDefaultFallbackLimit = 8;

class PoolState;

// RAII probe deleter. Owns state, while cookie identifies a non-owning fixed node.
struct MessageDeleter {
    std::shared_ptr<PoolState> state;
    Node* node_cookie = nullptr;

    // Returns a valid node or destroys a fallback; noexcept, safe across threads.
    // Invalid cookies increment diagnostics without deleting unknown pointers.
    void operator()(RecordedMessage* message) const noexcept;
};

using MessageHandle = std::unique_ptr<RecordedMessage, MessageDeleter>;

struct PoolSnapshot {
    std::size_t capacity;
    std::size_t in_use;
    std::size_t global_free;
    std::size_t tls_free;
    std::size_t fallback_in_use;
    std::size_t fallback_count;
    std::size_t internal_errors;
};

// Probe-only fixed pool. Public operations are thread safe; final conservation
// must be inspected after all users/cache owners have stopped.
class PoolState : public std::enable_shared_from_this<PoolState> {
public:
    // Allocates capacity nodes, with at most fallback_limit extra heap wrappers.
    // Throws bad_alloc; construct before workers. mode does not change at runtime.
    PoolState(
        std::size_t capacity,
        PoolMode mode,
        std::size_t fallback_limit = kDefaultFallbackLimit
    )
    : storage_(std::make_unique<Node[]>(capacity)),
      capacity_(capacity),
      mode_(mode),
      fallback_limit_(fallback_limit) {
        for (std::size_t index = 0; index < capacity_; ++index) {
            storage_[index].next_free = head_;
            head_ = &storage_[index];
        }
        global_free_ = capacity_;
    }

    // Acquires a wrapper without waiting for capacity. Returns null on exhaustion
    // or allocation failure. Throws only if TLS bookkeeping allocation fails.
    MessageHandle acquire();

    // Flushes only the calling thread's cache. Must be called before a quiescent
    // snapshot on a still-live worker; other caches flush at thread exit.
    void release_thread_cache();

    // Samples counters; approximate under load, exact after all caches flush.
    PoolSnapshot snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return {
            capacity_,
            in_use_.load(),
            global_free_,
            tls_free_.load(),
            fallback_in_use_.load(),
            fallback_count_.load(),
            internal_errors_.load()};
    }

    // Validates and returns a borrowed node; called only by its unique owner.
    // No I/O; noexcept. Invalid input is diagnosed and not blindly freed.
    void release(
        RecordedMessage* message,
        Node* cookie
    ) noexcept;

private:
    struct LocalCache {
        std::shared_ptr<PoolState> state;
        std::vector<Node*> free;

        ~LocalCache() { state->flush(free); }
    };

    static constexpr std::size_t kLocalCapacity = 8;
    static constexpr std::size_t kTransferBatch = 4;
    using CacheMap = std::unordered_map<PoolState*, std::unique_ptr<LocalCache>>;
    inline static thread_local CacheMap caches_;

    LocalCache& local_cache() {
        std::unique_ptr<LocalCache>& cache = caches_[this];
        if (!cache) {
            cache = std::make_unique<LocalCache>();
            cache->state = shared_from_this();
            cache->free.reserve(kLocalCapacity);
        }
        return *cache;
    }

    void flush(std::vector<Node*>& nodes) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (Node* node : nodes) {
            node->next_free = head_;
            head_ = node;
            ++global_free_;
        }
        tls_free_.fetch_sub(nodes.size());
        nodes.clear();
    }

    std::unique_ptr<Node[]> storage_;
    const std::size_t capacity_;
    const PoolMode mode_;
    const std::size_t fallback_limit_;
    mutable std::mutex mutex_;
    Node* head_ = nullptr;
    std::size_t global_free_ = 0;
    std::atomic<std::size_t> in_use_{0};
    std::atomic<std::size_t> tls_free_{0};
    std::atomic<std::size_t> fallback_in_use_{0};
    std::atomic<std::size_t> fallback_count_{0};
    std::atomic<std::size_t> internal_errors_{0};
};

inline MessageHandle PoolState::acquire() {
    Node* node = nullptr;
    if (mode_ == PoolMode::Tls) {
        LocalCache& cache = local_cache();
        if (cache.free.empty()) {
            const std::lock_guard<std::mutex> lock(mutex_);
            while (head_ != nullptr && cache.free.size() < kTransferBatch) {
                Node* next = head_;
                head_ = next->next_free;
                cache.free.push_back(next);
                --global_free_;
                tls_free_.fetch_add(1);
            }
        }
        if (!cache.free.empty()) {
            node = cache.free.back();
            cache.free.pop_back();
            tls_free_.fetch_sub(1);
        }
    } else {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (head_ != nullptr) {
            node = head_;
            head_ = node->next_free;
            --global_free_;
        }
    }
    if (node != nullptr) {
        require(
            !node->borrowed.exchange(true),
            "duplicate node acquisition"
        );
        in_use_.fetch_add(1);
        return MessageHandle(
            &node->message,
            MessageDeleter{shared_from_this(), node}
        );
    }
    std::size_t count = fallback_in_use_.load();
    while (count < fallback_limit_) {
        if (fallback_in_use_.compare_exchange_weak(
            count,
            count + 1
        )) {
            try {
                std::unique_ptr<RecordedMessage> fallback = std::make_unique<RecordedMessage>();
                fallback_count_.fetch_add(1);
                return MessageHandle(
                    fallback.release(),
                    MessageDeleter{shared_from_this(), nullptr}
                );
            } catch (const std::bad_alloc&) {
                fallback_in_use_.fetch_sub(1);
                return MessageHandle(
                    nullptr,
                    MessageDeleter{}
                );
            }
        }
    }
    return MessageHandle(
        nullptr,
        MessageDeleter{}
    );
}

inline void PoolState::release_thread_cache() { caches_.erase(this); }

inline void PoolState::release(
    RecordedMessage* message,
    Node* cookie
) noexcept {
    if (cookie == nullptr) {
        // Probe fallback was allocated by make_unique; restore the matching RAII
        // deleter rather than exposing explicit delete in the data path.
        const std::unique_ptr<RecordedMessage> fallback(message);
        fallback_in_use_.fetch_sub(1);
        return;
    }
    bool belongs = false;
    for (std::size_t index = 0; index < capacity_; ++index) {
        if (&storage_[index] == cookie) {
            belongs = true;
            break;
        }
    }
    if (!belongs || &cookie->message != message || !cookie->borrowed.exchange(false)) {
        internal_errors_.fetch_add(1);
        return;
    }
    message->payload_owner.reset();
    *message = RecordedMessage{};
    in_use_.fetch_sub(1);
    // release must not allocate a TLS cache: a different thread may be returning
    // the wrapper for the first time, including during exception unwinding.
    const auto found = caches_.find(this);
    if (mode_ == PoolMode::Tls && found != caches_.end() &&
        found->second->free.size() < kLocalCapacity) {
        found->second->free.push_back(cookie);
        tls_free_.fetch_add(1);
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    cookie->next_free = head_;
    head_ = cookie;
    ++global_free_;
}

inline void MessageDeleter::operator()(RecordedMessage* message) const noexcept {
    if (state) {
        state->release(
            message,
            node_cookie
        );
    } else if (node_cookie == nullptr) {
        const std::unique_ptr<RecordedMessage> fallback(message);
    }
}

struct BudgetSnapshot {
    std::size_t bytes;
    std::size_t messages;
    std::size_t acquired;
    std::size_t released;
};

// Probe budget: one mutex gives the two quotas a single linearization point.
class QueueBudget {
public:
    // Initializes immutable byte/message limits before concurrent access.
    QueueBudget(
        std::size_t max_bytes,
        std::size_t max_messages
    )
    : max_bytes_(max_bytes),
      max_messages_(max_messages) {}

    // Atomically reserves one message and bytes; false means no mutation.
    bool try_acquire(std::size_t bytes) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (bytes > max_bytes_ - used_bytes_ || used_messages_ == max_messages_) {
            return false;
        }
        used_bytes_ += bytes;
        ++used_messages_;
        ++acquired_;
        return true;
    }

    // Returns a previously acquired reservation; noexcept, thread safe.
    // Invariant corruption terminates the probe instead of wrapping counters.
    void release(std::size_t bytes) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (used_messages_ == 0 || bytes > used_bytes_) {
            std::terminate();
        }
        used_bytes_ -= bytes;
        --used_messages_;
        ++released_;
    }

    // Returns an exact counter snapshot under the budget lock.
    BudgetSnapshot snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return {used_bytes_, used_messages_, acquired_, released_};
    }

private:
    const std::size_t max_bytes_;
    const std::size_t max_messages_;
    mutable std::mutex mutex_;
    std::size_t used_bytes_ = 0;
    std::size_t used_messages_ = 0;
    std::size_t acquired_ = 0;
    std::size_t released_ = 0;
};

// Move-only reservation. Shared state prevents teardown from dangling its owner.
class BudgetLease {
public:
    // Empty lease has no release obligation; thread-confined until moved.
    BudgetLease() = default;

    // Adopts exactly one successful reservation; caller must not adopt twice.
    BudgetLease(
        std::shared_ptr<QueueBudget> owner,
        std::size_t bytes
    )
    : owner_(std::move(owner)),
      bytes_(bytes) {}

    BudgetLease(const BudgetLease&) = delete;
    BudgetLease& operator=(const BudgetLease&) = delete;

    // Transfers ownership without releasing; noexcept, requires exclusive access.
    BudgetLease(BudgetLease&& other) noexcept
    : owner_(std::move(other.owner_)),
      bytes_(std::exchange(
          other.bytes_,
          0
      )) {}

    // Releases any old reservation then adopts other's; same concurrency rule.
    BudgetLease& operator=(BudgetLease&& other) noexcept {
        if (this != &other) {
            reset();
            owner_ = std::move(other.owner_);
            bytes_ = std::exchange(
                other.bytes_,
                0
            );
        }
        return *this;
    }

    ~BudgetLease() { reset(); }

private:
    void reset() noexcept {
        if (owner_) {
            owner_->release(bytes_);
            owner_.reset();
        }
    }

    std::shared_ptr<QueueBudget> owner_;
    std::size_t bytes_ = 0;
};

struct QueuedMessage {
    MessageHandle message;
    BudgetLease budget;
};
}  // namespace ros2_bag::phase0
#endif  // ROS2_BAG_TESTS_SUPPORT_PROBE_MEMORY_HPP_
