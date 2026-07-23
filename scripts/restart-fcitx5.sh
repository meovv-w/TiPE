#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
use_model_example=0
use_model_adapter=0
use_model_current=1
explicit_model_option_count=0
debug=0
dry_run=0
ui_addon=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model-example)
            explicit_model_option_count=$((explicit_model_option_count + 1))
            use_model_example=1
            use_model_current=0
            ;;
        --model-adapter)
            explicit_model_option_count=$((explicit_model_option_count + 1))
            use_model_adapter=1
            use_model_current=0
            ;;
        --model-current)
            explicit_model_option_count=$((explicit_model_option_count + 1))
            use_model_current=1
            ;;
        --no-model)
            explicit_model_option_count=$((explicit_model_option_count + 1))
            use_model_current=0
            ;;
        --debug)
            debug=1
            ;;
        --dry-run)
            dry_run=1
            ;;
        --ui)
            if [[ $# -lt 2 ]]; then
                echo "--ui requires an addon name" >&2
                exit 2
            fi
            ui_addon="$2"
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [--model-example|--model-adapter|--model-current|--no-model] [--debug] [--ui ADDON] [--dry-run]

Restarts the current fcitx5 process and tries to switch the active input method to TiPE.
Run this only when changing the current input session is intentional.

  (default)        load tipe-model-current, which reads ~/.config/tipe/model-env
  --model-example  use the installed example wrapper instead
  --model-adapter  use the installed adapter wrapper instead
  --model-current  explicitly select the default tipe-model-current wrapper
  --no-model       restart without a model command
  --debug          enable TiPE cursor/candidate debug logs
  --ui ADDON       start fcitx5 with a specific UI addon for this run, e.g. tipeui
  --dry-run        print the planned restart command without changing fcitx5
EOF
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

if (( explicit_model_option_count > 1 )); then
    echo "--model-example, --model-adapter, --model-current, and --no-model cannot be used together" >&2
    exit 2
fi
model_option_count=$((use_model_example + use_model_adapter + use_model_current))
if [[ -n "$ui_addon" && ! "$ui_addon" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "--ui addon name may only contain letters, numbers, underscore, and hyphen" >&2
    exit 2
fi

if [[ -z "${HOME:-}" ]]; then
    echo "HOME is not set; cannot restart fcitx5 for the TiPE user session" >&2
    exit 1
fi

LOG_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/tipe"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/fcitx5.log"
LIVE_SUPERVISION_FILE="$LOG_DIR/supervision-current.tsv"
MODEL_CONFIG_FILE="${TIPE_MODEL_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/tipe/model-env}"

config_export_value() {
    local name="$1"
    local file="$2"
    [[ -r "$file" ]] || return 1
    sed -n "s/^export $name=['\"]\\{0,1\\}\\([^'\"]*\\)['\"]\\{0,1\\}$/\\1/p" "$file" | sed -n '1p'
}

have_session_dbus() {
    [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]]
}

safe_fcitx5_remote() {
    if ! have_session_dbus; then
        return 2
    fi
    local output status
    set +e
    output=$(fcitx5-remote "$@" 2>&1)
    status=$?
    set -e
    if (( status == 0 )); then
        printf '%s\n' "$output"
        return 0
    fi
    if [[ "$output" == *"Failed to create dbus connection"* ||
          "$output" == *"Cannot autolaunch D-Bus"* ||
          "$output" == *"Could not connect"* ]]; then
        return 2
    fi
    printf '%s\n' "$output" >&2
    return "$status"
}

service_property() {
    local property="$1"
    systemctl --user show fcitx5.service --property "$property" --value 2>/dev/null || true
}

fcitx5_pids() {
    pgrep -x fcitx5 2>/dev/null || true
}

report_service_failure() {
    local stage="$1"
    echo "fcitx5 user service failed runtime verification: $stage" >&2
    systemctl --user show fcitx5.service --property ActiveState --property SubState \
        --property MainPID --property Result --property ExecMainStatus --no-pager >&2 || true
    echo "refusing to accept a different D-Bus-activated fcitx5 process as the restarted service" >&2
}

expected_runtime_pid=""
use_systemd_launch=0

verify_runtime_identity() {
    local stage="$1"
    local active_state main_pid pid_list pid_count only_pid
    pid_list=$(fcitx5_pids)
    pid_count=$(awk 'NF { count++ } END { print count + 0 }' <<<"$pid_list")
    only_pid=$(awk 'NF { print; exit }' <<<"$pid_list")

    if [[ "$use_systemd_launch" == "1" ]]; then
        active_state=$(service_property ActiveState)
        main_pid=$(service_property MainPID)
        if [[ "$active_state" != "active" || ! "$main_pid" =~ ^[1-9][0-9]*$ ||
            -z "$expected_runtime_pid" || "$main_pid" != "$expected_runtime_pid" ||
            "$pid_count" != "1" || "$only_pid" != "$expected_runtime_pid" ]]; then
            report_service_failure "$stage (expected MainPID ${expected_runtime_pid:-unset}, active=${active_state:-unknown}, MainPID=${main_pid:-0}, processes=${pid_list:-none})"
            return 1
        fi
        return 0
    fi

    if [[ -z "$expected_runtime_pid" || "$pid_count" != "1" || "$only_pid" != "$expected_runtime_pid" ||
        ! -r "/proc/$expected_runtime_pid/status" ]]; then
        echo "direct fcitx5 process failed runtime verification: $stage" >&2
        echo "expected PID: ${expected_runtime_pid:-unset}; running fcitx5 PIDs: ${pid_list:-none}" >&2
        return 1
    fi
}

require_runtime_identity() {
    verify_runtime_identity "$1" || exit 1
}

wayland_session_reachable() {
    if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
        return 1
    fi
    if command -v wayland-info >/dev/null 2>&1; then
        wayland-info >/dev/null 2>&1
        return $?
    fi
    local socket_path="$WAYLAND_DISPLAY"
    if [[ "$socket_path" != /* ]]; then
        [[ -n "${XDG_RUNTIME_DIR:-}" ]] || return 1
        socket_path="$XDG_RUNTIME_DIR/$socket_path"
    fi
    [[ -S "$socket_path" ]]
}

env_args=()
candidate_edge_fallback="${TIPE_WAYLAND_POPUP_EDGE_FALLBACK:-0}"
status_edge_fallback="${TIPE_STATUS_EDGE_FALLBACK:-0}"
if [[ ! "$candidate_edge_fallback" =~ ^(0|1|true|false|on|off)$ ]]; then
    echo "TIPE_WAYLAND_POPUP_EDGE_FALLBACK must be 0/1, true/false, or on/off" >&2
    exit 2
fi
if [[ ! "$status_edge_fallback" =~ ^(0|1|true|false|on|off)$ ]]; then
    echo "TIPE_STATUS_EDGE_FALLBACK must be 0/1, true/false, or on/off" >&2
    exit 2
fi
env_args+=("TIPE_WAYLAND_POPUP_EDGE_FALLBACK=$candidate_edge_fallback")
env_args+=("TIPE_STATUS_EDGE_FALLBACK=$status_edge_fallback")
if [[ "$use_model_example" == "1" ]]; then
    if [[ -x "$HOME/.local/bin/tipe-model-protocol-example" ]]; then
        env_args+=("TIPE_MODEL_COMMAND=$HOME/.local/bin/tipe-model-protocol-example")
    else
        env_args+=("TIPE_MODEL_COMMAND=$ROOT/scripts/model-protocol-example.sh")
    fi
fi
if [[ "$use_model_adapter" == "1" ]]; then
    if [[ -x "$HOME/.local/bin/tipe-model-adapter" ]]; then
        env_args+=("TIPE_MODEL_COMMAND=$HOME/.local/bin/tipe-model-adapter")
    else
        env_args+=("TIPE_MODEL_COMMAND=$ROOT/scripts/model-adapter.sh")
    fi
fi
if [[ "$use_model_current" == "1" ]]; then
    if [[ -x "$HOME/.local/bin/tipe-model-current" ]]; then
        env_args+=("TIPE_MODEL_COMMAND=$HOME/.local/bin/tipe-model-current")
    else
        env_args+=("TIPE_MODEL_COMMAND=$ROOT/scripts/model-current.sh")
    fi
    env_args+=("TIPE_MODEL_CONFIG=$MODEL_CONFIG_FILE")
    if continuous_default=$(config_export_value TIPE_CONTINUOUS_MODE "$MODEL_CONFIG_FILE" || true);
        [[ "$continuous_default" =~ ^(0|1|true|false|on|off)$ ]]; then
        env_args+=("TIPE_CONTINUOUS_MODE=$continuous_default")
    fi
    model_timeout=$(config_export_value TIPE_MODEL_TIMEOUT_SECONDS "$MODEL_CONFIG_FILE" || true)
    if [[ -n "$model_timeout" ]]; then
        if [[ ! "$model_timeout" =~ ^([1-9]|[12][0-9]|30)$ ]]; then
            echo "invalid TIPE_MODEL_TIMEOUT_SECONDS in $MODEL_CONFIG_FILE: $model_timeout" >&2
            exit 2
        fi
        # The engine enforces this timeout before model-current can source its config.
        env_args+=("TIPE_MODEL_TIMEOUT_SECONDS=$model_timeout")
    fi
fi
if [[ "$debug" == "1" ]]; then
    env_args+=("TIPE_DEBUG=1" "TIPE_CANDIDATE_DEBUG=1")
fi
fcitx_args=(-r)
if [[ -n "$ui_addon" ]]; then
    fcitx_args+=(--ui "$ui_addon")
fi
fcitx_args+=(--verbose default=5)

if [[ "$dry_run" == "1" ]]; then
    echo "Dry run only; fcitx5 will not be restarted and the active input method will not be changed."
    echo "direct fallback fcitx5 log: $LOG_FILE"
    echo "user service fcitx5 log: journalctl --user -u fcitx5.service"
    echo "live supervision reset: $LIVE_SUPERVISION_FILE"
    if [[ "$use_model_current" == "1" ]]; then
        echo "model config: $MODEL_CONFIG_FILE"
        if [[ -r "$MODEL_CONFIG_FILE" ]]; then
            echo "model config status: present"
        else
            echo "model config status: missing; tipe-model-current will use offline heuristic defaults"
        fi
    fi
    printf 'planned command: nohup env'
    for env_arg in "${env_args[@]}"; do
        printf ' %q' "$env_arg"
    done
    printf ' fcitx5'
    for fcitx_arg in "${fcitx_args[@]}"; do
        printf ' %q' "$fcitx_arg"
    done
    printf ' >%q 2>&1 &\n' "$LOG_FILE"
    echo "preferred launch: set TiPE variables in the user systemd manager, stop any old fcitx5 instance, then start fcitx5.service and hold its exact MainPID through verification"
    echo "fallback launch: the nohup command above when fcitx5.service is unavailable or --ui is used"
    echo "direct launch preflight: require the current fcitx5 D-Bus endpoint and Wayland compositor to be reachable before stopping anything"
    if have_session_dbus; then
        echo "planned switch: fcitx5-remote -o; fcitx5-remote -s tipe; verify with fcitx5-remote -n, retry, settle-check, then final fcitx5-remote -s tipe"
    else
        echo "planned switch: skipped because DBUS_SESSION_BUS_ADDRESS is not set"
    fi
    echo "planned service verification: require active fcitx5.service, one unchanged MainPID, and no D-Bus-activated replacement before and after each switch step"
    echo "planned verification: tipe-doctor | grep -E 'runtime[[:space:]]+model-(command|config-active)'"
    exit 0
fi

if ! have_session_dbus; then
    echo "DBUS_SESSION_BUS_ADDRESS is not set; refusing to restart fcitx5 from a restricted shell" >&2
    echo "Run this helper from the real user session, or restart fcitx5.service with systemctl --user." >&2
    exit 1
fi

if [[ -z "$ui_addon" ]] && systemctl --user cat fcitx5.service >/dev/null 2>&1; then
    use_systemd_launch=1
fi

if [[ "$use_systemd_launch" != "1" ]]; then
    if ! safe_fcitx5_remote -n >/dev/null; then
        echo "fcitx5-remote cannot reach the current user session; refusing to stop the existing input method" >&2
        echo "Run this helper from the real desktop session or use the user fcitx5 service." >&2
        exit 1
    fi
    if ! wayland_session_reachable; then
        echo "the Wayland compositor is not reachable; refusing to stop the existing input method" >&2
        echo "Run this helper from the real desktop session." >&2
        exit 1
    fi
fi

rm -f "$LIVE_SUPERVISION_FILE"

if [[ "$use_systemd_launch" == "1" ]]; then
    systemctl --user stop fcitx5.service >/dev/null 2>&1 || true
fi
if pgrep -x fcitx5 >/dev/null 2>&1; then
    pkill -TERM -x fcitx5 >/dev/null 2>&1 || true
    for _ in {1..30}; do
        pgrep -x fcitx5 >/dev/null 2>&1 || break
        sleep 0.1
    done
fi
if pgrep -x fcitx5 >/dev/null 2>&1; then
    echo "existing fcitx5 process did not stop cleanly; refusing to start a competing instance" >&2
    exit 1
fi

if [[ "$use_systemd_launch" == "1" ]]; then
    systemctl --user unset-environment TIPE_MODEL_COMMAND TIPE_MODEL_CONFIG TIPE_MODEL_TIMEOUT_SECONDS \
        TIPE_CONTINUOUS_MODE \
        TIPE_DEBUG TIPE_CANDIDATE_DEBUG TIPE_WAYLAND_POPUP_EDGE_FALLBACK TIPE_STATUS_EDGE_FALLBACK >/dev/null
    if [[ "${#env_args[@]}" -gt 0 ]]; then
        systemctl --user set-environment "${env_args[@]}"
    fi
    systemctl --user start fcitx5.service
    for _ in {1..30}; do
        initial_active_state=$(service_property ActiveState)
        initial_main_pid=$(service_property MainPID)
        if [[ "$initial_active_state" == "active" && "$initial_main_pid" =~ ^[1-9][0-9]*$ ]]; then
            expected_runtime_pid="$initial_main_pid"
            break
        fi
        sleep 0.1
    done
    if [[ -z "$expected_runtime_pid" ]]; then
        report_service_failure "startup did not produce an active MainPID"
        exit 1
    fi
    echo "fcitx5 launch mode: user service"
    echo "fcitx5 service MainPID: $expected_runtime_pid"
else
    (
        umask 077
        : >"$LOG_FILE"
        chmod 0600 "$LOG_FILE" || exit 1
        exec nohup env "${env_args[@]}" fcitx5 "${fcitx_args[@]}" >>"$LOG_FILE" 2>&1
    ) &
    expected_runtime_pid=$!
    echo "fcitx5 launch mode: direct fallback"
    echo "fcitx5 direct PID: $expected_runtime_pid"
fi
sleep 2
require_runtime_identity "initial settle"
echo "fcitx5 process is running with verified identity"
sleep 1
require_runtime_identity "before input-method activation"
remote_available=1
current_method=""
activate_tipe_once() {
    require_runtime_identity "before fcitx5-remote enable"
    safe_fcitx5_remote -o >/dev/null || return $?
    require_runtime_identity "after fcitx5-remote enable"
    sleep 0.2
    safe_fcitx5_remote -s tipe >/dev/null || return $?
    require_runtime_identity "after fcitx5-remote select"
    sleep 0.3
    current_method=$(safe_fcitx5_remote -n || true)
    require_runtime_identity "after fcitx5-remote query"
    [[ "$current_method" == "tipe" ]]
}
if [[ "$remote_available" == "1" ]]; then
    for attempt in 1 2 3; do
        activate_tipe_once && break
        status=$?
        if (( status == 2 )); then
            remote_available=0
            break
        fi
        sleep 0.5
    done
fi
if [[ "$remote_available" == "1" && "${current_method:-}" == "tipe" ]]; then
    sleep 1.2
    require_runtime_identity "settled activation"
    settled_method=$(safe_fcitx5_remote -n || true)
    require_runtime_identity "after settled input-method query"
    if [[ "$settled_method" != "tipe" ]]; then
        current_method="$settled_method"
        for attempt in 1 2; do
            activate_tipe_once && break
            status=$?
            if (( status == 2 )); then
                remote_available=0
                break
            fi
            sleep 0.5
        done
    else
        current_method="$settled_method"
    fi
fi
if [[ "$remote_available" == "1" ]]; then
    require_runtime_identity "before final input-method select"
    safe_fcitx5_remote -s tipe >/dev/null || remote_available=0
    require_runtime_identity "after final input-method select"
    sleep 0.3
    if [[ "$remote_available" == "1" ]]; then
        current_method=$(safe_fcitx5_remote -n || true)
        require_runtime_identity "after final input-method query"
    fi
fi
if [[ "$remote_available" == "1" && "${current_method:-}" == "tipe" ]]; then
    sleep 1.2
    require_runtime_identity "post-command activation settle"
    current_method=$(safe_fcitx5_remote -n || true)
    require_runtime_identity "after post-command input-method query"
fi
if [[ "$remote_available" == "1" ]]; then
    printf 'current input method: %s\n' "${current_method:-unknown}"
    if [[ "${current_method:-}" != "tipe" ]]; then
        echo "warning: TiPE was not activated after restart; current input method is ${current_method:-unknown}" >&2
        echo "check whether TiPE is available with: fcitx5-remote -m tipe" >&2
        echo "if TiPE is available but cannot be selected, the active fcitx5 profile may not include TiPE" >&2
    fi
else
    echo "current input method: unavailable because fcitx5-remote cannot access the session D-Bus" >&2
    exit 1
fi
require_runtime_identity "final verification"
if [[ "$model_option_count" -gt 0 ]]; then
    require_runtime_identity "model environment verification"
    runtime_pid="$expected_runtime_pid"
    expected_model_command=""
    for env_arg in "${env_args[@]}"; do
        if [[ "$env_arg" == TIPE_MODEL_COMMAND=* ]]; then
            expected_model_command="${env_arg#TIPE_MODEL_COMMAND=}"
            break
        fi
    done
    if [[ -n "$runtime_pid" && -n "$expected_model_command" && -r "/proc/$runtime_pid/environ" ]]; then
        runtime_model_command=$(tr '\0' '\n' <"/proc/$runtime_pid/environ" |
            sed -n 's/^TIPE_MODEL_COMMAND=//p' | sed -n '1p')
        if [[ "$runtime_model_command" != "$expected_model_command" ]]; then
            echo "fcitx5 restarted but did not inherit TIPE_MODEL_COMMAND" >&2
            echo "expected: $expected_model_command" >&2
            echo "actual: ${runtime_model_command:-unset}" >&2
            exit 1
        fi
        echo "model runtime command: $runtime_model_command"
    fi
fi
if [[ "$use_systemd_launch" == "1" ]]; then
    echo "fcitx5 log: user journal (journalctl --user -u fcitx5.service)"
else
    echo "fcitx5 log: $LOG_FILE"
fi
echo "verify model runtime with: tipe-doctor | grep -E 'runtime[[:space:]]+model-(command|config-active)'"
