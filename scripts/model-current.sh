#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${HOME:-}" ]]; then
    echo "HOME is not set; TiPE model configuration is unavailable" >&2
    exit 1
fi

config_file="${TIPE_MODEL_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/tipe/model-env}"
process_model_command="${TIPE_MODEL_COMMAND:-}"
if [[ -r "$config_file" ]]; then
    # shellcheck disable=SC1090
    source "$config_file"
fi

mode="${TIPE_MODEL_MODE:-heuristic}"
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

usage() {
    cat <<EOF
Usage:
  tipe-model-current
  tipe-model-current --show
  tipe-model-current --print-env

Without options, reads a TiPE model request TSV on stdin and runs the currently
configured model helper. Use tipe-model-config --write MODE ... to change the
configuration, then restart fcitx5 with tipe-restart-fcitx5.
EOF
}

helper_path() {
    local helper_name="$1"
    if [[ -x "$script_dir/$helper_name" ]]; then
        printf '%s\n' "$script_dir/$helper_name"
    elif [[ -x "$script_dir/${helper_name#tipe-}.sh" ]]; then
        printf '%s\n' "$script_dir/${helper_name#tipe-}.sh"
    elif [[ -x "$script_dir/${helper_name#tipe-}.py" ]]; then
        printf '%s\n' "$script_dir/${helper_name#tipe-}.py"
    elif [[ -x "$script_dir/../build/$helper_name" ]]; then
        printf '%s\n' "$script_dir/../build/$helper_name"
    elif [[ -x "$HOME/.local/bin/$helper_name" ]]; then
        printf '%s\n' "$HOME/.local/bin/$helper_name"
    else
        return 1
    fi
}

show_personal_model() {
    local path="$1"
    local helper inspection key value
    printf 'personal-model\t%s\n' "$path"
    if [[ ! -r "$path" ]]; then
        printf 'personal-model-status\tuntrained\n'
        return
    fi
    helper=$(helper_path tipe-personal-model) || {
        printf 'personal-model-status\tunavailable\n'
        return
    }
    if ! inspection=$("$helper" inspect --model "$path" 2>/dev/null); then
        printf 'personal-model-status\tinvalid\n'
        return
    fi
    printf 'personal-model-status\tready\n'
    while IFS=$'\t' read -r key value _; do
        case "$key" in
            name|architecture|feature-version|pinyin-prior-entries|training-pinyin-prior-sources|training-samples|training-ranking-samples|training-chinese-ranking-samples|training-correction-only-samples|training-non-leading-samples|pair-evidence|active-pair-evidence|raw-token-evidence|active-raw-token-evidence|training-raw-token-evidence-entries|training-active-raw-token-evidence|active-correction-patterns|active-key-habits|keyboard-correction-safe|raw-profile-safe|generic-ranking-safe|component-update-safe|evidence-merge-strategy|training-validation-strategy|training-validation-accuracy|training-validation-baseline-accuracy|training-validation-gain|training-validation-non-leading-samples|training-validation-non-leading-correct|training-validation-non-leading-accuracy|training-validation-leading-samples|training-validation-leading-correct|training-validation-generic-non-leading-samples|training-validation-generic-non-leading-correct|training-validation-generic-non-leading-accuracy|training-validation-generic-excluded-direct-evidence|training-validation-generic-excluded-seen-preedit|training-validation-generic-excluded-raw-candidate|training-validation-generic-excluded-derived-prefix|training-raw-profile-samples|training-raw-profile-accepted-samples|training-raw-profile-rejected-samples|training-raw-profile-auxiliary-positive-samples|training-raw-profile-validation-samples|training-raw-profile-validation-correct|training-raw-profile-validation-false-promotions|training-raw-profile-validation-accuracy|training-raw-profile-recommendation|training-recommendation|promotion-margin)
                printf 'personal-model-%s\t%s\n' "$key" "$value"
                ;;
        esac
    done <<< "$inspection"
}

