#!/usr/bin/env bash
set -euo pipefail

# Builds only the app-local GTK input modules. Nothing changes in the user's
# desktop session. --install optionally stages modules in a dedicated prefix.
root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
commit=3b18d2ab7401d4233daf38bbc5896f1703685a43
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/tipe/fcitx5-gtk"
src_dir="${TIPE_GTK_SOURCE_DIR:-$cache_dir/source}"
build_dir="${TIPE_GTK_BUILD_DIR:-$cache_dir/build}"
prefix="${TIPE_GTK_PREFIX:-$HOME/.local/share/tipe/gtk}"
mode="${1:-build}"
if [[ "$mode" != build && "$mode" != --install ]]; then
    echo "Usage: $0 [build|--install]" >&2
    exit 2
fi

# Both are enabled automatically when their development packages exist.
# Set TIPE_BUILD_GTK3=ON or TIPE_BUILD_GTK4=ON to require that toolkit.
gtk3="${TIPE_BUILD_GTK3:-AUTO}"
gtk4="${TIPE_BUILD_GTK4:-AUTO}"
if [[ "$gtk3" == AUTO ]]; then
    if pkg-config --exists gtk+-3.0; then gtk3=ON; else gtk3=OFF; fi
fi
if [[ "$gtk4" == AUTO ]]; then
    if pkg-config --exists 'gtk4 >= 4.2'; then gtk4=ON; else gtk4=OFF; fi
fi
if [[ "$gtk3" == OFF && "$gtk4" == OFF ]]; then
    echo 'GTK3/GTK4 development packages are missing; install at least one first.' >&2
    exit 1
fi

if [[ ! -d "$src_dir/.git" ]]; then
    if [[ -e "$src_dir" ]]; then
        echo "Source directory is not a Git checkout: $src_dir" >&2
        exit 1
    fi
    mkdir -p -- "$(dirname -- "$src_dir")"
    git clone --no-checkout https://github.com/fcitx/fcitx5-gtk.git "$src_dir"
    git -C "$src_dir" checkout --detach "$commit"
fi
if [[ "$(git -C "$src_dir" rev-parse HEAD)" != "$commit" ]]; then
    echo "Expected fcitx5-gtk commit $commit; use a separate source directory." >&2
    exit 1
fi
for patch in \
    "$root_dir/integration/fcitx5-gtk-tipe.patch" \
    "$root_dir/integration/fcitx5-gtk4-popup.patch"; do
    if git -C "$src_dir" apply --reverse --check "$patch" 2>/dev/null; then
        : # Already applied; rebuilding does not reset or overwrite local work.
    else
        git -C "$src_dir" apply --check "$patch"
        git -C "$src_dir" apply "$patch"
    fi
done

cmake -S "$src_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_SKIP_RPATH=ON -DBUILD_ONLY_PLUGIN=ON -DENABLE_GIR=OFF \
    -DENABLE_GTK2_IM_MODULE=OFF \
    -DENABLE_GTK3_IM_MODULE="$gtk3" -DENABLE_GTK4_IM_MODULE="$gtk4" \
    -DTIPE_SOURCE_DIR="$root_dir" -DTIPE_BUILD_TESTS=ON
cmake --build "$build_dir" -j"${TIPE_BUILD_JOBS:-$(nproc)}"
ctest --test-dir "$build_dir" --output-on-failure

if [[ "$mode" == --install ]]; then
    cmake --install "$build_dir"
    echo "GTK modules installed under: $prefix/lib"
fi
printf 'GTK3=%s GTK4=%s\nBuild output: %s\n' "$gtk3" "$gtk4" "$build_dir"
echo 'Existing applications keep their loaded module until they are restarted.'
