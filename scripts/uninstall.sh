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

niri_keybinds="${TIPE_NIRI_KEYBINDS:-$user_home/.config/niri/keybinds.kdl}"
if [[ -r "$niri_keybinds" ]] && grep -Eq 'tipe-toggle([[:space:]";}]|$)' "$niri_keybinds"; then
    printf 'warning\t%s\n' \
        "$niri_keybinds still calls tipe-toggle; change that shortcut before removal or it will stop working"
fi

while IFS= read -r installed_file; do
    case "$installed_file" in
        "$user_home"/.local/*)
            ;;
        *)
            printf 'skipped\t%s\n' "$installed_file"
            continue
            ;;
    esac
    if [[ $dry_run -eq 1 ]]; then
        if [[ -e "$installed_file" ]]; then
            printf 'present\t%s\n' "$installed_file"
        else
            printf 'missing\t%s\n' "$installed_file"
        fi
    else
        if [[ -e "$installed_file" ]]; then
            rm -f "$installed_file"
            printf 'removed\t%s\n' "$installed_file"
        else
            printf 'missing\t%s\n' "$installed_file"
        fi
    fi
done < <(tipe_installed_files)
if [[ $dry_run -eq 1 ]]; then
    echo "Dry run only; no TiPE user install files were removed."
else
    echo "TiPE user install files removed. Restart fcitx5 to unload it."
fi