model_kind_for() {
    local selected_mode="$1"
    local selected_backend="$2"
    case "$selected_mode" in
        off|disabled|none)
            echo "disabled"
            ;;
        dump)
            echo "request-dump"
            ;;
        custom)
            echo "custom-wrapper"
            ;;
        personal)
            echo "personal-reranker"
            ;;
        llama-cpp)
            echo "local-llama-cpp"
            ;;
        ollama)
            echo "local-http:$selected_backend"
            ;;
        openai)
            echo "official-openai:$selected_backend"
            ;;
        openai-compatible)
            echo "openai-compatible:$selected_backend"
            ;;
        heuristic|"")
            echo "offline-heuristic"
            ;;
        *)
            echo "unknown"
            ;;
    esac
}

config_export_value() {
    local name="$1"
    local file="$2"
    [[ -r "$file" ]] || return 1
    sed -n "s/^export $name=['\"]\\{0,1\\}\\([^'\"]*\\)['\"]\\{0,1\\}$/\\1/p" "$file" | sed -n '1p'
}

config_api_key_env() {
    local file="$1"
    [[ -r "$file" ]] || return 1
    sed -n 's/^export TIPE_MODEL_API_KEY="${\([A-Za-z_][A-Za-z0-9_]*\):-}"$/\1/p' "$file" | sed -n '1p'
}

config_api_key_file() {
    local file="$1"
    config_export_value TIPE_MODEL_API_KEY_FILE "$file"
}

