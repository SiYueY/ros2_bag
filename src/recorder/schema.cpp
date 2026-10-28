#include "ros2_bag/recorder/schema.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <openssl/sha.h>
#include <rclcpp/typesupport_helpers.hpp>

namespace ros2_bag::recorder {
namespace {

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
                          return std::isspace(character) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return {first, last};
}

bool primitive(std::string_view type) {
    static const std::set<std::string> types{
        "bool",
        "byte",
        "char",
        "float32",
        "float64",
        "int8",
        "uint8",
        "int16",
        "uint16",
        "int32",
        "uint32",
        "int64",
        "uint64",
        "string",
        "wstring",
        "time",
        "duration"};
    return types.find(std::string(type)) != types.end();
}

std::string element_type(std::string type) {
    const auto bracket = type.find('[');
    if (bracket != std::string::npos) {
        type.erase(bracket);
    }
    const auto bound = type.find("<=");
    if (bound != std::string::npos) {
        type.erase(bound);
    }
    return type;
}

class SchemaResolver {
public:
    Result<std::string, Error> resolve(const std::string& type) {
        const auto appended = append_definition(type);
        if (!appended) {
            return Result<std::string, Error>(unexpect, appended.error());
        }

        std::ostringstream encoded;
        encoded << definitions_.back().second;
        for (auto definition = definitions_.rbegin() + 1; definition != definitions_.rend();
             ++definition) {
            encoded << "==========================================================================="
                       "=====\\n";
            encoded << "MSG: " << definition->first << "\\n" << definition->second;
        }
        return Result<std::string, Error>(encoded.str());
    }

private:
    Result<void, Error> append_definition(const std::string& type) {
        if (visited_.find(type) != visited_.end()) {
            return Result<void, Error>();
        }
        if (!visiting_.insert(type).second) {
            return Result<void, Error>(
                unexpect,
                Error{ErrorCode::SchemaUnavailable, Effects::None, "recursive schema dependency"});
        }

        const auto first = type.find('/');
        auto second = std::string::npos;
        if (first != std::string::npos) {
            second = type.find('/', first + 1);
        }
        if (first == std::string::npos || second == std::string::npos ||
            type.substr(first + 1, second - first - 1) != "msg") {
            return Result<void, Error>(
                unexpect,
                Error{ErrorCode::SchemaUnavailable, Effects::None, "unsupported ROS message type"});
        }

        const auto package = type.substr(0, first);
        const auto name = type.substr(second + 1);
        std::ifstream input;
        try {
            input.open(
                std::filesystem::path(ament_index_cpp::get_package_share_directory(package)) /
                "msg" / (name + ".msg"));
        } catch (const std::exception& exception) {
            return Result<void, Error>(
                unexpect,
                Error{ErrorCode::SchemaUnavailable, Effects::None, exception.what()});
        }
        if (!input) {
            return Result<void, Error>(
                unexpect,
                Error{
                    ErrorCode::SchemaUnavailable,
                    Effects::None,
                    "message definition is unavailable"});
        }

        std::ostringstream body;
        std::string line;
        std::vector<std::string> dependencies;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            body << line << '\n';

            const auto comment = line.find('#');
            const auto content = trim(line.substr(0, comment));
            if (content.empty() || content.find('=') != std::string::npos) {
                continue;
            }

            std::istringstream fields(content);
            std::string field_type;
            std::string field_name;
            fields >> field_type >> field_name;
            if (field_type.empty() || field_name.empty()) {
                continue;
            }

            field_type = element_type(field_type);
            if (primitive(field_type)) {
                continue;
            }
            if (field_type.find('/') == std::string::npos) {
                field_type = package + "/msg/" + field_type;
            }
            dependencies.push_back(field_type);
        }

        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(
            std::unique(dependencies.begin(), dependencies.end()),
            dependencies.end());
        for (const auto& dependency : dependencies) {
            const auto appended = append_definition(dependency);
            if (!appended) {
                return appended;
            }
        }

        visiting_.erase(type);
        visited_.insert(type);
        definitions_.emplace_back(type, body.str());
        return Result<void, Error>();
    }

    std::unordered_set<std::string> visiting_;
    std::unordered_set<std::string> visited_;
    std::vector<std::pair<std::string, std::string>> definitions_;
};

}  // namespace

std::string schema_sha256(const std::string& definition) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(definition.data()), definition.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

Result<void, Error> validate_type_support(const std::string& type) {
    if (type.empty()) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::TypeUnavailable, Effects::None, "empty ROS type"});
    }
    try {
        const auto library = rclcpp::get_typesupport_library(type, "rosidl_typesupport_cpp");
        if (!library) {
            return Result<void, Error>(
                unexpect,
                Error{
                    ErrorCode::TypeUnavailable,
                    Effects::None,
                    "ROS type support library is unavailable"});
        }
        return Result<void, Error>();
    } catch (const std::exception& exception) {
        return Result<void, Error>(
            unexpect,
            Error{ErrorCode::TypeUnavailable, Effects::None, exception.what()});
    }
}

Result<ResolvedSchema, Error> resolve_schema(const std::string& type) {
    const auto supported = validate_type_support(type);
    if (!supported) {
        return Result<ResolvedSchema, Error>(unexpect, supported.error());
    }
    SchemaResolver resolver;
    const auto definition = resolver.resolve(type);
    if (!definition) {
        return Result<ResolvedSchema, Error>(unexpect, definition.error());
    }

    ResolvedSchema schema;
    schema.root_type = type;
    schema.definition = definition.value();
    schema.sha256 = schema_sha256(schema.definition);
    return Result<ResolvedSchema, Error>(std::move(schema));
}

}  // namespace ros2_bag::recorder
