#!/usr/bin/env bash
# Generate metadata only from verified local gitlinks, never from floating tags.
set -euo pipefail
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -ne 1 ]]; then
    echo 'usage: generate-third-party-lock.sh OUTPUT_JSON' >&2
    exit 2
fi
exec python3 "$task_root/tools/dependency_metadata.py" --output "$1"
