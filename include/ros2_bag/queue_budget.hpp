// Task-local byte and message admission accounting for the recording queue.
#ifndef ROS2_BAG_QUEUE_BUDGET_HPP_
#define ROS2_BAG_QUEUE_BUDGET_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ros2_bag {

enum class BudgetReleaseReason {
    kNormal,
    kRollback,
};

struct QueueBudgetStatistics {
    std::size_t max_bytes = 0;
    std::size_t max_messages = 0;
    std::size_t used_bytes = 0;
    std::size_t used_messages = 0;
    std::uint64_t acquire_count = 0;
    std::uint64_t release_count = 0;
    std::uint64_t release_normal_count = 0;
    std::uint64_t release_rollback_count = 0;
    std::uint64_t double_release_count = 0;
    std::uint64_t invalid_release_count = 0;
};

class QueueBudget;

// Move-only reservation returned only after QueueBudget has admitted one message.
class BudgetLease {
public:
    BudgetLease() = default;
    BudgetLease(std::shared_ptr<QueueBudget> budget, std::size_t bytes) noexcept;
    BudgetLease(const BudgetLease&) = delete;
    BudgetLease& operator=(const BudgetLease&) = delete;
    BudgetLease(BudgetLease&& other) noexcept;
    BudgetLease& operator=(BudgetLease&& other) noexcept;
    ~BudgetLease();

    // Returns whether this object still owns a reservation.
    bool valid() const noexcept;
    std::size_t bytes() const noexcept;

    // Releases once. The default describes a processed Writer message; rollback
    // is used for allocation/enqueue/control failures. Thread-safe via budget.
    void release(BudgetReleaseReason reason = BudgetReleaseReason::kNormal) noexcept;

private:
    std::shared_ptr<QueueBudget> budget_;
    std::size_t bytes_ = 0;
};

// Bounded dual quota. All accounting mutations are serialized by mutex_.
class QueueBudget : public std::enable_shared_from_this<QueueBudget> {
public:
    // Throws invalid_argument for a zero limit. The object is safe to share.
    QueueBudget(std::size_t max_bytes, std::size_t max_messages);

    // Atomically reserves bytes and one message. Empty lease means capacity was
    // unavailable; ordinary pressure does not throw.
    BudgetLease try_acquire(std::size_t bytes);

    QueueBudgetStatistics statistics() const noexcept;

private:
    friend class BudgetLease;

    void release(std::size_t bytes, BudgetReleaseReason reason) noexcept;

    const std::size_t max_bytes_;
    const std::size_t max_messages_;
    mutable std::mutex mutex_;
    QueueBudgetStatistics statistics_;
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_QUEUE_BUDGET_HPP_
