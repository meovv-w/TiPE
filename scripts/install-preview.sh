#!/usr/bin/env bash
set -euo pipefail
build_dir="${1:-build}"
cmake --build "$build_dir"
stage_dir="${DESTDIR:-/tmp/tipe-staging}"
# Remove the old module name from previous TiPE builds so the staging tree
# cannot make an obsolete plugin look installed alongside the current one.
rm -f "$stage_dir/usr/local/lib/fcitx5/tipe.so"
rm -f "$stage_dir/usr/local/lib/fcitx5/libtipe-ui.so"
DESTDIR="$stage_dir" cmake --install "$build_dir"
echo "已完成安装预演，文件位于 $stage_dir；未修改全局 fcitx/niri 配置。"
