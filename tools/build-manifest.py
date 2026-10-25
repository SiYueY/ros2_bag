#!/usr/bin/env python3
"""Record the reproducible Recorder development build identity."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

from dependency_metadata import verify


def output(*command):
    return subprocess.check_output(command, text=True).strip()


def verify_runtime_dependencies(binary: Path) -> list[str]:
    """Returns resolved dynamic dependencies or terminates on an unsafe result."""
    result = output("ldd", str(binary))
    if "not found" in result:
        raise SystemExit("build manifest requires resolved runtime dependencies")
    if "/work/build/" in result or "/build/" in result:
        raise SystemExit("build manifest rejects private build-directory runtime dependencies")
    return result.splitlines()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--base-image", default="not-recorded")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_files = {}
    for folder in ("src", "include", "tests", "tools", "docker", "config", ".github"):
        for path in sorted((root / folder).rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                source_files[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in ("CMakeLists.txt", "package.xml", ".clang-format", ".gitmodules", "docs/system_technology_selection.md"):
        source_files[name] = hashlib.sha256((root / name).read_bytes()).hexdigest()
    packages = subprocess.run(["dpkg-query", "-W", "-f=${Package}=${Version}\n", "ros-*"],
                              capture_output=True, text=True, check=False)
    cache = args.binary.parent / "CMakeCache.txt"
    flags = {}
    if cache.exists():
        for line in cache.read_text().splitlines():
            if ':' not in line or '=' not in line or line.startswith(('//', '#')):
                continue
            name, value = line.split('=', 1)
            name = name.split(':', 1)[0]
            if name.startswith(('CMAKE_CXX_', 'ROS2_BAG_')) or name == 'CMAKE_BUILD_TYPE':
                flags[name] = value
    working_tree_dirty = bool(output("git", "-C", str(root), "status", "--porcelain"))
    data = {
        "schema_version": 2, "artifact_scope": "recorder_development",
        "source_commit": output("git", "-C", str(root), "rev-parse", "HEAD"),
        "working_tree_dirty": working_tree_dirty,
        "source_file_sha256": source_files,
        "third_party_shas": verify(root),
        "build_timestamp": datetime.now(timezone.utc).isoformat(),
        "ros_distro": os.environ.get("ROS_DISTRO"), "architecture": platform.machine(),
        "ubuntu_version": Path("/etc/os-release").read_text(),
        "base_image_digest": args.base_image,
        "compiler_version": output("c++", "--version"),
        "glibc_version": platform.libc_ver(),
        "ros_package_versions": packages.stdout.splitlines(),
        "build_flags": flags,
        "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "elf_dynamic": output("readelf", "-d", str(args.binary)),
        "runtime_dependencies_verified": True,
        "runtime_dependencies": verify_runtime_dependencies(args.binary),
    }
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_text(json.dumps(data, indent=2) + "\n")


if __name__ == "__main__":
    main()
