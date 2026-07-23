#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/scripts/installed-files.sh"
if [[ -z "${HOME:-}" ]]; then
    echo "HOME is not set; cannot choose the TiPE user install prefix" >&2
    exit 1
fi

sanitize=0
if [[ "${1:-}" == "--sanitize" ]]; then
    sanitize=1
elif [[ $# -gt 0 ]]; then
    echo "usage: $0 [--sanitize]" >&2
    exit 2
fi

build_dir="$ROOT/build"
build_type=RelWithDebInfo
sanitize_option=OFF
if [[ $sanitize -eq 1 ]]; then
    build_dir="$ROOT/build-sanitize"
    build_type=Debug
    sanitize_option=ON
    cmake -E remove_directory "$build_dir"
fi
tested_stamp="$build_dir/.tipe-build-tested"
source_fingerprint_before_build=$(tipe_build_source_fingerprint "$ROOT")
clean_first=0
if [[ -f "$tested_stamp" ]]; then
    previous_tested_fingerprint=$(sed -n '1p' "$tested_stamp")
    if [[ ! "$previous_tested_fingerprint" =~ ^[0-9a-f]{64}$ ||
        "$previous_tested_fingerprint" != "$source_fingerprint_before_build" ]]; then
        clean_first=1
    fi
fi

jobs=${TIPE_BUILD_JOBS:-}
if [[ -z "$jobs" ]]; then
    jobs=1
    if command -v nproc >/dev/null 2>&1; then
        jobs=$(nproc)
    elif command -v getconf >/dev/null 2>&1; then
        jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')
    fi
    if ((jobs > 4)); then
        jobs=4
    fi
fi
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "TIPE_BUILD_JOBS must be a positive integer" >&2
    exit 2
fi
nice_value=${TIPE_BUILD_NICE:-5}
if [[ ! "$nice_value" =~ ^([0-9]|1[0-9])$ ]]; then
    echo "TIPE_BUILD_NICE must be an integer from 0 to 19" >&2
    exit 2
fi
low_priority=()
if ((nice_value > 0)) && command -v nice >/dev/null 2>&1; then
    low_priority=(nice -n "$nice_value")
fi
cmake -E rm -f "$tested_stamp"

cmake -S "$ROOT" -B "$build_dir" -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DTIPE_ENABLE_SANITIZERS="$sanitize_option"
build_args=(--build "$build_dir" -j "$jobs")
if [[ $clean_first -eq 1 ]]; then
    build_args+=(--clean-first)
fi
"${low_priority[@]}" cmake "${build_args[@]}"
if [[ $sanitize -eq 1 ]]; then
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "${low_priority[@]}" ctest --test-dir "$build_dir" --output-on-failure
else
    "${low_priority[@]}" ctest --test-dir "$build_dir" --output-on-failure
fi
tested_fingerprint=$(tipe_build_source_fingerprint "$ROOT")
if [[ ! "$tested_fingerprint" =~ ^[0-9a-f]{64}$ ]]; then
    echo "failed to fingerprint the tested TiPE source tree" >&2
    exit 1
fi
printf '%s\n' "$tested_fingerprint" >"$tested_stamp.tmp"
mv -f -- "$tested_stamp.tmp" "$tested_stamp"
