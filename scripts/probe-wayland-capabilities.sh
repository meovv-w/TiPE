#!/usr/bin/env bash
set -euo pipefail

# Read-only Wayland capability probe.  This deliberately does not bind an
# input-method object or create a surface; it records what the compositor
# advertises so a desktop run cannot confuse a protocol global with a popup
# that TiPE actually owns.
out_dir="${1:-build/wayland-capability-probe}"
mkdir -p "$out_dir"
info_file="$out_dir/wayland-info.txt"
json_file="$out_dir/capabilities.json"
: >"$info_file"

display="${WAYLAND_DISPLAY:-}"
runtime="${XDG_RUNTIME_DIR:-}"
socket_path="$display"
if [[ "$display" != /* ]]; then
  socket_path="$runtime/$display"
fi
status="ok"
reason=""
tool=""

if [[ -z "$display" ]]; then
  status="skipped"
  reason="no-wayland-display"
elif [[ ! -S "$socket_path" ]]; then
  status="skipped"
  reason="wayland-socket-not-found"
elif command -v wayland-info >/dev/null 2>&1; then
  tool="wayland-info"
  if ! timeout 5s wayland-info -i input >"$info_file" 2>&1; then
    status="error"
    reason="wayland-info-failed"
  fi
elif command -v weston-info >/dev/null 2>&1; then
  tool="weston-info"
  if ! timeout 5s weston-info >"$info_file" 2>&1; then
    status="error"
    reason="weston-info-failed"
  fi
else
  status="skipped"
  reason="no-wayland-info-tool"
fi

python3 - "$info_file" "$json_file" "$status" "$reason" "$tool" "$display" <<'PY'
import json
import re
import sys
from pathlib import Path

info_path, json_path, status, reason, tool, display = sys.argv[1:]
text = Path(info_path).read_text(errors="replace")
globals_ = {}
for match in re.finditer(r"interface:\s*'([^']+)'\s*,\s*version:\s*(\d+)", text):
    name, version = match.group(1), int(match.group(2))
    globals_[name] = max(version, globals_.get(name, 0))

def version(*names):
    for name in names:
        if name in globals_:
            return globals_[name]
    return 0

manager = version("zwp_input_method_manager_v2", "xx_input_method_manager_v2")
popup_manager = version("zwp_input_popup_manager_v2", "xx_input_popup_manager_v2")
popup_surface = version("zwp_input_popup_surface_v2", "xx_input_popup_surface_v2")
result = {
    "status": status,
    "reason": reason,
    "tool": tool,
    "wayland_display": bool(display),
    "display": display,
    "globals": globals_,
    "input_method_manager_v2": manager > 0,
    "input_method_manager_v2_version": manager,
    "popup_manager_v2_global": popup_manager > 0,
    "popup_manager_v2_global_version": popup_manager,
    "popup_surface_v2_global": popup_surface > 0,
    "popup_surface_v2_global_version": popup_surface,
    # The popup surface is normally a child object of the input-method v2
    # object, so it is expected not to appear in registry output.  More
    # importantly, this probe has no access to Fcitx's private popup factory.
    "native_popup_created": False,
    "native_popup_evidence": "registry-only",
}
Path(json_path).write_text(json.dumps(result, ensure_ascii=False, sort_keys=True) + "\n")
print("status=%s input_method_manager_v2=%s popup_surface_global=%s output=%s" %
      (status, bool(manager), bool(popup_surface), json_path))
PY

# A failed probe is diagnostic rather than a reason to prevent the input
# method from starting.  Keep a nonzero exit only for an actual tool error so
# CI can distinguish a broken invocation from a machine without Wayland.
[[ "$status" != "error" ]]
