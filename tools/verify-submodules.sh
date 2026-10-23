#!/usr/bin/env bash
# Verify gitlinks, checkouts, upstreams and dirty dependencies; no network access.
set -euo pipefail
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$task_root/tools/dependency_metadata.py" "$@"
