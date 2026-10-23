#!/usr/bin/env python3
"""Create an explicitly incomplete Phase 0 native-platform packaging preview."""
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
build, distro, destination = Path(sys.argv[1]).resolve(), sys.argv[2], Path(sys.argv[3]).resolve()
if distro not in ("humble", "jazzy"):
    raise SystemExit("unsupported ROS distro")
arch = {"x86_64": "amd64", "aarch64": "arm64"}[platform.machine()]
if destination.exists():
    raise SystemExit("refusing to overwrite an existing preview directory")
destination.mkdir(parents=True)
with tempfile.TemporaryDirectory(prefix="ros2-bag-preview-") as temporary:
    staging = Path(temporary) / "ros2_bag"
    staging.mkdir()
    for name in ("ros2_bag", "bag_cli", "bag_web", "README.md"):
        shutil.copy2(root / name, staging / name)
        if name != "README.md":
            # The release layout specifies exact entrypoint modes. copy2 alone
            # inherits workspace/group permissions and can preserve mode 0775.
            (staging / name).chmod(0o755)
    for name in ("scripts", "config"):
        shutil.copytree(root / name, staging / name)
    (staging / "docs").mkdir()
    for name in ("deployment.md", "recovery.md", "phase0_validation_report.md",
                 "implementation_notes.md", "recorded_message_data_model.md"):
        shutil.copy2(root / "docs" / name, staging / "docs" / name)
    target = staging / distro / arch
    target.mkdir(parents=True)
    shutil.copy2(build / "ros2_bag_recorder", target / "ros2_bag_recorder")
    subprocess.run(["bash", str(root / "tools/generate-third-party-lock.sh"),
                    str(staging / "third_party.lock")], check=True)
    manifest_path = build / "build-manifest.json"
    if not manifest_path.is_file():
        raise SystemExit("build-manifest.json must be generated in the actual build environment first")
    manifest = json.loads(manifest_path.read_text())
    binary_hash = hashlib.sha256((build / "ros2_bag_recorder").read_bytes()).hexdigest()
    if manifest["binary_sha256"] != binary_hash or manifest["ros_distro"] != distro:
        raise SystemExit("build manifest does not match the selected binary/distro")
    shutil.copy2(manifest_path, staging / "build-manifest.json")
    (staging / "PHASE0_ONLY.json").write_text(json.dumps(
        {"functional_recorder": False, "complete_release": False, "platforms": [f"{distro}/{arch}"]}) + "\n")
    licenses = staging / "LICENSES"
    licenses.mkdir()
    shutil.copy2(root / "LICENSE", licenses / "ros2_bag.txt")
    names = {"concurrentqueue": "LICENSE.md", "sockpp": "LICENSE", "json": "LICENSE.MIT",
             "yaml-cpp": "LICENSE"}
    for dependency, filename in names.items():
        shutil.copy2(root / "third_party" / dependency / filename, licenses / f"{dependency}.txt")
    artifact = destination / "ros2_bag.tar.xz"
    with tarfile.open(artifact, "w:xz") as archive:
        archive.add(staging, arcname="ros2_bag")
digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
(destination / "SHA256SUMS").write_text(f"{digest}  ros2_bag.tar.xz\n")
print(f"PASS Phase 0 preview only: {artifact}")
