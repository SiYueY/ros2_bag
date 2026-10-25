// Phase 0 allocator/alias lifetime experiment without DDS participants. Keeping
// this independent distinguishes payload failures from prebuilt RMW diagnostics.
#include <atomic>
#include <iostream>
#include <memory>

#include <rclcpp/serialization.hpp>
#include <std_msgs/msg/u_int64.hpp>

#include "check.hpp"
#include "payload_binding.hpp"

namespace {
struct AllocatorCounters {
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> releases{0};
};

// These C allocator callbacks delegate to rcutils' matching allocator. They are
// owned by SerializedMessage RAII; no business code receives allocation duties.
void* counted_allocate(std::size_t size, void* state) {
    const rcutils_allocator_t allocator = rcutils_get_default_allocator();
    void* memory = allocator.allocate(size, allocator.state);
    if (memory != nullptr) {
        static_cast<AllocatorCounters*>(state)->allocations.fetch_add(1);
    }
    return memory;
}

void counted_release(void* memory, void* state) {
    if (memory != nullptr) {
        static_cast<AllocatorCounters*>(state)->releases.fetch_add(1);
    }
    const rcutils_allocator_t allocator = rcutils_get_default_allocator();
    allocator.deallocate(memory, allocator.state);
}

void* counted_reallocate(void* memory, std::size_t size, void* state) {
    const rcutils_allocator_t allocator = rcutils_get_default_allocator();
    void* resized = allocator.reallocate(memory, size, allocator.state);
    if (resized != nullptr && memory == nullptr) {
        static_cast<AllocatorCounters*>(state)->allocations.fetch_add(1);
    }
    return resized;
}

void* counted_zero_allocate(std::size_t count, std::size_t size, void* state) {
    const rcutils_allocator_t allocator = rcutils_get_default_allocator();
    void* memory = allocator.zero_allocate(count, size, allocator.state);
    if (memory != nullptr) {
        static_cast<AllocatorCounters*>(state)->allocations.fetch_add(1);
    }
    return memory;
}
}  // namespace

int main() {
    try {
        AllocatorCounters counters;
        rcutils_allocator_t allocator = rcutils_get_default_allocator();
        allocator.state = &counters;
        allocator.allocate = counted_allocate;
        allocator.deallocate = counted_release;
        allocator.reallocate = counted_reallocate;
        allocator.zero_allocate = counted_zero_allocate;
        constexpr std::size_t kCapacity = 128;
        std::shared_ptr<rclcpp::SerializedMessage> owner =
            std::make_shared<rclcpp::SerializedMessage>(kCapacity, allocator);
        std_msgs::msg::UInt64 expected;
        expected.data = 42;
        rclcpp::Serialization<std_msgs::msg::UInt64> serialization;
        serialization.serialize_message(&expected, owner.get());
        const std::uint8_t* address = owner->get_rcl_serialized_message().buffer;
        std::shared_ptr<rosbag2_storage::SerializedBagMessage> bound =
            ros2_bag::phase0::bind_payload(owner, 1, "/payload");
        const std::weak_ptr<rclcpp::SerializedMessage> lifetime = owner;
        ros2_bag::phase0::require(bound->serialized_data->buffer == address, "adapter copied CDR");
        owner.reset();
        ros2_bag::phase0::require(
            !lifetime.expired() && counters.releases.load() == 0,
            "payload freed before Writer descriptor");
        std_msgs::msg::UInt64 decoded;
        serialization.deserialize_message(lifetime.lock().get(), &decoded);
        ros2_bag::phase0::require(decoded.data == expected.data, "aliased payload corrupt");
        bound.reset();
        ros2_bag::phase0::require(lifetime.expired(), "descriptor retained owner");
        ros2_bag::phase0::require(
            counters.allocations.load() == 1 && counters.releases.load() == 1,
            "allocator did not release exactly once");
        std::cout << "PASS payload adapter_buffer_identity=preserved allocations=1 releases=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
