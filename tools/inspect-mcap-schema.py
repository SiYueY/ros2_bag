#!/usr/bin/env python3
"""Inspect actual MCAP summary Schema/Channel/Statistics records without ROS types.

This does not replace PlotJuggler field parsing. Chunk payloads are not decoded.
The independent summary scan intentionally requires the configured summary.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MAGIC = b"\x89MCAP0\r\n"


def string(data, offset):
    length = struct.unpack_from("<I", data, offset)[0]
    start = offset + 4
    end = start + length
    if end > len(data):
        raise ValueError("truncated MCAP string")
    return data[start:end].decode("utf-8"), end


def inspect(path):
    schemas = {}
    channels = {}
    chunks = []
    count = None
    with path.open("rb") as stream:
        if stream.read(8) != MAGIC:
            raise ValueError("invalid MCAP header")
        while True:
            header = stream.read(9)
            if len(header) < 9:
                if header != MAGIC:
                    raise ValueError("missing MCAP trailing magic")
                break
            opcode, length = struct.unpack("<BQ", header)
            if opcode not in (3, 4, 6, 11):
                stream.seek(length, 1)
                continue
            body = stream.read(length)
            if len(body) != length:
                raise ValueError("truncated MCAP record")
            if opcode == 3:
                schema_id = struct.unpack_from("<H", body)[0]
                name, offset = string(body, 2)
                encoding, offset = string(body, offset)
                definition, offset = string(body, offset)
                schemas[schema_id] = {"name": name, "encoding": encoding, "definition": definition}
            elif opcode == 4:
                channel_id, schema_id = struct.unpack_from("<HH", body)
                topic, offset = string(body, 4)
                encoding, offset = string(body, offset)
                channels[channel_id] = {"schema_id": schema_id, "topic": topic, "encoding": encoding}
            elif opcode == 11:
                count = struct.unpack_from("<Q", body)[0]
            elif opcode == 6:
                compression, offset = string(body, 28)
                chunks.append({"compression": compression,
                               "uncompressed_bytes": struct.unpack_from("<Q", body, 16)[0],
                               "compressed_bytes": struct.unpack_from("<Q", body, offset)[0]})
    for channel in channels.values():
        if channel["schema_id"] not in schemas or channel["encoding"] != "cdr":
            raise ValueError("missing Schema association or wrong CDR encoding")
    for schema in schemas.values():
        if schema["encoding"] not in ("ros2msg", "ros2idl") or not schema["definition"]:
            raise ValueError("missing supported schema definition")
        if schema["name"].endswith("/RobotState"):
            for dependency in ("JointState", "MotorState", "TestTimestamp"):
                if not re.search(r"ros2_bag_test_msgs/(?:msg/)?" + dependency, schema["definition"]):
                    raise ValueError(f"missing dependency reference: {dependency}")
                if schema["encoding"] == "ros2msg" and not re.search(
                        r"MSG: ros2_bag_test_msgs/(?:msg/)?" + dependency, schema["definition"]):
                    raise ValueError(f"missing dependency definition: {dependency}")
    if not schemas or not channels or count is None:
        raise ValueError("missing summary schema/channel/statistics")
    return {"file": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "schemas": schemas, "channels": channels, "chunks": chunks, "message_count": count}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--expect-custom", action="store_true")
    args = parser.parse_args()
    result = inspect(args.file)
    if args.expect_custom:
        topics = {channel["topic"] for channel in result["channels"].values()}
        if topics != {"/phase0/custom", "/phase0/mixed"} or result["message_count"] != 20:
            raise ValueError("custom fixture topic/count mismatch")
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
