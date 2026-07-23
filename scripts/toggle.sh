#!/usr/bin/env bash
# Keep TiPE active and toggle its internal Chinese/English input mode.
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [--status | --set chinese|english]

Without arguments, toggles TiPE's internal Chinese/English mode. TiPE remains
the active fcitx5 input method so pass-through English keys can be supervised.
EOF
}

if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
    mode_dir="$XDG_RUNTIME_DIR/tipe"
else
    mode_dir="${XDG_CACHE_HOME:-${HOME:?HOME is not set}/.cache}/tipe"
fi
mode_file="$mode_dir/input-mode"
applied_file="$mode_dir/input-mode-applied"
temporary=""

cleanup() {
    if [[ -n "${temporary:-}" ]]; then
        rm -f -- "$temporary"
    fi
}

read_mode() {
    local value="chinese" ignored=""
    if [[ -r "$mode_file" ]]; then
        read -r value ignored <"$mode_file" || value="chinese"
    fi
    case "${value,,}" in
        english|eng)
            printf 'english\n'
            ;;
        *)
            printf 'chinese\n'
            ;;
    esac
}

write_mode() {
    local value="$1"
    local request_id="$2"
    mkdir -p "$mode_dir"
    chmod 0700 "$mode_dir" 2>/dev/null || true
    temporary="$mode_file.tmp.$$"
    trap cleanup EXIT
    (umask 077; printf '%s\t%s\n' "$value" "$request_id" >"$temporary")
    mv -f -- "$temporary" "$mode_file"
    temporary=""
    trap - EXIT
}

activate_tipe() {
    local attempt stable=0
    for attempt in {1..40}; do
        state=$(fcitx5-remote 2>/dev/null || true)
        current_method=$(fcitx5-remote -n 2>/dev/null || true)
        if [[ "$state" == "2" && "$current_method" == "tipe" ]]; then
            stable=$((stable + 1))
            if (( stable >= 3 )); then
                return 0
            fi
        else
            stable=0
            if [[ "$state" != "2" ]]; then
                fcitx5-remote -o >/dev/null 2>&1 || true
            fi
            if [[ "$current_method" != "tipe" ]]; then
                fcitx5-remote -s tipe >/dev/null 2>&1 || true
            fi
        fi
        sleep 0.05
    done
    return 1
}

wait_for_applied_mode() {
    local requested_mode="$1"
    local request_id="$2"
    local attempt applied_mode="" applied_id=""
    for attempt in {1..80}; do
        if [[ -r "$applied_file" ]]; then
            read -r applied_mode applied_id <"$applied_file" || true
            if [[ "$applied_mode" == "$requested_mode" && "$applied_id" == "$request_id" ]]; then
                return 0
            fi
        fi
        if (( attempt % 4 == 0 )); then
            state=$(fcitx5-remote 2>/dev/null || true)
            current_method=$(fcitx5-remote -n 2>/dev/null || true)
            if [[ "$state" != "2" || "$current_method" != "tipe" ]]; then
                activate_tipe || return 1
            fi
        fi
        sleep 0.025
    done
    return 1
}

requested=""
case "${1:-}" in
    "")
        ;;
    --status)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        read_mode
        exit 0
        ;;
    --set)
        [[ $# -eq 2 ]] || { usage >&2; exit 2; }
        case "${2,,}" in
            chinese|zh)
                requested="chinese"
                ;;
            english|eng|en)
                requested="english"
                ;;
            *)
                echo "invalid TiPE input mode: $2" >&2
                exit 2
                ;;
        esac
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if ! command -v fcitx5-remote >/dev/null 2>&1; then
    echo "fcitx5-remote not found" >&2
    exit 1
fi

state=$(fcitx5-remote 2>/dev/null || true)
current_method=$(fcitx5-remote -n 2>/dev/null || true)
current_mode=$(read_mode)
if [[ -z "$requested" ]]; then
    if [[ "$state" != "2" || "$current_method" != "tipe" ]]; then
        requested="chinese"
    elif [[ "$current_mode" == "english" ]]; then
        requested="chinese"
    else
        requested="english"
    fi
fi

if ! activate_tipe; then
    echo "TiPE could not be activated and kept stable" >&2
    exit 1
fi

request_id="${BASHPID:-$$}.${RANDOM}.${RANDOM}"
write_mode "$requested" "$request_id"
if ! wait_for_applied_mode "$requested" "$request_id"; then
    echo "TiPE was activated, but its engine did not confirm the requested mode" >&2
    exit 1
fi

if ! activate_tipe; then
    echo "TiPE applied the mode, but did not remain active" >&2
    exit 1
fi
