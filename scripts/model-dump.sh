#!/usr/bin/env bash
set -euo pipefail

default_dump_path() {
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        printf '%s\n' "$XDG_CACHE_HOME/tipe/model-request.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.cache/tipe/model-request.tsv"
    else
        printf '%s\n' "/tmp/tipe/model-request.tsv"
    fi
}

dump_path="${TIPE_MODEL_DUMP_PATH:-$(default_dump_path)}"
dump_dir=$(dirname -- "$dump_path")
dump_name=$(basename -- "$dump_path")
umask 077
mkdir -p "$dump_dir"
temporary=$(mktemp "$dump_dir/.${dump_name}.tmp.XXXXXX")
cleanup() {
    rm -f -- "$temporary"
}
trap cleanup EXIT
cat >"$temporary"
chmod 0600 "$temporary"
mv -f -- "$temporary" "$dump_path"
temporary=""
trap - EXIT

if [[ -n "${TIPE_MODEL_DUMP_RESPONSE:-}" ]]; then
    printf '%b' "$TIPE_MODEL_DUMP_RESPONSE"
fi
