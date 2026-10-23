#!/usr/bin/env bash
set -euo pipefail
echo "TiPE 重启预演：仅检查构建产物，不重启桌面或 fcitx5。"
test -x "${1:-build}/tipe-demo"
test -f "${1:-build}/libtipe.so"
echo "产物可用；真实重启请由用户在确认后执行。"
