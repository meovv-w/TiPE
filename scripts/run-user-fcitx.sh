#!/usr/bin/env bash
set -euo pipefail
export FCITX_ADDON_DIRS="${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/addon:$HOME/.local/lib/fcitx5:/usr/lib64/fcitx5"
export LD_LIBRARY_PATH="${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/addon:$HOME/.local/lib/fcitx5:${LD_LIBRARY_PATH:-}"
export XDG_DATA_DIRS="${XDG_DATA_HOME:-$HOME/.local/share}:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
# Keep the separate diagnostic X11 UI off. Patched classicui and the GTK/Qt
# client modules share the candidate painter and anchor through their native
# input-popup/text rectangles.
export TIPE_UI_X11="${TIPE_UI_X11:-0}"
# Enable the shared TiPE grid in patched classicui. Stock classicui ignores it.
export TIPE_CLASSIC_GRID="${TIPE_CLASSIC_GRID:-1}"
# Compact mode is the product default: one horizontal candidate row. The
# engine switches to its multi-row navigation model only after arrow/Tab
# expansion; users can opt into a vertical fallback explicitly.
export TIPE_CLASSIC_LAYOUT="${TIPE_CLASSIC_LAYOUT:-horizontal}"
exec fcitx5 "$@"
