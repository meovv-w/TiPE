#!/usr/bin/env bash
set -euo pipefail

restart_script=${1:?restart helper path is required}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

fake_bin="$tmp_dir/bin"
state_dir="$tmp_dir/state"
home_dir="$tmp_dir/home"
mkdir -p "$fake_bin" "$state_dir" "$home_dir"

cat >"$fake_bin/fake-command" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

command_name=${0##*/}
state_dir=${TIPE_RESTART_TEST_STATE:?}

read_state() {
    local name=$1 fallback=$2
    if [[ -r "$state_dir/$name" ]]; then
        sed -n '1p' "$state_dir/$name"
    else
        printf '%s\n' "$fallback"
    fi
}

write_state() {
    printf '%s\n' "$2" >"$state_dir/$1"
}

case "$command_name" in
    systemctl)
        action=${2:-}
        case "$action" in
            cat)
                exit 0
                ;;
            stop)
                write_state active inactive
                write_state substate dead
                write_state main_pid 0
                write_state process_pid ""
                ;;
            unset-environment|set-environment)
                ;;
            start)
                write_state active active
                write_state substate running
                write_state main_pid 4242
                write_state process_pid 4242
                ;;
            show)
                properties=()
                value_only=0
                while [[ $# -gt 0 ]]; do
                    case "$1" in
                        --property)
                            properties+=("$2")
                            shift
                            ;;
                        --value)
                            value_only=1
                            ;;
                    esac
                    shift
                done
                for property in "${properties[@]}"; do
                    case "$property" in
                        ActiveState) value=$(read_state active inactive) ;;
                        SubState) value=$(read_state substate dead) ;;
                        MainPID) value=$(read_state main_pid 0) ;;
                        Result) value=success ;;
                        ExecMainStatus) value=0 ;;
                        *) value= ;;
                    esac
                    if [[ "$value_only" == "1" ]]; then
                        printf '%s\n' "$value"
                    else
                        printf '%s=%s\n' "$property" "$value"
                    fi
                done
                ;;
            *)
                exit 2
                ;;
        esac
        ;;
    pgrep)
        process_pid=$(read_state process_pid "")
        [[ -n "$process_pid" ]] || exit 1
        printf '%s\n' "$process_pid"
        ;;
    pkill)
        write_state process_pid ""
        ;;
    fcitx5-remote)
        if [[ "${TIPE_RESTART_TEST_CRASH_ON_ENABLE:-0}" == "1" && "${1:-}" == "-o" ]]; then
            write_state active inactive
            write_state substate dead
            write_state main_pid 0
            write_state process_pid 5252
        fi
        case "${1:-}" in
            -n)
                query_count=$(read_state remote_query_count 0)
                query_count=$((query_count + 1))
                write_state remote_query_count "$query_count"
                if [[ "${TIPE_RESTART_TEST_LATE_FALLBACK:-0}" == "1" && "$query_count" -ge 4 ]]; then
                    printf 'keyboard-us\n'
                else
                    printf 'tipe\n'
                fi
                ;;
            -m) printf 'tipe\n' ;;
            -o|-s) ;;
            *) exit 2 ;;
        esac
        ;;
    sleep)
        ;;
    *)
        exit 127
        ;;
esac
EOF
chmod +x "$fake_bin/fake-command"
for command in systemctl pgrep pkill fcitx5-remote sleep; do
    ln -s fake-command "$fake_bin/$command"
done

reset_state() {
    printf 'active\n' >"$state_dir/active"
    printf 'running\n' >"$state_dir/substate"
    printf '1111\n' >"$state_dir/main_pid"
    printf '1111\n' >"$state_dir/process_pid"
    printf '0\n' >"$state_dir/remote_query_count"
}

reset_state
success_output=$(
    HOME="$home_dir" PATH="$fake_bin:$PATH" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/tipe-test-bus \
        TIPE_RESTART_TEST_STATE="$state_dir" "$restart_script" 2>&1
)
if [[ "$success_output" != *"fcitx5 service MainPID: 4242"* ||
    "$success_output" != *"fcitx5 process is running with verified identity"* ||
    "$success_output" != *"current input method: tipe"* ]]; then
    echo "restart helper should keep and verify the service MainPID" >&2
    exit 1
fi

reset_state
late_fallback_output=$(
    HOME="$home_dir" PATH="$fake_bin:$PATH" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/tipe-test-bus \
        TIPE_RESTART_TEST_STATE="$state_dir" TIPE_RESTART_TEST_LATE_FALLBACK=1 \
        "$restart_script" 2>&1
)
if [[ "$late_fallback_output" != *"current input method: keyboard-us"* ||
    "$late_fallback_output" != *"warning: TiPE was not activated after restart"* ||
    "$late_fallback_output" == *"current input method: tipe"* ]]; then
    echo "restart helper should report a post-command input-method fallback" >&2
    exit 1
fi

reset_state
set +e
failure_output=$(
    HOME="$home_dir" PATH="$fake_bin:$PATH" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/tipe-test-bus \
        TIPE_RESTART_TEST_STATE="$state_dir" TIPE_RESTART_TEST_CRASH_ON_ENABLE=1 \
        "$restart_script" 2>&1
)
failure_status=$?
set -e
if (( failure_status == 0 )); then
    echo "restart helper should reject a D-Bus-activated replacement process" >&2
    exit 1
fi
if [[ "$failure_output" != *"expected MainPID 4242"* ||
    "$failure_output" != *"processes=5252"* ||
    "$failure_output" != *"refusing to accept a different D-Bus-activated fcitx5 process"* ]]; then
    echo "restart helper should explain the service/replacement identity mismatch" >&2
    exit 1
fi

echo "restart helper tests passed"
