#!/usr/bin/env bash
set -euo pipefail
profile="${FCITX_PROFILE:-${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/profile}"
# fcitx5 writes its in-memory profile when it reloads or exits.  Editing the
# file while a daemon is alive therefore races and can silently restore the
# old input-method list.  Use activate-user.sh for a live session; this
# command remains the low-level, daemon-stopped profile editor.
if command -v fcitx5-remote >/dev/null 2>&1 && fcitx5-remote --check >/dev/null 2>&1; then
  echo "Fcitx5 正在运行；为避免 profile 被旧配置覆盖，请先停止它，或运行 scripts/activate-user.sh。" >&2
  exit 2
fi
exec "$(dirname "$0")/fcitx-profile.py" enable "$profile"
