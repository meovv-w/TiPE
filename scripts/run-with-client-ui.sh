#!/usr/bin/env bash
set -euo pipefail

# Run one newly-started GTK/Qt process with TiPE's user-local client UI
# modules. This wrapper is process-local and never restarts existing apps.
gtk_prefix="${TIPE_GTK_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/tipe/gtk}"
qt_prefix="${TIPE_QT_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/tipe/qt}"
gtk_path=""
[[ -d "$gtk_prefix/lib/gtk-4.0" ]] && gtk_path+="$gtk_prefix/lib/gtk-4.0"
[[ -d "$gtk_prefix/lib/gtk-3.0" ]] && gtk_path+="${gtk_path:+:}$gtk_prefix/lib/gtk-3.0"
if [[ -n "$gtk_path" ]]; then
  [[ -n "${GTK_PATH:-}" ]] && gtk_path+="${gtk_path:+:}$GTK_PATH"
  export GTK_PATH="$gtk_path"
fi
qt_plugins="$qt_prefix/lib/qt6/plugins"
if [[ -d "$qt_plugins" ]]; then
  qt_plugin_path="$qt_plugins"
  [[ -n "${QT_PLUGIN_PATH:-}" ]] && qt_plugin_path+="${qt_plugin_path:+:}$QT_PLUGIN_PATH"
  export QT_PLUGIN_PATH="$qt_plugin_path"
fi
export GTK_IM_MODULE="${GTK_IM_MODULE:-fcitx}"
export QT_IM_MODULE="${QT_IM_MODULE:-fcitx}"
export XMODIFIERS="${XMODIFIERS:-@im=fcitx}"
if (($# == 0)); then
  echo "用法：$0 command [args ...]" >&2
  exit 2
fi
exec "$@"
