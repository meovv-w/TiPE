#!/usr/bin/env bash
set -euo pipefail

# Run the isolated real-toolkit edit scenario and assert the final text that
# GTK4 and Qt6 report after two Chinese commits, a real cursor move/delete,
# an English token, and another Chinese commit.  This is intentionally kept
# outside CTest because it starts a private Xwayland/Fcitx session.
build_dir="${1:-build}"
out_dir="${2:-${build_dir}/isolated-edit-regression}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
source_root="$(cd "$script_dir/.." && pwd)"
if [[ "$build_dir" = /* ]]; then
  build_path="$build_dir"
else
  build_path="$source_root/$build_dir"
fi
if [[ "$out_dir" = /* ]]; then
  out_path="$out_dir"
else
  out_path="$source_root/$out_dir"
fi
mkdir -p "$out_path"
TIPE_HARNESS_TIMEOUT_MS="${TIPE_HARNESS_TIMEOUT_MS:-15000}" \
TIPE_REAL_APP_SCENARIO=edit \
  "$source_root/scripts/run-isolated-real-app-harness.sh" \
  "$build_path" "$out_path"

python3 - "$out_path" <<'PY'
import json
import pathlib
import sys

out = pathlib.Path(sys.argv[1])
expected = "你好界github你好"

def values(path):
    result = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if obj.get("event") == "text" and "value" in obj:
            result.append(obj["value"])
    return result

for name in ("gtk4", "qt6"):
    seen = values(out / f"{name}.jsonl")
    if expected not in seen:
        raise SystemExit(f"{name}: expected final text {expected!r}, saw {seen!r}")
    print(f"{name}=verified final_text={expected}")

print("status=verified isolated_real_edit=true")
PY
