// One-million-message concurrent stress test for formal pool, budget and queue.
#include <atomic>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "ros2_bag/queue_budget.hpp"
#include "ros2_bag/recorded_message_pool.hpp"
#include "ros2_bag/recording_queue.hpp"

namespace {
constexpr std::size_t kProducerCount = 4;
constexpr std::size_t kMessagesPerProducer = 250000;
constexpr std::size_t kTotalMessages = kProducerCount * kMessagesPerProducer;
constexpr std::size_t kPayloadBytes = 16;

}  // namespace

int main() {
    try {
        ros2_bag::PoolConfig config;
        config.mode = ros2_bag::PoolMode::kGlobalMutex;
        config.capacity = 4096;
        config.max_fallback_in_use = 0;
        auto pool = std::make_shared<ros2_bag::PoolState>(config);
        auto budget = std::make_shared<ros2_bag::QueueBudget>(
            config.capacity * kPayloadBytes,
            config.capacity);
        auto queue = std::make_shared<ros2_bag::MessageQueue>(config.capacity, kProducerCount, 0);
        std::atomic<std::size_t> consumed{0};
        std::atomic<bool> producers_done{false};
        std::atomic<bool> failed{false};
        std::thread consumer([&]() {
            moodycamel::ConsumerToken token(*queue);
            ros2_bag::QueuedMessage queued;
            while (!producers_done.load() || consumed.load() != kTotalMessages) {
                if (!queue->try_dequeue(token, queued)) {
                    std::this_thread::yield();
                    continue;
                }
                if (!queued.message || queued.message->recording_generation != 1 ||
                    !queued.budget.valid()) {
                    failed.store(true);
                }
                queued = ros2_bag::QueuedMessage{};
                consumed.fetch_add(1);
            }
        });
        std::vector<std::thread> producers;
        producers.reserve(kProducerCount);
        for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
            producers.emplace_back([&, producer_index]() {
                ros2_bag::ProducerToken token(queue.get());
                for (std::size_t index = 0; index < kMessagesPerProducer; ++index) {
                    ros2_bag::MessageHandle handle;
                    ros2_bag::BudgetLease lease;
                    while (!handle || !lease.valid()) {
                        handle = pool->acquire();
                        lease = budget->try_acquire(kPayloadBytes);
                        if (!handle || !lease.valid()) {
                            handle = ros2_bag::MessageHandle{};
                            lease = ros2_bag::BudgetLease{};
                            std::this_thread::yield();
                        }
                    }
                    handle->recording_generation = 1;
                    handle->topic_id = static_cast<ros2_bag::TopicId>(producer_index + 1);
                    ros2_bag::QueuedMessage queued{std::move(handle), std::move(lease)};
                    while (!token.try_enqueue(&queued)) std::this_thread::yield();
                }
            });
        }
        for (std::thread& producer : producers) producer.join();
        producers_done.store(true);
        consumer.join();
        pool->begin_stop();
        if (failed.load() || consumed.load() != kTotalMessages || !pool->try_finish_stop() ||
            budget->statistics().used_messages != 0) {
            std::cerr << "pool queue stress contract failed\n";
            return 1;
        }
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
