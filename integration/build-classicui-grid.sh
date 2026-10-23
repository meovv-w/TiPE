#!/usr/bin/env bash
set -euo pipefail

# Build classicui with the same TiPE painter as the GTK/Qt frontend modules.
# Kept under its original name for existing installation workflows.
root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
commit=1319952f284eae17a36cba9e800843ca61a163c1
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/tipe/fcitx5-classicui"
src_dir="${TIPE_FCITX_SOURCE_DIR:-$cache_dir/source}"
build_dir="${TIPE_FCITX_BUILD_DIR:-$cache_dir/build}"

if [[ ! -d "$src_dir/.git" ]]; then
    if [[ -e "$src_dir" ]]; then
        echo "Source directory is not a Git checkout: $src_dir" >&2
        exit 1
    fi
    mkdir -p -- "$(dirname -- "$src_dir")"
    git clone --no-checkout https://github.com/fcitx/fcitx5.git "$src_dir"
    git -C "$src_dir" checkout --detach "$commit"
fi
if [[ "$(git -C "$src_dir" rev-parse HEAD)" != "$commit" ]]; then
    echo "Expected Fcitx 5.1.21 commit $commit; use a separate source directory." >&2
    exit 1
fi
patch="$root_dir/integration/fcitx5-tipe-painter.patch"
damage_patch="$root_dir/integration/fcitx5-xim-parent-damage.patch"
# The parent-damage overlay touches the painter patch's context. Temporarily
# remove only this exact, known overlay when checking/reapplying its base.
if git -C "$src_dir" apply --reverse --check "$damage_patch" 2>/dev/null; then
    git -C "$src_dir" apply --reverse "$damage_patch"
fi
if git -C "$src_dir" apply --reverse --check "$patch" 2>/dev/null; then
    : # Rebuild safely without resetting existing source edits.
else
    git -C "$src_dir" apply --check "$patch"
    git -C "$src_dir" apply "$patch"
fi
activation_patch="$root_dir/integration/fcitx5-wayland-activation.patch"
if git -C "$src_dir" apply --reverse --check "$activation_patch" 2>/dev/null; then
    :
else
    git -C "$src_dir" apply --check "$activation_patch"
    git -C "$src_dir" apply "$activation_patch"
fi
if git -C "$src_dir" apply --reverse --check "$damage_patch" 2>/dev/null; then
    :
else
    git -C "$src_dir" apply --check "$damage_patch"
    git -C "$src_dir" apply "$damage_patch"
fi
git -C "$src_dir" submodule update --init --depth=1 third_party/yoga

cmake -S "$src_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SKIP_RPATH=ON \
    -DENABLE_WAYLAND=ON -DENABLE_X11=ON \
    -DENABLE_ENCHANT=OFF -DENABLE_TEST=OFF \
    -DTIPE_SOURCE_DIR="$root_dir"
cmake --build "$build_dir" --target classicui waylandim -j"${TIPE_BUILD_JOBS:-4}"
echo "classicui 已构建：$build_dir/bin/libclassicui.so"
echo "Wayland 激活事务修复已构建：$build_dir/bin/libwaylandim.so"
echo "安装时运行：TIPE_CLASSICUI_SO=$build_dir/bin/libclassicui.so $root_dir/scripts/install-user.sh build"
echo '此脚本只构建，不安装或重启用户输入法。'
