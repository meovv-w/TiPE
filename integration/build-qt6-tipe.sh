#!/usr/bin/env bash
set -euo pipefail

# Qt6 frontend overlay; never writes system libraries or starts an input method.
root_dir="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="${TIPE_QT6_WORK_DIR:-$root_dir/build/integration}"
src_dir="${TIPE_QT6_SOURCE_DIR:-$work_dir/fcitx5-qt}"
build_dir="${TIPE_QT6_BUILD_DIR:-$work_dir/fcitx5-qt-build}"
prefix="${TIPE_QT6_PREFIX:-$work_dir/qt6-prefix}"
version=5.1.14
archive_sha=80cbbc6b3ad08a6b7a40bb6cc4072e1c3bcf2099a821ce04b1bd510848c804d7
mkdir -p "$work_dir"

fetch() {
    local url="$1" destination="$2"
    if ! curl --fail --location --connect-timeout 15 --max-time 90 "$url" -o "$destination"; then
        curl --noproxy '*' --fail --location --connect-timeout 15 --max-time 90 "$url" -o "$destination"
    fi
}

if [[ ! -f "$src_dir/CMakeLists.txt" ]]; then
    archive="$work_dir/fcitx5-qt-$version.tar.gz"
    if [[ ! -f "$archive" ]] || ! echo "$archive_sha  $archive" | sha256sum --check --status; then
        fetch "https://codeload.github.com/fcitx/fcitx5-qt/tar.gz/refs/tags/$version" "$archive"
    fi
    echo "$archive_sha  $archive" | sha256sum --check
    mkdir -p "$src_dir"
    tar -xzf "$archive" --strip-components=1 -C "$src_dir"
fi
if ! rg -q '^project\(fcitx5-qt VERSION 5\.1\.14\)' "$src_dir/CMakeLists.txt"; then
    echo "Expected fcitx5-qt 5.1.14 source in $src_dir" >&2
    exit 1
fi
# An extracted tarball may live inside TiPE's own Git repository. Give the
# dependency its own root so git apply cannot silently skip relative paths.
if [[ ! -d "$src_dir/.git" ]]; then
    git -C "$src_dir" init -q
fi
# Keep local edits; only apply when clean upstream or this exact patch is present.
if git -C "$src_dir" apply --check "$root_dir/integration/fcitx5-qt-tipe.patch" 2>/dev/null; then
    git -C "$src_dir" apply "$root_dir/integration/fcitx5-qt-tipe.patch"
elif ! git -C "$src_dir" apply --reverse --check "$root_dir/integration/fcitx5-qt-tipe.patch" 2>/dev/null; then
    echo "Qt source has a different patch; use a fresh TIPE_QT6_SOURCE_DIR." >&2
    exit 1
fi

qt_version="$(pkg-config --modversion Qt6Core)"
qt_headers="$(pkg-config --variable=includedir Qt6Core)"
deps_prefix="${TIPE_QT6_DEPS_PREFIX:-}"
if [[ -z "$deps_prefix" ]] && { [[ ! -f "$qt_headers/QtGui/$qt_version/QtGui/qpa/qplatforminputcontext.h" ]] || [[ ! -f "$qt_headers/QtWaylandClient/$qt_version/QtWaylandClient/private/qwaylandwindow_p.h" ]]; }; then
    # Fedora ships the private headers separately. Extract exactly the installed
    # runtime's RPM into the build tree; never upgrade the user's Qt packages.
    if ! command -v rpm >/dev/null || ! rpm -q qt6-qtbase >/dev/null 2>&1; then
        echo "Install matching Qt6 private development headers or set TIPE_QT6_DEPS_PREFIX." >&2
        exit 1
    fi
    deps_root="$work_dir/qt6-deps-$qt_version"
    deps_prefix="$deps_root/usr"
    mkdir -p "$work_dir/deps" "$deps_root"
    for package in qt6-qtbase qt6-qtwayland; do
        read -r rpm_version rpm_release rpm_arch < <(rpm -q --qf '%{VERSION} %{RELEASE} %{ARCH}\n' "$package")
        if [[ "$rpm_version" != "$qt_version" ]]; then
            echo "Mismatched Qt runtime versions: $package $rpm_version, Qt6Core $qt_version" >&2
            exit 1
        fi
        if [[ "$package" == qt6-qtbase ]]; then subpackage=qt6-qtbase-private-devel; else subpackage=qt6-qtwayland-devel; fi
        filename="$subpackage-$rpm_version-$rpm_release.$rpm_arch.rpm"
        rpm_path="$work_dir/deps/$filename"
        if ! rpm -K "$rpm_path" >/dev/null 2>&1; then
            fetch "https://kojipkgs.fedoraproject.org/packages/$package/$rpm_version/$rpm_release/$rpm_arch/$filename" "$rpm_path"
        fi
        rpm -K "$rpm_path"
        if [[ "$(rpm -qp --qf '%{NAME} %{VERSION} %{RELEASE} %{ARCH}' "$rpm_path")" != "$subpackage $rpm_version $rpm_release $rpm_arch" ]]; then
            echo "Unexpected Qt dependency package: $rpm_path" >&2
            exit 1
        fi
        (cd "$deps_root" && rpm2cpio "$rpm_path" | cpio -idm --quiet)
    done
    # Qt 6.11 puts Wayland's CMake interfaces in the installed runtime, while
    # its private headers are in the RPM above. Relocate only those interfaces.
    qt_cmake="$(pkg-config --variable=libdir Qt6Core)/cmake"
    for module in Qt6WaylandClientPrivate Qt6WaylandGlobalPrivate; do
        if [[ ! -d "$deps_prefix/lib64/cmake/$module" && -d "$qt_cmake/$module" ]]; then
            cp -a "$qt_cmake/$module" "$deps_prefix/lib64/cmake/"
        fi
    done
fi

extra_cmake=()
if [[ -n "$deps_prefix" ]]; then
    extra_cmake+=("-DCMAKE_PREFIX_PATH=$deps_prefix")
    for module in Qt6CorePrivate Qt6GuiPrivate Qt6WaylandClientPrivate Qt6WaylandGlobalPrivate; do
        if [[ -d "$deps_prefix/lib64/cmake/$module" ]]; then
            extra_cmake+=("-D${module}_DIR=$deps_prefix/lib64/cmake/$module")
        fi
    done
fi
cmake -S "$src_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_QT6PLUGINDIR="$prefix/lib/qt6/plugins" \
    -DENABLE_QT4=OFF -DENABLE_QT5=OFF -DENABLE_QT6=ON \
    -DBUILD_ONLY_PLUGIN=ON -DENABLE_QT6_WAYLAND_WORKAROUND=ON \
    -DTIPE_SOURCE_DIR="$root_dir" "${extra_cmake[@]}"
cmake --build "$build_dir" --target fcitx5platforminputcontextplugin-qt6 -j"${TIPE_BUILD_JOBS:-4}"
cmake --install "$build_dir"
echo "Qt6 TiPE frontend: $prefix/lib/qt6/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so"
echo "Launch a new Qt6 process with QT_PLUGIN_PATH=$prefix/lib/qt6/plugins QT_IM_MODULE=fcitx"
