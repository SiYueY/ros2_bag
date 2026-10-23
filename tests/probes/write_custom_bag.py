#!/usr/bin/env python3
"""Record deterministic AC-55 fixtures using installed ROS interfaces and MCAP."""
import argparse
import gc
import json
import math
import os
from pathlib import Path

from rclpy.serialization import serialize_message
import rosbag2_py
from ros2_bag_test_msgs.msg import JointState, MixedRobotState, MotorState, RobotState, TestTimestamp


def robot(sequence):
    value = RobotState()
    motor = MotorState(id=7, position=1.25 + sequence, velocity=-2.5, effort=3.75)
    value.joints = [JointState(name="joint_alpha", motors=[motor], calibration_offsets=[0.125, -0.25])]
    value.stamp = TestTimestamp(seconds=100 + sequence, nanoseconds=123456789)
    value.base_position = [10.0, 20.0, 30.0]
    value.enabled = True
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; refusing overwrite")
    writer = rosbag2_py.SequentialWriter()
    writer.open(rosbag2_py.StorageOptions(uri=str(args.output), storage_id="mcap",
                    storage_config_uri=str(Path(__file__).resolve().parents[2] / "config/mcap.yaml")),
                rosbag2_py.ConverterOptions("cdr", "cdr"))
    for name, message_type in (("/phase0/custom", "RobotState"), ("/phase0/mixed", "MixedRobotState")):
        options = {"id": 0} if os.environ.get("ROS_DISTRO") == "jazzy" else {}
        writer.create_topic(rosbag2_py.TopicMetadata(**options, name=name,
                            type=f"ros2_bag_test_msgs/msg/{message_type}", serialization_format="cdr"))
    count = 10
    sizes = []
    for sequence in range(count):
        timestamp = 1700000000000000000 + sequence * 100000000
        value = robot(sequence)
        custom_payload = serialize_message(value)
        sizes.append(len(custom_payload))
        writer.write("/phase0/custom", custom_payload, timestamp)
        mixed = MixedRobotState(robot=value)
        mixed.stamp.sec = 200 + sequence
        mixed.stamp.nanosec = 987654321
        mixed_payload = serialize_message(mixed)
        sizes.append(len(mixed_payload))
        writer.write("/phase0/mixed", mixed_payload, timestamp)
    del writer
    gc.collect()
    print(json.dumps({"writer": "rosbag2_py", "path": str(args.output),
                      "messages_per_topic": count, "first_log_time_ns": 1700000000000000000,
                      "interval_ns": 100000000, "fixture": "nested-custom-and-mixed",
                      "payload_size_bytes": {str(q): sorted(sizes)[math.ceil(q * len(sizes)) - 1]
                                             for q in (0.5, 0.95, 0.99, 1.0)}}))


if __name__ == "__main__":
    main()
