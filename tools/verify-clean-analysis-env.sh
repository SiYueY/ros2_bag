#!/usr/bin/env bash
# AC-55 precondition: fail if the fixture package is resolvable or on active paths.
set -euo pipefail
if ros2 pkg prefix ros2_bag_test_msgs >/dev/null 2>&1; then
    echo 'FAIL: test message package is resolvable' >&2
    exit 1
fi
python3 - <<'PY'
import os
from pathlib import Path
for key in ("AMENT_PREFIX_PATH", "CMAKE_PREFIX_PATH", "COLCON_PREFIX_PATH", "PYTHONPATH", "LD_LIBRARY_PATH"):
    for entry in os.environ.get(key, "").split(os.pathsep):
        if not entry:
            continue
        path = Path(entry)
        if "message-install" in str(path) or "ros2_bag_test_msgs" in str(path):
            raise SystemExit(f"FAIL: contaminated search path {key}: {path}")
        index = path / "share/ament_index/resource_index/packages/ros2_bag_test_msgs"
        if index.exists():
            raise SystemExit(f"FAIL: active package index: {index}")
print("PASS: clean analysis environment has no resolvable fixture package")
PY