api_key_file_runtime_status() {
    local file="$1"
    if [[ ! -e "$file" ]]; then
        echo "unset"
        return
    fi
    if [[ ! -f "$file" || -L "$file" || ! -r "$file" ]]; then
        echo "invalid"
        return
    fi
    local size owner file_mode
    size=$(stat -c '%s' "$file" 2>/dev/null) || {
        echo "invalid"
        return
    }
    owner=$(stat -c '%u' "$file" 2>/dev/null) || {
        echo "invalid"
        return
    }
    file_mode=$(stat -c '%a' "$file" 2>/dev/null) || {
        echo "invalid"
        return
    }
    if (( size <= 0 || size > 16385 )) || [[ "$owner" != "$(id -u)" ]] ||
        (( (8#$file_mode & 077) != 0 )); then
        echo "invalid"
        return
    fi
    echo "set"
}

is_model_current_command() {
    local command_line="$1"
    command_line="${command_line//\\ / }"
    [[ -n "$command_line" && "$command_line" =~ ^[A-Za-z0-9_./:=+\ -]+$ ]] || return 1

    local tokens=()
    read -r -a tokens <<< "$command_line"
    while [[ "${#tokens[@]}" -gt 0 && "${tokens[0]}" =~ ^[A-Za-z_][A-Za-z0-9_]*=.+$ ]]; do
        tokens=("${tokens[@]:1}")
    done
    [[ "${#tokens[@]}" -gt 0 ]] || return 1
    case "${tokens[0]}" in
        "$HOME/.local/bin/tipe-model-current"|*/tipe-model-current|*/model-current.sh)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

run_direct_command() {
    local command_line="$1"
    if [[ -z "$command_line" || ! "$command_line" =~ ^[A-Za-z0-9_./:=+\ -]+$ ]]; then
        echo "unsafe TIPE model command" >&2
        exit 2
    fi

    local tokens=()
    read -r -a tokens <<< "$command_line"
    if [[ "${#tokens[@]}" -eq 0 ]]; then
        echo "empty TIPE model command" >&2
        exit 2
    fi

    local env_args=()
    while [[ "${#tokens[@]}" -gt 0 && "${tokens[0]}" =~ ^[A-Za-z_][A-Za-z0-9_]*=.+$ ]]; do
        env_args+=("${tokens[0]}")
        tokens=("${tokens[@]:1}")
    done
    if [[ "${#tokens[@]}" -eq 0 ]]; then
        echo "TIPE model command is missing an executable" >&2
        exit 2
    fi
    if [[ ! -x "${tokens[0]}" ]]; then
        echo "TIPE model command is not executable: ${tokens[0]}" >&2
        exit 1
    fi
    exec env "${env_args[@]}" "${tokens[@]}"
}

print_show() {
    local backend="${TIPE_MODEL_BACKEND:-heuristic}"
    local configured_command=""
    local configured_api_key_env=""
    local configured_api_key_file=""
    echo "config	$config_file"
    if [[ -r "$config_file" ]]; then
        sed 's/^/config-line\t/' "$config_file"
        configured_command=$(config_export_value TIPE_MODEL_COMMAND "$config_file" || true)
        configured_api_key_env=$(config_api_key_env "$config_file" || true)
        configured_api_key_file=$(config_api_key_file "$config_file" || true)
    else
        echo "status	missing"
    fi
    echo "mode	$mode"
    echo "backend	$backend"
    echo "kind	$(model_kind_for "$mode" "$backend")"
    echo "configured-command	${configured_command:-unset}"
    if is_model_current_command "${configured_command:-}"; then
        echo "configured-command-valid	1"
    else
        echo "configured-command-valid	0"
    fi
    echo "process-command	${process_model_command:-unset}"
    echo "process-command-scope	current-shell-environment"
    echo "process-command-active-scope	current-shell-only-not-fcitx5-runtime"
    echo "runtime-verification	tipe-doctor"
    if is_model_current_command "${process_model_command:-}"; then
        echo "process-command-active	1"
    else
        echo "process-command-active	0"
        echo "activation-hint	tipe-restart-fcitx5"
    fi
    echo "click-trigger	F9"
    echo "continuous-mode	local-light-rerank"
    echo "continuous-toggle	Shift+F9"
    case "$mode" in
        personal)
            personal_path="${TIPE_PERSONAL_MODEL_PATH:-${XDG_DATA_HOME:-$HOME/.local/share}/tipe/personal-reranker.json}"
            show_personal_model "$personal_path"
            ;;
        custom)
            echo "custom-command	${TIPE_MODEL_CUSTOM_COMMAND:-}"
            ;;
        dump)
            echo "dump-path	${TIPE_MODEL_DUMP_PATH:-${XDG_CACHE_HOME:-$HOME/.cache}/tipe/model-request.tsv}"
            ;;
        llama-cpp)
            echo "invocation	on-demand-single-process"
            echo "model	${TIPE_MODEL_NAME:-}"
            echo "llama-command	${TIPE_LLAMA_CPP_COMMAND:-/usr/bin/llama-cli}"
            if [[ -x "${TIPE_LLAMA_CPP_COMMAND:-/usr/bin/llama-cli}" ]]; then
                echo "llama-command-valid	1"
            else
                echo "llama-command-valid	0"
            fi
            if [[ -r "${TIPE_MODEL_NAME:-}" ]]; then
                echo "llama-model-readable	1"
            else
                echo "llama-model-readable	0"
            fi
            echo "llama-threads	${TIPE_LLAMA_CPP_THREADS:-6}"
            echo "llama-context	${TIPE_LLAMA_CPP_CONTEXT:-8192}"
            ;;
        ollama|openai|openai-compatible)
            echo "base-url	${TIPE_MODEL_BASE_URL:-}"
            echo "model	${TIPE_MODEL_NAME:-}"
            echo "chat-path	${TIPE_MODEL_CHAT_PATH:-/chat/completions}"
            if [[ -n "$configured_api_key_file" ]]; then
                echo "api-key-file	$configured_api_key_file"
                echo "api-key-source	stored-file"
                echo "api-key-runtime	$(api_key_file_runtime_status "$configured_api_key_file")"
            elif [[ -n "$configured_api_key_env" ]]; then
                echo "api-key-env	$configured_api_key_env"
                echo "api-key-source	environment"
                if [[ -n "${!configured_api_key_env:-}" ]]; then
                    echo "api-key-runtime	set"
                else
                    echo "api-key-runtime	unset"
                fi
            elif [[ -n "${TIPE_MODEL_API_KEY:-}" ]]; then
                echo "api-key-runtime	set"
            else
                echo "api-key-runtime	unset"
            fi
            ;;
    esac
    echo "timeout	${TIPE_MODEL_TIMEOUT_SECONDS:-2}"
    echo "http-timeout	${TIPE_MODEL_HTTP_TIMEOUT_SECONDS:-8}"
    echo "temperature	${TIPE_MODEL_TEMPERATURE:-0}"
    echo "max-tokens	${TIPE_MODEL_MAX_TOKENS:-128}"
    echo "continuous	${TIPE_CONTINUOUS_MODE:-0}"
    echo "training-context	${TIPE_PERSONAL_TRAIN_CONTEXT:-0}"
    echo "training-surrounding	${TIPE_PERSONAL_TRAIN_SURROUNDING:-0}"
    echo "send-recent-input	${TIPE_MODEL_SEND_RECENT_INPUT:-0}"
    echo "send-surrounding	${TIPE_MODEL_SEND_SURROUNDING:-0}"
    case "$mode" in
        llama-cpp|ollama|openai|openai-compatible)
            echo "dry-run-test-supported	1"
            ;;
        *)
            echo "dry-run-test-supported	0"
            ;;
    esac
}

