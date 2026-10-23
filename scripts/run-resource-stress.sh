#!/usr/bin/env bash
set -euo pipefail

# Run the sustained local-worker resource regression.  This deliberately
# creates an isolated state directory inside the test process and never
# touches the user's profile.  Increase rounds/pause for a longer soak, e.g.:
#   TIPE_RESOURCE_STRESS_ROUNDS=1800 TIPE_RESOURCE_STRESS_PAUSE_MS=20 \
#     scripts/run-resource-stress.sh build build/resource-stress.log
build_dir="${1:-build}"
out_file="${2:-${build_dir}/resource-stress.log}"
rounds="${TIPE_RESOURCE_STRESS_ROUNDS:-600}"
events="${TIPE_RESOURCE_STRESS_EVENTS:-128}"
pause_ms="${TIPE_RESOURCE_STRESS_PAUSE_MS:-10}"

cmake --build "$build_dir" --target tipe-resource-stress-tests
mkdir -p "$(dirname "$out_file")"
{
  echo "TiPE sustained resource stress"
  echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "rounds=$rounds events_per_round=$events pause_ms=$pause_ms"
  echo "host=$(hostname)"
  if command -v fcitx5 >/dev/null 2>&1; then fcitx5 --version 2>&1 | head -n 1; fi
  TIPE_RESOURCE_STRESS_ROUNDS="$rounds" \
  TIPE_RESOURCE_STRESS_EVENTS="$events" \
  TIPE_RESOURCE_STRESS_PAUSE_MS="$pause_ms" \
    "$build_dir/tipe-resource-stress-tests"
} | tee "$out_file"
echo "resource stress log: $out_file"
