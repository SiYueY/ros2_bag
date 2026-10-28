#ifndef ROS2_BAG_RECORDER_WRITER_HPP_
#define ROS2_BAG_RECORDER_WRITER_HPP_

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

#include "ros2_bag/recorder/completion.hpp"
#include "ros2_bag/recorder/message.hpp"
#include "ros2_bag/recorder/topic.hpp"

namespace ros2_bag::recorder {

class Writer {
public:
    explicit Writer(std::size_t control_queue_capacity);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    Completion<void> open(std::string directory);
    Completion<void> register_topic(TopicInfo topic);
    Completion<void> write(RecordedMessage message, std::string topic_name, std::string type_name);
    Completion<void> close();
    void shutdown();

private:
    class WriterState;
    bool submit(std::function<void()> command);
    void run();

    const std::size_t control_queue_capacity_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> commands_;
    bool stopping_ = false;
    std::unique_ptr<WriterState> state_;
    std::thread thread_;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_WRITER_HPP_
