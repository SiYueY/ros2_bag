#ifndef ROS2_BAG_RECORDER_RECORDER_HPP_
#define ROS2_BAG_RECORDER_RECORDER_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <rclcpp/node.hpp>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/completion.hpp"
#include "ros2_bag/recorder/config.hpp"
#include "ros2_bag/recorder/state.hpp"
#include "ros2_bag/recorder/task.hpp"
#include "ros2_bag/recorder/topic.hpp"
#include "ros2_bag/recorder/topic_events.hpp"

namespace ros2_bag::recorder {

class Recorder {
public:
    explicit Recorder(RecorderConfig config);
    Recorder(RecorderConfig config, rclcpp::Node::SharedPtr node);
    // The owner must await successful shutdown() before destroying Recorder.
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    Completion<TaskInfo> start(const StartOptions& options);
    Completion<void> stop(std::string_view recording_id, std::uint64_t generation);
    Completion<void> pause(std::string_view recording_id, std::uint64_t generation);
    Completion<void> resume(std::string_view recording_id, std::uint64_t generation);
    Completion<TopicInfo> add_topic(
        std::string_view recording_id, std::uint64_t generation, const TopicOptions& options,
        const OperationId& operation_id);
    Completion<void> remove_topic(
        std::string_view recording_id, std::uint64_t generation, TopicId topic_id,
        std::uint64_t topic_epoch, const OperationId& operation_id);
    State state() const;
    Result<std::vector<DiscoveredTopic>, Error> discover_topics() const;
    Result<TaskDetail, Error> get_task(
        std::string_view recording_id, std::size_t segment_offset = 0,
        std::size_t segment_limit = 100) const;
    Result<TaskList, Error> list_tasks(std::size_t offset, std::size_t limit) const;
    Result<TopicOperationState, Error> topic_operation_state(
        std::string_view recording_id, const OperationId& operation_id) const;
    Completion<void> shutdown();

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_RECORDER_HPP_
