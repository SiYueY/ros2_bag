#include "ros2_bag/recorder/schema.hpp"

#include <iomanip>
#include <sstream>

#include <openssl/sha.h>
#include <rosbag2_cpp/typesupport_helpers.hpp>

namespace ros2_bag::recorder {

std::string schema_sha256(const std::string& definition) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(definition.data()), definition.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

Result<void, Error> validate_type_support(const std::string& type) {
    if (type.empty()) {
        return Result<void, Error>(
            unexpect, Error{ErrorCode::TypeUnavailable, Effects::None, "empty ROS type"});
    }
    try {
        const auto library = rosbag2_cpp::get_typesupport_library(type, "rosidl_typesupport_cpp");
        if (!library) {
            return Result<void, Error>(
                unexpect, Error{ErrorCode::TypeUnavailable, Effects::None, "ROS type support library is unavailable"});
        }
        return Result<void, Error>();
    } catch (const std::exception& exception) {
        return Result<void, Error>(unexpect, Error{ErrorCode::TypeUnavailable, Effects::None, exception.what()});
    }
}

}  // namespace ros2_bag::recorder
