// Phase 0 non-ROS experiments: fixed pool, move-only queue and budget failures.
#include <array>
#include <chrono>
#include <atomic>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include <blockingconcurrentqueue.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include "probe_memory.hpp"

namespace ros2_bag::phase0 {
namespace {
constexpr std::size_t kCapacity = 64;
constexpr std::size_t kPayloadBytes = 8;
constexpr std::size_t kMessages = 10000;
constexpr std::size_t kProducers = 4;
constexpr std::size_t kBulkBatchSize = 16;
constexpr std::chrono::milliseconds kQueueWait{10};
using MessageQueue = moodycamel::BlockingConcurrentQueue<QueuedMessage>;

void check_conservation(const std::shared_ptr<PoolState>& pool) {
    pool->release_thread_cache();
    const PoolSnapshot state = pool->snapshot();
    require(state.in_use == 0 && state.fallback_in_use == 0, "outstanding wrappers");
    require(state.global_free == state.capacity && state.tls_free == 0, "lost fixed nodes");
    require(state.internal_errors == 0, "pool ownership error");
}

void exercise_pool(PoolMode mode) {
    const std::shared_ptr<PoolState> pool = std::make_shared<PoolState>(kCapacity, mode);
    std::array<std::thread, kProducers> threads;
    std::atomic<bool> failed{false};
    for (std::thread& thread : threads) {
        thread = std::thread([&pool, &failed]() {
            try {
                for (std::size_t index = 0; index < kMessages; ++index) {
                    MessageHandle message = pool->acquire();
                    require(static_cast<bool>(message), "pool unexpectedly exhausted");
                    message->sequence = index;
                }
            } catch (const std::exception&) {
                failed.store(true);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    require(!failed.load(), "worker failure");

    // Return a producer-owned wrapper on a different thread with no local cache.
    MessageHandle message = pool->acquire();
    std::thread consumer([handle = std::move(message)]() mutable { handle.reset(); });
    consumer.join();
    check_conservation(pool);

    // A queued owner keeps PoolState alive after the facade goes away.
    std::shared_ptr<PoolState> temporary = std::make_shared<PoolState>(kCapacity, mode);
    MessageHandle survivor = temporary->acquire();
    temporary->release_thread_cache();
    const std::weak_ptr<PoolState> observer = temporary;
    temporary.reset();
    require(!observer.expired(), "handle lost pool owner");
    survivor = MessageHandle{};
    require(observer.expired(), "pool retained after final handle/cache");
}

void exercise_fallback() {
    constexpr std::size_t kFixed = 2;
    constexpr std::size_t kFallback = 1;
    const std::shared_ptr<PoolState> pool =
        std::make_shared<PoolState>(kFixed, PoolMode::GlobalMutex, kFallback);
    std::vector<MessageHandle> handles;
    for (std::size_t index = 0; index < kFixed + kFallback; ++index) {
        handles.push_back(pool->acquire());
        require(static_cast<bool>(handles.back()), "bounded fallback unavailable");
    }
    require(!pool->acquire(), "fallback limit bypassed");
    require(pool->snapshot().fallback_count == kFallback, "fallback not counted");
    handles.clear();
    check_conservation(pool);
}

void exercise_queue(PoolMode mode) {
    const std::shared_ptr<PoolState> pool = std::make_shared<PoolState>(kCapacity, mode);
    const std::shared_ptr<QueueBudget> budget =
        std::make_shared<QueueBudget>(kCapacity * kPayloadBytes, kCapacity);
    // Token-aware preallocation accounts for the maximum explicit producers.
    MessageQueue queue(kCapacity, kProducers, 0);
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> failures{0};
    std::atomic<bool> done{false};
    std::thread consumer([&]() {
        moodycamel::ConsumerToken token(queue);
        std::array<QueuedMessage, kBulkBatchSize> batch;
        while (!done.load() || budget->snapshot().messages != 0) {
            const std::size_t count =
                queue.wait_dequeue_bulk_timed(token, batch.begin(), batch.size(), kQueueWait);
            for (std::size_t index = 0; index < count; ++index) {
                if (!batch[index].message || batch[index].message->generation != 1) {
                    failures.fetch_add(1);
                }
                batch[index] = QueuedMessage{};
            }
        }
    });
    std::array<std::thread, kProducers> producers;
    for (std::thread& producer : producers) {
        producer = std::thread([&]() {
            moodycamel::ProducerToken token(queue);
            for (std::size_t index = 0; index < kMessages; ++index) {
                if (!budget->try_acquire(kPayloadBytes)) {
                    continue;
                }
                BudgetLease lease(budget, kPayloadBytes);
                MessageHandle handle = pool->acquire();
                if (!handle) {
                    continue;
                }
                handle->generation = 1;
                handle->sequence = index;
                QueuedMessage message{std::move(handle), std::move(lease)};
                if (queue.try_enqueue(token, std::move(message))) {
                    accepted.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    done.store(true);
    consumer.join();
    require(accepted.load() > 0 && failures.load() == 0, "queue payload mismatch");
    const BudgetSnapshot state = budget->snapshot();
    require(
        state.bytes == 0 && state.messages == 0 && state.acquired == state.released,
        "budget not conserved");
    check_conservation(pool);

    // Deliberately leave a lease inside the queue: destruction must release it.
    {
        MessageQueue abandoned(kCapacity);
        require(budget->try_acquire(kPayloadBytes), "reservation failed");
        QueuedMessage message{pool->acquire(), BudgetLease(budget, kPayloadBytes)};
        require(abandoned.enqueue(std::move(message)), "destruction setup failed");
    }
    require(budget->snapshot().messages == 0, "queue destruction leaked reservation");
    check_conservation(pool);

    // Force try_enqueue failure using a tiny non-allocating queue. Failure keeps
    // the argument owned by the producer until its RAII scope ends.
    {
        MessageQueue tiny(0);
        moodycamel::ProducerToken token(tiny);
        std::size_t rejected = 0;
        for (std::size_t index = 0; index < kCapacity; ++index) {
            require(budget->try_acquire(kPayloadBytes), "failure-path quota");
            QueuedMessage message{pool->acquire(), BudgetLease(budget, kPayloadBytes)};
            if (!tiny.try_enqueue(token, std::move(message))) {
                ++rejected;
            }
        }
        require(rejected > 0, "enqueue failure path not reached");
    }
    require(
        budget->snapshot().acquired == budget->snapshot().released,
        "failed enqueue leaked lease");
    check_conservation(pool);
}

void exercise_budget_and_invalid_cookie() {
    const std::shared_ptr<QueueBudget> budget = std::make_shared<QueueBudget>(kPayloadBytes, 1);
    require(!budget->try_acquire(kPayloadBytes + 1), "byte limit ignored");
    require(budget->try_acquire(kPayloadBytes), "valid reservation rejected");
    BudgetLease first(budget, kPayloadBytes);
    require(!budget->try_acquire(0), "message limit ignored");
    BudgetLease second(std::move(first));
    first = std::move(second);
    first = BudgetLease{};
    require(budget->snapshot().released == 1, "move released more than once");

    const std::shared_ptr<PoolState> pool =
        std::make_shared<PoolState>(kCapacity, PoolMode::GlobalMutex);
    MessageHandle handle = pool->acquire();
    Node unrelated;
    pool->release(handle.get(), &unrelated);
    require(pool->snapshot().internal_errors == 1, "invalid cookie not detected");
    handle.reset();
    require(pool->snapshot().in_use == 0, "valid owner could not recover");
}

void exercise_task_restart() {
    constexpr std::uint64_t kGenerations = 16;
    const std::shared_ptr<PoolState> pool = std::make_shared<PoolState>(kCapacity, PoolMode::Tls);
    // Keep one OS worker alive while task queues are destroyed and recreated.
    // Token scopes deliberately end before their queue, independent of Pool TLS.
    std::atomic<bool> failed{false};
    std::thread worker([&]() {
        try {
            for (std::uint64_t generation = 1; generation <= kGenerations; ++generation) {
                const std::shared_ptr<QueueBudget> budget =
                    std::make_shared<QueueBudget>(kPayloadBytes, 1);
                MessageQueue queue(kCapacity);
                {
                    moodycamel::ProducerToken producer(queue);
                    moodycamel::ConsumerToken consumer(queue);
                    require(budget->try_acquire(kPayloadBytes), "restart reservation failed");
                    MessageHandle handle = pool->acquire();
                    require(static_cast<bool>(handle), "restart pool acquisition failed");
                    handle->generation = generation;
                    QueuedMessage message{std::move(handle), BudgetLease(budget, kPayloadBytes)};
                    require(
                        queue.try_enqueue(producer, std::move(message)),
                        "restart enqueue failed");
                    QueuedMessage received;
                    require(queue.try_dequeue(consumer, received), "restart dequeue failed");
                    require(
                        received.message->generation == generation,
                        "stale generation after queue reconstruction");
                }
                require(budget->snapshot().messages == 0, "restart leaked budget");
            }
        } catch (const std::exception&) {
            failed.store(true);
        }
    });
    worker.join();
    require(!failed.load(), "persistent-worker task restart failed");
    check_conservation(pool);
}
}  // namespace
}  // namespace ros2_bag::phase0

int main() {
    static_assert(!std::is_copy_constructible_v<ros2_bag::phase0::QueuedMessage>);
    static_assert(std::is_nothrow_move_constructible_v<ros2_bag::phase0::QueuedMessage>);
    try {
        const YAML::Node config = YAML::Load("phase: 0");
        ros2_bag::phase0::require(config["phase"].as<int>() == 0, "yaml dependency");
        for (const ros2_bag::phase0::PoolMode mode :
             {ros2_bag::phase0::PoolMode::Tls, ros2_bag::phase0::PoolMode::GlobalMutex}) {
            ros2_bag::phase0::exercise_pool(mode);
            ros2_bag::phase0::exercise_queue(mode);
        }
        ros2_bag::phase0::exercise_fallback();
        ros2_bag::phase0::exercise_budget_and_invalid_cookie();
        ros2_bag::phase0::exercise_task_restart();
        std::cout << nlohmann::json({{"probe", "core"},
                                     {"status", "PASS"},
                                     {"pool_modes", {"tls", "global_mutex"}},
                                     {"producer_iterations", 40000}})
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
