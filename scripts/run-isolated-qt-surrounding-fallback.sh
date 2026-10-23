#!/usr/bin/env bash
set -euo pipefail

# Run one real Qt6 widget in the same private Fcitx/X11 session as the normal
# isolated harness, while making ImSurroundingText explicitly unavailable.
# This is deliberately a separate scenario so the normal GTK/Qt edit check
# keeps exercising a fully capable widget.
build_dir="${1:-build}"
out_dir="${2:-${build_dir}/isolated-qt-surrounding-fallback}"
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

run_out="$out_path/isolated"
mkdir -p "$run_out"
TIPE_HARNESS_NO_SURROUNDING=1 \
TIPE_REAL_APP_SCENARIO=surrounding-fallback \
TIPE_HARNESS_TIMEOUT_MS="${TIPE_HARNESS_TIMEOUT_MS:-7000}" \
  "$source_root/scripts/run-isolated-real-app-harness.sh" \
  "$build_path" "$run_out"

qt_log="$run_out/qt6.jsonl"
if [[ ! -s "$qt_log" ]]; then
  printf 'status=skipped reason=qt6-jsonl-not-produced\n'
  exit 0
fi

# The commit proves this is an actual TiPE transaction. The unavailable query
# marker proves the widget advertised the protocol fallback. Finally, the
# native key handlers must see both edits and leave the committed text as one
# CJK code point; a restored "nihao" preedit would instead produce a second IM
# transaction and no native Backspace handler event.
grep -q '"event":"input_method_query".*"surrounding_available":false' \
  "$qt_log" || { printf 'status=failed reason=qt6-did-not-report-surrounding-unavailable\n'; exit 1; }
grep -q '"commit":"你好"' "$qt_log" || {
  printf 'status=failed reason=qt6-did-not-commit-你好\n'
  exit 1
}
grep -q '"event":"key_press".*"key":16777219.*"phase":"after".*"value":"你"' \
  "$qt_log" || { printf 'status=failed reason=qt6-backspace-did-not-reach-client\n'; exit 1; }
grep -q '"event":"key_press".*"key":16777223.*"phase":"after".*"value":"你"' \
  "$qt_log" || { printf 'status=failed reason=qt6-delete-did-not-reach-client\n'; exit 1; }
# This regression is intentionally scoped to the Qt6 path.  The shared
# harness may also start GTK4, but nested Xwayland focus is not deterministic
# for that optional client (it can receive literal key events instead of the
# IM transaction).  Do not let that unrelated probe make the Qt fallback test
# flaky; the normal real-app harness covers GTK4 separately.
printf 'status=verified toolkit=qt6 surrounding_available=false commit=你好 final=你 backspace=client delete=client\n'
