#!/usr/bin/env bash
# Phase 0 packaging skeleton: never presents a probe as a V1.0 release.
set -euo pipefail
if [[ $# -ne 4 || "$1" != --phase0-preview ]]; then
    echo 'usage: package.sh --phase0-preview BUILD_MAIN_DIR humble|jazzy OUTPUT_DIR' >&2
    echo 'Production packaging is blocked until Phase 1-3 and four-platform acceptance.' >&2
    exit 2
fi
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$task_root/tools/package_preview.py" "$2" "$3" "$4"