print_env() {
    echo "export TIPE_MODEL_COMMAND=$HOME/.local/bin/tipe-model-current"
    echo "export TIPE_MODEL_CONFIG=$config_file"
    echo "export TIPE_MODEL_MODE=$mode"
    echo "export TIPE_MODEL_BACKEND=${TIPE_MODEL_BACKEND:-heuristic}"
    [[ -z "${TIPE_MODEL_BASE_URL:-}" ]] || printf 'export TIPE_MODEL_BASE_URL=%q\n' "$TIPE_MODEL_BASE_URL"
    [[ -z "${TIPE_MODEL_NAME:-}" ]] || printf 'export TIPE_MODEL_NAME=%q\n' "$TIPE_MODEL_NAME"
    [[ -z "${TIPE_MODEL_CHAT_PATH:-}" ]] || printf 'export TIPE_MODEL_CHAT_PATH=%q\n' "$TIPE_MODEL_CHAT_PATH"
    [[ -z "${TIPE_MODEL_CUSTOM_COMMAND:-}" ]] || printf 'export TIPE_MODEL_CUSTOM_COMMAND=%q\n' "$TIPE_MODEL_CUSTOM_COMMAND"
    [[ -z "${TIPE_PERSONAL_MODEL_PATH:-}" ]] || printf 'export TIPE_PERSONAL_MODEL_PATH=%q\n' "$TIPE_PERSONAL_MODEL_PATH"
    [[ -z "${TIPE_LLAMA_CPP_COMMAND:-}" ]] || printf 'export TIPE_LLAMA_CPP_COMMAND=%q\n' "$TIPE_LLAMA_CPP_COMMAND"
    [[ -z "${TIPE_LLAMA_CPP_THREADS:-}" ]] || printf 'export TIPE_LLAMA_CPP_THREADS=%q\n' "$TIPE_LLAMA_CPP_THREADS"
    [[ -z "${TIPE_LLAMA_CPP_CONTEXT:-}" ]] || printf 'export TIPE_LLAMA_CPP_CONTEXT=%q\n' "$TIPE_LLAMA_CPP_CONTEXT"
    [[ -z "${TIPE_MODEL_DUMP_PATH:-}" ]] || printf 'export TIPE_MODEL_DUMP_PATH=%q\n' "$TIPE_MODEL_DUMP_PATH"
    [[ -z "${TIPE_MODEL_TIMEOUT_SECONDS:-}" ]] || printf 'export TIPE_MODEL_TIMEOUT_SECONDS=%q\n' "$TIPE_MODEL_TIMEOUT_SECONDS"
    [[ -z "${TIPE_MODEL_HTTP_TIMEOUT_SECONDS:-}" ]] || printf 'export TIPE_MODEL_HTTP_TIMEOUT_SECONDS=%q\n' "$TIPE_MODEL_HTTP_TIMEOUT_SECONDS"
    [[ -z "${TIPE_MODEL_TEMPERATURE:-}" ]] || printf 'export TIPE_MODEL_TEMPERATURE=%q\n' "$TIPE_MODEL_TEMPERATURE"
    [[ -z "${TIPE_MODEL_MAX_TOKENS:-}" ]] || printf 'export TIPE_MODEL_MAX_TOKENS=%q\n' "$TIPE_MODEL_MAX_TOKENS"
    [[ -z "${TIPE_MODEL_API_KEY_FILE:-}" ]] || printf 'export TIPE_MODEL_API_KEY_FILE=%q\n' "$TIPE_MODEL_API_KEY_FILE"
    [[ -z "${TIPE_CONTINUOUS_MODE:-}" ]] || printf 'export TIPE_CONTINUOUS_MODE=%q\n' "$TIPE_CONTINUOUS_MODE"
    [[ -z "${TIPE_PERSONAL_TRAIN_CONTEXT:-}" ]] ||
        printf 'export TIPE_PERSONAL_TRAIN_CONTEXT=%q\n' "$TIPE_PERSONAL_TRAIN_CONTEXT"
    [[ -z "${TIPE_PERSONAL_TRAIN_SURROUNDING:-}" ]] ||
        printf 'export TIPE_PERSONAL_TRAIN_SURROUNDING=%q\n' "$TIPE_PERSONAL_TRAIN_SURROUNDING"
    [[ -z "${TIPE_MODEL_SEND_RECENT_INPUT:-}" ]] ||
        printf 'export TIPE_MODEL_SEND_RECENT_INPUT=%q\n' "$TIPE_MODEL_SEND_RECENT_INPUT"
    [[ -z "${TIPE_MODEL_SEND_SURROUNDING:-}" ]] ||
        printf 'export TIPE_MODEL_SEND_SURROUNDING=%q\n' "$TIPE_MODEL_SEND_SURROUNDING"
    if [[ -r "$config_file" ]]; then
        api_key_line=$(sed -n '/^export TIPE_MODEL_API_KEY=/p' "$config_file" | sed -n '1p')
        if [[ "$api_key_line" == *'${'*':-}'* ]]; then
            printf '%s\n' "$api_key_line"
        elif [[ -n "${TIPE_MODEL_API_KEY:-}" ]]; then
            echo "export TIPE_MODEL_API_KEY=<set-redacted>"
        fi
    elif [[ -n "${TIPE_MODEL_API_KEY:-}" ]]; then
        echo "export TIPE_MODEL_API_KEY=<set-redacted>"
    fi
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
    --show|--status)
        print_show
        exit 0
        ;;
    --print-env)
        print_env
        exit 0
        ;;
    "")
        ;;
    *)
        echo "unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
