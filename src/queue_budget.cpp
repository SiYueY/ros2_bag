// QueueBudget and RAII BudgetLease implementation.
#include "ros2_bag/queue_budget.hpp"

#include <stdexcept>
#include <utility>

namespace ros2_bag {

BudgetLease::BudgetLease(std::shared_ptr<QueueBudget> budget, std::size_t bytes) noexcept
: budget_(std::move(budget)), bytes_(bytes) {}

BudgetLease::BudgetLease(BudgetLease&& other) noexcept
: budget_(std::move(other.budget_)), bytes_(other.bytes_) {
    other.bytes_ = 0;
}

BudgetLease& BudgetLease::operator=(BudgetLease&& other) noexcept {
    if (this != &other) {
        release(BudgetReleaseReason::kRollback);
        budget_ = std::move(other.budget_);
        bytes_ = other.bytes_;
        other.bytes_ = 0;
    }
    return *this;
}

BudgetLease::~BudgetLease() { release(BudgetReleaseReason::kRollback); }

bool BudgetLease::valid() const noexcept { return static_cast<bool>(budget_); }

std::size_t BudgetLease::bytes() const noexcept { return bytes_; }

void BudgetLease::release(BudgetReleaseReason reason) noexcept {
    if (!budget_) {
        return;
    }
    const std::shared_ptr<QueueBudget> budget = std::move(budget_);
    budget->release(bytes_, reason);
    bytes_ = 0;
}

QueueBudget::QueueBudget(std::size_t max_bytes, std::size_t max_messages)
: max_bytes_(max_bytes), max_messages_(max_messages) {
    if (max_bytes_ == 0 || max_messages_ == 0) {
        throw std::invalid_argument("QueueBudget limits must be positive");
    }
    statistics_.max_bytes = max_bytes_;
    statistics_.max_messages = max_messages_;
}

BudgetLease QueueBudget::try_acquire(std::size_t bytes) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > max_bytes_ - statistics_.used_bytes || statistics_.used_messages >= max_messages_) {
        return BudgetLease{};
    }
    statistics_.used_bytes += bytes;
    ++statistics_.used_messages;
    ++statistics_.acquire_count;
    return BudgetLease(shared_from_this(), bytes);
}

void QueueBudget::release(std::size_t bytes, BudgetReleaseReason reason) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (statistics_.used_messages == 0 || bytes > statistics_.used_bytes) {
        ++statistics_.invalid_release_count;
        return;
    }
    statistics_.used_bytes -= bytes;
    --statistics_.used_messages;
    ++statistics_.release_count;
    if (reason == BudgetReleaseReason::kNormal) {
        ++statistics_.release_normal_count;
    } else {
        ++statistics_.release_rollback_count;
    }
}

QueueBudgetStatistics QueueBudget::statistics() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
}

}  // namespace ros2_bag
