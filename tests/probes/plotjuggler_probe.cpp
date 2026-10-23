// AC-55 harness: execute the installed PlotJuggler MCAP loader and ROS2 parser.
// No ROS fixture types are linked or loaded. A clean process supplies only MCAP.
#include "check.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include <QApplication>
#include <QPluginLoader>
#include <PlotJuggler/dataloader_base.h>
#include <PlotJuggler/messageparser_base.h>

int main(
    int argc,
    char** argv
) {
    QApplication application(
        argc,
        argv
    );
    try {
        ros2_bag::phase0::require(
            argc == 3,
            "usage: phase0_plotjuggler_probe PREFIX MCAP_FILE"
        );
        const QString prefix = QString::fromLocal8Bit(argv[1]);
        QPluginLoader parser_loader(prefix + "/lib/plotjuggler/libParserROS2.so");
        QPluginLoader mcap_loader(prefix + "/lib/plotjuggler/libDataLoadMCAP.so");
        PJ::ParserFactoryPlugin* parser =
            qobject_cast<PJ::ParserFactoryPlugin*>(parser_loader.instance());
        PJ::DataLoader* loader = qobject_cast<PJ::DataLoader*>(mcap_loader.instance());
        ros2_bag::phase0::require(
            parser != nullptr,
            parser_loader.errorString().toStdString()
        );
        ros2_bag::phase0::require(
            loader != nullptr,
            mcap_loader.errorString().toStdString()
        );
        PJ::ParserFactories factories;
        const PJ::ParserFactoryPtr parser_reference(
            parser,
            [](PJ::ParserFactoryPlugin*) {}
        );
        const QStringList encodings = QString::fromUtf8(parser->encoding()).split(';');
        for (const QString& encoding : encodings) {
            factories[encoding] = parser_reference;
        }
        loader->setParserFactories(&factories);
        PJ::FileLoadInfo info;
        info.filename = QString::fromLocal8Bit(argv[2]);
        QDomElement root = info.plugin_config.createElement("plugin");
        QDomElement parameters = info.plugin_config.createElement("parameters");
        parameters.setAttribute(
            "use_timestamp",
            0
        );
        parameters.setAttribute(
            "use_mcap_log_time",
            1
        );
        parameters.setAttribute(
            "clamp_large_arrays",
            0
        );
        parameters.setAttribute(
            "max_array_size",
            1000
        );
        parameters.setAttribute(
            "selected_topics",
            "/phase0/custom;/phase0/mixed"
        );
        root.appendChild(parameters);
        info.plugin_config.appendChild(root);
        ros2_bag::phase0::require(
            loader->xmlLoadState(root),
            "MCAP plugin rejected saved options"
        );
        PJ::PlotDataMapRef data;
        ros2_bag::phase0::require(
            loader->readDataFromFile(
            &info,
            data
        ),
            "MCAP loader failed"
        );
        // Each expected scalar/array element is checked for every sample, not
        // merely the presence of a Topic or one successfully parsed field.
        std::map<std::string, std::pair<double, double>> expected;
        for (const std::string base : {"/phase0/custom/", "/phase0/mixed/robot/"}) {
            expected[base + "joints[0]/motors[0]/id"] = {7.0, 0.0};
            expected[base + "joints[0]/motors[0]/position"] = {1.25, 1.0};
            expected[base + "joints[0]/motors[0]/velocity"] = {-2.5, 0.0};
            expected[base + "joints[0]/motors[0]/effort"] = {3.75, 0.0};
            expected[base + "joints[0]/calibration_offsets[0]"] = {0.125, 0.0};
            expected[base + "joints[0]/calibration_offsets[1]"] = {-0.25, 0.0};
            expected[base + "stamp/seconds"] = {100.0, 1.0};
            expected[base + "stamp/nanoseconds"] = {123456789.0, 0.0};
            expected[base + "base_position[0]"] = {10.0, 0.0};
            expected[base + "base_position[1]"] = {20.0, 0.0};
            expected[base + "base_position[2]"] = {30.0, 0.0};
            expected[base + "enabled"] = {1.0, 0.0};
        }
        expected["/phase0/mixed/stamp/sec"] = {200.0, 1.0};
        expected["/phase0/mixed/stamp/nanosec"] = {987654321.0, 0.0};
        ros2_bag::phase0::require(
            data.numeric.size() == expected.size(),
            "numeric field set mismatch"
        );
        constexpr std::size_t kSamples = 10;
        constexpr double kEpochSeconds = 1700000000.0;
        constexpr double kIntervalSeconds = 0.1;
        for (const auto& field : expected) {
            const auto found = data.numeric.find(field.first);
            ros2_bag::phase0::require(
                found != data.numeric.end(),
                "missing field: " + field.first
            );
            const PJ::PlotData& series = found->second;
            ros2_bag::phase0::require(
                series.size() == kSamples,
                "parsed field count mismatch"
            );
            for (std::size_t index = 0; index < kSamples; ++index) {
                const double sample = static_cast<double>(index);
                const double value = field.second.first + sample * field.second.second;
                ros2_bag::phase0::require(
                    std::abs(series.at(index).y - value) < 1e-9,
                    "field value mismatch: " + field.first
                );
                ros2_bag::phase0::require(
                    std::abs(series.at(index).x - (kEpochSeconds + sample * kIntervalSeconds)) <
                        1e-5,
                    "MCAP log timestamp mismatch: " + field.first
                );
            }
            std::cout << "PASS field=" << field.first << " samples=" << series.size() << '\n';
        }
        ros2_bag::phase0::require(
            data.strings.size() == 2,
            "string field set mismatch"
        );
        for (const std::string key :
             {"/phase0/custom/joints[0]/name", "/phase0/mixed/robot/joints[0]/name"}) {
            const auto found = data.strings.find(key);
            ros2_bag::phase0::require(
                found != data.strings.end(),
                "missing joint name"
            );
            const PJ::StringSeries& series = found->second;
            ros2_bag::phase0::require(
                series.size() == kSamples,
                "string sample count mismatch"
            );
            for (std::size_t index = 0; index < kSamples; ++index) {
                ros2_bag::phase0::require(
                    series.getString(series.at(index).y) == "joint_alpha",
                    "string field mismatch"
                );
                ros2_bag::phase0::require(
                    std::abs(
                        series.at(index).x -
                        (kEpochSeconds + static_cast<double>(index) * kIntervalSeconds)) < 1e-5,
                    "string timestamp mismatch"
                );
            }
            std::cout << "PASS field=" << key << " samples=" << series.size() << '\n';
        }
        std::cout
            << "PASS installed PlotJuggler MCAP+ROS2 plugins parsed custom dependency closure\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
