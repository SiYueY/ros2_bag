#ifndef ROS2_BAG_RECORDER_WRITER_HPP_
#define ROS2_BAG_RECORDER_WRITER_HPP_

#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "ros2_bag/result.hpp"
#include "ros2_bag/recorder/error.hpp"
#include "ros2_bag/recorder/message.hpp"
#include "ros2_bag/recorder/task.hpp"
#include "ros2_bag/recorder/topic.hpp"

namespace ros2_bag::recorder {

class Writer {
public:
    explicit Writer(CompressionMode compression);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    Result<void, Error> open(const std::filesystem::path& directory);
    Result<void, Error> register_topic(const TopicInfo& topic);
    Result<void, Error> write_batch(const std::vector<RecordedMessage>& messages);
    Result<void, Error> close();

private:
    class WriterState;

    static std::filesystem::path resolve_mcap_config();
    void finalize_segment();

    CompressionMode compression_;
    std::mutex mutex_;
    std::unique_ptr<WriterState> state_;
};

}  // namespace ros2_bag::recorder

#endif  // ROS2_BAG_RECORDER_WRITER_HPP_