esac

case "$mode" in
    off|disabled|none)
        exit 0
        ;;
    dump)
        helper=$(helper_path tipe-model-dump) || {
            echo "tipe-model-dump helper is not installed" >&2
            exit 1
        }
        exec "$helper"
        ;;
    custom)
        if [[ -z "${TIPE_MODEL_CUSTOM_COMMAND:-}" ]]; then
            echo "TIPE_MODEL_CUSTOM_COMMAND is not set for custom model mode" >&2
            exit 2
        fi
        run_direct_command "$TIPE_MODEL_CUSTOM_COMMAND"
        ;;
    personal)
        helper=$(helper_path tipe-personal-model) || {
            echo "tipe-personal-model helper is not installed" >&2
            exit 1
        }
        if [[ -n "${TIPE_PERSONAL_MODEL_PATH:-}" ]]; then
            exec "$helper" predict --model "$TIPE_PERSONAL_MODEL_PATH"
        fi
        exec "$helper" predict
        ;;
    heuristic|llama-cpp|ollama|openai|openai-compatible)
        helper=$(helper_path tipe-model-adapter) || {
            echo "tipe-model-adapter helper is not installed" >&2
            exit 1
        }
        case "$mode" in
            openai)
                export TIPE_MODEL_BACKEND=openai-compatible
                ;;
            *)
                export TIPE_MODEL_BACKEND="$mode"
                ;;
        esac
        exec "$helper"
        ;;
    *)
        echo "unknown TIPE_MODEL_MODE: $mode" >&2
        exit 2
        ;;
esac
