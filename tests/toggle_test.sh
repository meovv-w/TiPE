#!/usr/bin/env bash
set -euo pipefail

[[ $# -eq 1 ]] || { echo "expected toggle helper path" >&2; exit 2; }
toggle=$1
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
mkdir -p "$temporary/bin" "$temporary/runtime"

cat >"$temporary/bin/fcitx5-remote" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$TIPE_TOGGLE_TEST_LOG"

maybe_ack_mode() {
    local request_file="$XDG_RUNTIME_DIR/tipe/input-mode"
    local applied_file="$XDG_RUNTIME_DIR/tipe/input-mode-applied"
    local mode="" token="" previous="" count=0
    [[ -r "$request_file" ]] || return 0
    read -r mode token <"$request_file" || return 0
    [[ -n "$token" ]] || return 0
    if [[ -r "$TIPE_TOGGLE_TEST_ACK_TOKEN" ]]; then
        read -r previous <"$TIPE_TOGGLE_TEST_ACK_TOKEN" || true
    fi
    if [[ "$previous" != "$token" ]]; then
        printf '%s\n' "$token" >"$TIPE_TOGGLE_TEST_ACK_TOKEN"
        printf '0\n' >"$TIPE_TOGGLE_TEST_ACK_COUNTER"
    elif [[ -r "$TIPE_TOGGLE_TEST_ACK_COUNTER" ]]; then
        read -r count <"$TIPE_TOGGLE_TEST_ACK_COUNTER" || count=0
    fi
    count=$((count + 1))
    printf '%s\n' "$count" >"$TIPE_TOGGLE_TEST_ACK_COUNTER"
    if (( count >= ${TIPE_TOGGLE_TEST_ACK_DELAY:-2} )); then
        printf '%s\t%s\n' "$mode" "$token" >"$applied_file"
    fi
}

maybe_ack_mode
case "${1:-}" in
    -n)
        cat "$TIPE_TOGGLE_TEST_METHOD"
        ;;
    -o)
        printf '2\n' >"$TIPE_TOGGLE_TEST_STATE"
        ;;
    -s)
        [[ $# -eq 2 ]]
        printf '%s\n' "$2" >"$TIPE_TOGGLE_TEST_METHOD"
        ;;
    *)
        cat "$TIPE_TOGGLE_TEST_STATE"
        ;;
esac
EOF
cat >"$temporary/bin/notify-send" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$TIPE_TOGGLE_NOTIFY_LOG"
exit 0
EOF
chmod +x "$temporary/bin/fcitx5-remote" "$temporary/bin/notify-send"

export PATH="$temporary/bin:$PATH"
export XDG_RUNTIME_DIR="$temporary/runtime"
export TIPE_TOGGLE_TEST_LOG="$temporary/remote.log"
export TIPE_TOGGLE_NOTIFY_LOG="$temporary/notify.log"
export TIPE_TOGGLE_TEST_STATE="$temporary/fcitx-state"
export TIPE_TOGGLE_TEST_METHOD="$temporary/fcitx-method"
export TIPE_TOGGLE_TEST_ACK_COUNTER="$temporary/ack-counter"
export TIPE_TOGGLE_TEST_ACK_TOKEN="$temporary/ack-token"
export TIPE_TOGGLE_TEST_ACK_DELAY=2

printf '2\n' >"$TIPE_TOGGLE_TEST_STATE"
printf 'tipe\n' >"$TIPE_TOGGLE_TEST_METHOD"

[[ "$($toggle --status)" == "chinese" ]]
$toggle
[[ "$($toggle --status)" == "english" ]]
read -r written_mode written_token <"$XDG_RUNTIME_DIR/tipe/input-mode"
[[ "$written_mode" == "english" && -n "$written_token" ]]
read -r applied_mode applied_token <"$XDG_RUNTIME_DIR/tipe/input-mode-applied"
[[ "$applied_mode" == "$written_mode" && "$applied_token" == "$written_token" ]]
$toggle --set chinese
[[ "$($toggle --status)" == "chinese" ]]
read -r written_mode written_token <"$XDG_RUNTIME_DIR/tipe/input-mode"
[[ "$written_mode" == "chinese" && -n "$written_token" ]]

printf 'english\n' >"$XDG_RUNTIME_DIR/tipe/input-mode"
printf 'keyboard-us\n' >"$TIPE_TOGGLE_TEST_METHOD"
$toggle
[[ "$($toggle --status)" == "chinese" ]]
[[ "$(cat "$TIPE_TOGGLE_TEST_METHOD")" == "tipe" ]]
[[ "$(cat "$TIPE_TOGGLE_TEST_STATE")" == "2" ]]

export TIPE_TOGGLE_TEST_ACK_DELAY=6
rm -f -- "$TIPE_TOGGLE_TEST_ACK_COUNTER" "$TIPE_TOGGLE_TEST_ACK_TOKEN"
$toggle --set english
[[ "$(cat "$TIPE_TOGGLE_TEST_ACK_COUNTER")" -ge 6 ]]
read -r written_mode written_token <"$XDG_RUNTIME_DIR/tipe/input-mode"
read -r applied_mode applied_token <"$XDG_RUNTIME_DIR/tipe/input-mode-applied"
[[ "$written_mode" == "english" && "$applied_mode" == "$written_mode" && "$applied_token" == "$written_token" ]]
export TIPE_TOGGLE_TEST_ACK_DELAY=2

printf 'english\n' >"$XDG_RUNTIME_DIR/tipe/input-mode"
printf '1\n' >"$TIPE_TOGGLE_TEST_STATE"
printf 'tipe\n' >"$TIPE_TOGGLE_TEST_METHOD"
$toggle
[[ "$($toggle --status)" == "chinese" ]]
[[ "$(cat "$TIPE_TOGGLE_TEST_METHOD")" == "tipe" ]]
[[ "$(cat "$TIPE_TOGGLE_TEST_STATE")" == "2" ]]

if [[ -e "$TIPE_TOGGLE_NOTIFY_LOG" ]]; then
    echo "toggle helper must use TiPE's own mode indicator instead of desktop notifications" >&2
    exit 1
fi
if rg -q -- '(^| )-c($| )' "$TIPE_TOGGLE_TEST_LOG"; then
    echo "toggle helper must not deactivate fcitx5" >&2
    exit 1
fi
if ! rg -q -- '-s tipe' "$TIPE_TOGGLE_TEST_LOG" && ! rg -q '^$' "$TIPE_TOGGLE_TEST_LOG"; then
    echo "toggle helper did not verify TiPE activation" >&2
    exit 1
fi

touch "$temporary/not-a-directory"
if XDG_RUNTIME_DIR="$temporary/not-a-directory" "$toggle" --set english \
    >"$temporary/unwritable.out" 2>"$temporary/unwritable.err"; then
    echo "toggle helper should fail when its mode directory cannot be created" >&2
    exit 1
fi
if rg -q 'unbound variable' "$temporary/unwritable.err"; then
    echo "toggle helper cleanup must remain safe after a mode-file write failure" >&2
    exit 1
fi

echo "toggle helper ok"
