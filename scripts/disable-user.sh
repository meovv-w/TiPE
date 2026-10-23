#!/usr/bin/env bash
set -euo pipefail
profile="${FCITX_PROFILE:-${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/profile}"
if command -v fcitx5-remote >/dev/null 2>&1 && fcitx5-remote --check >/dev/null 2>&1; then
  echo "Fcitx5 正在运行；请先停止它，再禁用 TiPE，以免 profile 被旧配置覆盖。" >&2
  exit 2
fi
exec "$(dirname "$0")/fcitx-profile.py" disable "$profile"
