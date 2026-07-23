#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/scripts/installed-files.sh"
user_home=$(tipe_user_home)

dry_run=0
if [[ "${1:-}" == "--dry-run" ]]; then
    dry_run=1
elif [[ $# -gt 0 ]]; then
    echo "usage: $0 [--dry-run]" >&2
    exit 2
fi

required_build_files=(
    "$ROOT/build/libtipe.so"
    "$ROOT/build/libtipeui.so"
    "$ROOT/build/tipe-state-probe"
    "$ROOT/build/tipe-candidate-window"
    "$ROOT/build/tipe-learning-panel-window"
)
mingw_compiler=$(sed -n 's/^TIPE_MINGW_CXX_EXECUTABLE:FILEPATH=//p' "$ROOT/build/CMakeCache.txt" 2>/dev/null || true)
if [[ -f "$ROOT/build/tipe-wine-caret-bridge.exe" ||
      ( -n "$mingw_compiler" && "$mingw_compiler" != *-NOTFOUND ) ]]; then
    required_build_files+=("$ROOT/build/tipe-wine-caret-bridge.exe")
fi
for build_file in "${required_build_files[@]}"; do
    if [[ ! -e "$build_file" ]]; then
        echo "TiPE build output is missing: $build_file" >&2
        echo "Run ./scripts/build.sh first." >&2
        exit 1
    fi
done

tested_stamp="$ROOT/build/.tipe-build-tested"
if [[ ! -f "$tested_stamp" ]]; then
    echo "TiPE build has not completed its test suite: $tested_stamp" >&2
    echo "Run ./scripts/build.sh before installing." >&2
    exit 1
fi
tested_fingerprint=$(sed -n '1p' "$tested_stamp")
current_fingerprint=$(tipe_build_source_fingerprint "$ROOT")
if [[ ! "$tested_fingerprint" =~ ^[0-9a-f]{64}$ || "$current_fingerprint" != "$tested_fingerprint" ]]; then
    echo "TiPE source content changed after the last tested build." >&2
    echo "Run ./scripts/build.sh before installing." >&2
    exit 1
fi

required_source_files=(
    "$ROOT/addon/tipe.conf"
    "$ROOT/addon/tipeui.conf"
    "$ROOT/addon/tipe-inputmethod.conf"
    "$ROOT/data/tipe-supervision.desktop"
    "$ROOT/data/support/wechat.png"
    "$ROOT/data/support/alipay.png"
)
while IFS=$'\t' read -r source_file _destination; do
    required_source_files+=("$source_file")
done < <(tipe_helper_install_pairs "$ROOT")
for source_file in "${required_source_files[@]}"; do
    if [[ ! -f "$source_file" || ! -r "$source_file" ]]; then
        echo "TiPE install source is missing or unreadable: $source_file" >&2
        exit 1
    fi
done
while IFS=$'\t' read -r source_file _destination; do
    if [[ ! -x "$source_file" ]]; then
        echo "TiPE helper is not executable: $source_file" >&2
        exit 1
    fi
done < <(tipe_helper_install_pairs "$ROOT")

if [[ $dry_run -eq 1 ]]; then
    printf 'dry-run\tprefix\t%s\n' "$user_home/.local"
    while IFS= read -r installed_file; do
        printf 'would-install\t%s\n' "$installed_file"
    done < <(TIPE_SOURCE_ROOT="$ROOT" tipe_installed_files)
    echo "Dry run only; no TiPE files were installed and fcitx5 was not restarted."
    exit 0
fi

install_stage=$(mktemp -d "${TMPDIR:-/tmp}/tipe-install.XXXXXX")
install_temporary=""
cleanup_install() {
    if [[ -n "$install_temporary" && -e "$install_temporary" ]]; then
        rm -f -- "$install_temporary"
    fi
    if [[ -n "$install_stage" && -d "$install_stage" ]]; then
        rm -rf -- "$install_stage"
    fi
}
trap cleanup_install EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

atomic_install_file() {
    local source_file="$1"
    local destination="$2"
    local mode="$3"
    local destination_dir destination_name
    destination_dir=$(dirname -- "$destination")
    destination_name=$(basename -- "$destination")
    mkdir -p -- "$destination_dir"
    install_temporary=$(mktemp "$destination_dir/.${destination_name}.tmp.XXXXXX")
    install -m "$mode" -- "$source_file" "$install_temporary"
    mv -fT -- "$install_temporary" "$destination"
    install_temporary=""
}

# Stage the complete CMake install before touching a live user installation.
# Each final file is then replaced by a same-directory rename, so a running
# fcitx5 process can never fault in a partially overwritten shared object.
DESTDIR="$install_stage" cmake --install "$ROOT/build" --prefix "$user_home/.local"
if [[ ! -s "$ROOT/build/install_manifest.txt" ]]; then
    echo "CMake did not produce an install manifest" >&2
    exit 1
fi

cmake_install_destinations=()
while IFS= read -r destination || [[ -n "$destination" ]]; do
    [[ -n "$destination" ]] || continue
    cmake_install_destinations+=("$destination")
    case "$destination" in
        "$user_home"/.local/*)
            ;;
        *)
            echo "CMake install destination escaped the TiPE user prefix: $destination" >&2
            exit 1
            ;;
    esac
    staged_file="$install_stage$destination"
    if [[ ! -f "$staged_file" ]]; then
        echo "Staged CMake install file is missing: $staged_file" >&2
        exit 1
    fi
done <"$ROOT/build/install_manifest.txt"
if (( ${#cmake_install_destinations[@]} == 0 )); then
    echo "CMake produced an empty install manifest" >&2
    exit 1
fi

staged_tipe_metadata="$install_stage/tipe.conf"
staged_tipeui_metadata="$install_stage/tipeui.conf"
sed "s|^Library=.*|Library=$user_home/.local/share/fcitx5/addon/libtipe|" \
    "$ROOT/addon/tipe.conf" >"$staged_tipe_metadata"
sed "s|^Library=.*|Library=$user_home/.local/share/fcitx5/addon/libtipeui|" \
    "$ROOT/addon/tipeui.conf" >"$staged_tipeui_metadata"
if ! grep -qxF "Library=$user_home/.local/share/fcitx5/addon/libtipe" "$staged_tipe_metadata" ||
    ! grep -qxF "Library=$user_home/.local/share/fcitx5/addon/libtipeui" "$staged_tipeui_metadata"; then
    echo "Failed to stage TiPE addon metadata for the user install prefix" >&2
    exit 1
fi

# Validation above is deliberately separate from replacement. A malformed or
# incomplete staged install must leave every existing live file untouched.
for destination in "${cmake_install_destinations[@]}"; do
    staged_file="$install_stage$destination"
    staged_mode=0644
    if [[ -x "$staged_file" ]]; then
        staged_mode=0755
    fi
    atomic_install_file "$staged_file" "$destination" "$staged_mode"
done

while IFS=$'\t' read -r source_file destination; do
    atomic_install_file "$source_file" "$destination" 0755
done < <(tipe_helper_install_pairs "$ROOT")

atomic_install_file "$ROOT/build/libtipe.so" "$user_home/.local/lib/fcitx5/libtipe.so" 0755
atomic_install_file "$ROOT/build/libtipe.so" "$user_home/.local/lib64/fcitx5/libtipe.so" 0755
atomic_install_file "$ROOT/build/libtipe.so" "$user_home/.local/share/fcitx5/addon/libtipe.so" 0755
atomic_install_file "$ROOT/build/libtipeui.so" "$user_home/.local/lib/fcitx5/libtipeui.so" 0755
atomic_install_file "$ROOT/build/libtipeui.so" "$user_home/.local/lib64/fcitx5/libtipeui.so" 0755
atomic_install_file "$ROOT/build/libtipeui.so" "$user_home/.local/share/fcitx5/addon/libtipeui.so" 0755

atomic_install_file "$staged_tipe_metadata" "$user_home/.local/share/fcitx5/addon/tipe.conf" 0644
atomic_install_file "$staged_tipeui_metadata" "$user_home/.local/share/fcitx5/addon/tipeui.conf" 0644

echo "TiPE files installed under the CMake install prefix."
if [[ -e "$ROOT/build/install_manifest.txt" ]]; then
    echo "CMake installed files:"
    sed 's/^/  /' "$ROOT/build/install_manifest.txt"
    printf '\n'
fi
echo "TiPE managed user install files:"
while IFS= read -r installed_file; do
    if [[ -e "$installed_file" ]]; then
        printf '  %s\n' "$installed_file"
    fi
done < <(tipe_installed_files)
echo "Restart fcitx5 and switch to TiPE only after confirming you want to change the current input session."
