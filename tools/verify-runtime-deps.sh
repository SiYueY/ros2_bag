#!/usr/bin/env bash
# Check a trusted, locally built ELF; archive dynamic resolution for review.
set -euo pipefail
if [[ $# -ne 1 || ! -x "$1" ]]; then
    echo 'usage: verify-runtime-deps.sh TRUSTED_EXECUTABLE' >&2
    exit 2
fi
readelf -h "$1"
readelf -d "$1"
task_resolution="$(ldd "$1")"
printf '%s\n' "$task_resolution"
if [[ "$task_resolution" == *'not found'* ]]; then
    echo 'FAIL: unresolved ELF dependency' >&2
    exit 1
fi
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$task_resolution" == *"$task_root/build/"* || "$task_resolution" == *'/work/build/'* ]]; then
    echo 'FAIL: runtime library resolves inside a private build directory' >&2
    exit 1
fi
echo 'PASS: ELF NEEDED resolution (dynamic ROS plugin loading is tested separately)'
