#!/usr/bin/env bash
# Execute the installed PlotJuggler plugins in fixture-free, networkless images.
set -euo pipefail
task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_fixtures="${1:-$task_root/docs/evidence/phase0/fixtures}"
task_logs="$task_root/docs/evidence/phase0"
for task_analysis in humble jazzy; do
    for task_recording in humble jazzy; do
        docker run --rm --network none --entrypoint bash \
            -v "$task_fixtures:/fixtures:ro" "ros2-bag-analysis:$task_analysis" -lc \
            'source /opt/ros/$ROS_DISTRO/setup.bash &&
             bash /analysis/verify-clean-analysis-env.sh &&
             test ! -e /work &&
             timeout 45 /analysis/phase0_plotjuggler_probe /opt/ros/$ROS_DISTRO "$1"' \
            -- "/fixtures/$task_recording.mcap" \
            > "$task_logs/ac55-$task_recording-to-$task_analysis.txt" 2>&1
    done
done
echo 'PASS AC-55: four loader/parser combinations, all numeric/string samples checked'
