// Minimal Recorder-only development runtime; no control transports are exposed.
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>

#include "ros2_bag/config.hpp"
#include "ros2_bag/recording_coordinator.hpp"
#include "ros2_bag/recovery.hpp"

namespace {

// Prevent concurrent Recorder runtimes from mutating the same output directory.
// The lock file is intentionally persistent: unlinking a locked inode permits
// another process to create and acquire a different lock under the same path.
class OutputRootLock {
public:
    explicit OutputRootLock(const std::filesystem::path& output_root) {
        std::filesystem::create_directories(output_root);
        const auto directory = std::filesystem::canonical(output_root);
        const int directory_fd =
            ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory_fd < 0) {
            throw std::runtime_error(
                "cannot open Recorder output root: " + std::string(std::strerror(errno)));
        }
        const int descriptor = ::openat(
            directory_fd,
            ".ros2_bag_recorder.lock",
            O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
            0600);
        const int open_error = errno;
        ::close(directory_fd);
        if (descriptor < 0) {
            throw std::runtime_error(
                "cannot open Recorder output lock: " + std::string(std::strerror(open_error)));
        }
        struct stat metadata {};
        if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
            ::close(descriptor);
            throw std::runtime_error("Recorder output lock must be a regular file");
        }
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int lock_error = errno;
            ::close(descriptor);
            throw std::runtime_error(
                "Recorder output root is already in use: " + directory.string() + ": " +
                std::strerror(lock_error));
        }
        descriptor_ = descriptor;
    }

    ~OutputRootLock() { ::close(descriptor_); }
    OutputRootLock(const OutputRootLock&) = delete;
    OutputRootLock& operator=(const OutputRootLock&) = delete;

private:
    int descriptor_ = -1;
};

}  // namespace

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    int result = 0;
    try {
        std::string config_path = "config/recorder.yaml";
        for (int index = 1; index < argc; ++index) {
            const std::string argument(argv[index]);
            if (argument == "--config" && index + 1 < argc) {
                config_path = argv[++index];
            } else {
                std::cerr << "usage: ros2_bag_recorder [--config recorder.yaml]\n";
                rclcpp::shutdown();
                return 2;
            }
        }
        const ros2_bag::RecorderConfig config = ros2_bag::load_recorder_config(config_path);
        const OutputRootLock output_lock(config.recording.output_root);
        const auto recovery = ros2_bag::recover_deleting_segments(config.recording.output_root);
        const auto incomplete =
            ros2_bag::recover_incomplete_recordings(config.recording.output_root);
        for (const auto& failure : recovery) {
            std::cerr << "ros2_bag_recorder: recovery pending for " << failure.task_directory
                      << ": " << failure.error << '\n';
        }
        for (const auto& failure : incomplete) {
            std::cerr << "ros2_bag_recorder: recovery pending for " << failure.task_directory
                      << ": " << failure.error << '\n';
        }
        const auto node = std::make_shared<rclcpp::Node>("ros2_bag_recorder");
        ros2_bag::RecordingCoordinator recorder(node, config);
        rclcpp::executors::MultiThreadedExecutor executor(
            rclcpp::ExecutorOptions(),
            config.executor.thread_count);
        executor.add_node(node);
        std::cerr << "Recorder-only runtime: no external control interface; "
                     "recording operations are exercised through C++ tests.\n";
        executor.spin();
        executor.remove_node(node);
        recorder.shutdown();
    } catch (const std::exception& error) {
        std::cerr << "ros2_bag_recorder: " << error.what() << '\n';
        result = 1;
    }
    if (rclcpp::ok()) {
        rclcpp::shutdown();
    }
    return result;
}
