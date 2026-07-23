#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${HOME:-}" ]]; then
    echo "HOME is not set; TiPE model configuration paths are unavailable" >&2
    exit 1
fi

config_file="${TIPE_MODEL_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/tipe/model-env}"
api_key_file_default="$(dirname -- "$config_file")/model-api-key"
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
current_command="$HOME/.local/bin/tipe-model-current"
analyze_window_command="$HOME/.local/bin/tipe-analyze-window"
supervision_window_command="$HOME/.local/bin/tipe-supervision-window"
self_test_command="$HOME/.local/bin/tipe-model-self-test"
personal_model_default="${XDG_DATA_HOME:-$HOME/.local/share}/tipe/personal-reranker.json"
llama_model_default="${XDG_DATA_HOME:-$HOME/.local/share}/tipe/models/qwen2.5-1.5b-instruct-q4_k_m.gguf"
mode=""
dry_run=0
test_config=0
test_dry_run=0
base_url=""
model_name=""
chat_path=""
dump_path=""
custom_command=""
personal_model_path=""
api_key_env=""
api_key_stdin=0
clear_api_key=0
api_key_value=""
api_key_reference_path=""
timeout_seconds=""
http_timeout_seconds=""
temperature=""
max_tokens=""
continuous_mode=""
training_context=""
training_surrounding=""
send_recent_input=""
send_surrounding=""
llama_command=""
llama_threads=""
llama_context=""

usage() {
    cat <<EOF
Usage:
  tipe-model-config --show
  tipe-model-config --write MODE [options] [--dry-run]

Modes:
  off                 disable external model output
  heuristic           offline adapter checks and conservative corrections
  personal            trained local TiPE candidate reranker
  llama-cpp           on-demand local GGUF model through llama-cli
  dump                capture TiPE's model request TSV for debugging
  custom              run a user-provided local/cloud wrapper script
  ollama              local Ollama/OpenAI-compatible endpoint
  openai              official OpenAI API endpoint
  openai-compatible   cloud or custom OpenAI-compatible endpoint

Options:
  --base-url URL          endpoint base URL
  --model NAME           model name
  --chat-path PATH       chat completion path, default /chat/completions
  --dump-path PATH       request dump path for dump mode
  --command PATH         wrapper command path for custom mode
  --personal-model PATH  personal reranker model path
  --llama-command PATH   llama-cli executable, default /usr/bin/llama-cli
  --llama-threads N      CPU threads for one analysis, default 6
  --llama-context N      llama.cpp context size, default 8192
  --api-key-env NAME     read API key from this environment variable at runtime
  --api-key-stdin        read one API key from stdin and store it in a user-only file
  --clear-api-key        remove the stored API key while saving this configuration
  --timeout SECONDS      TiPE command timeout, 1..30
  --http-timeout SECONDS adapter HTTP timeout, 1..60
  --temperature VALUE    adapter temperature, 0..2
  --max-tokens N         adapter max tokens, 1..4096
  --continuous on|off    default Shift+F9 continuous light rerank mode
  --training-context on|off
                         use recent-commit fingerprints for personal training
  --training-surrounding on|off
                         use cursor-surrounding fingerprints for personal training
  --send-recent-input on|off
                         let cloud models receive recent keys and edit history
  --send-surrounding on|off
                         let cloud models receive cursor text and application name
  --dry-run              print the config without writing it
  --test                 run tipe-model-self-test against the selected config
  --test-dry-run         validate adapter request JSON without model HTTP calls
EOF
}

require_value() {
    local option="$1"
    if [[ $# -lt 2 || -z "${2:-}" ]]; then
        echo "$option requires a value" >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --show)
            mode="show"
            ;;
        --write)
            if [[ $# -lt 2 ]]; then
                echo "--write requires a mode" >&2
                exit 2
            fi
            mode="$2"
            shift
            ;;
        --base-url)
            require_value "$1" "${2:-}"
            base_url="$2"
            shift
            ;;
        --model)
            require_value "$1" "${2:-}"
            model_name="$2"
            shift
            ;;
        --chat-path)
            require_value "$1" "${2:-}"
            chat_path="$2"
            shift
            ;;
        --dump-path)
            require_value "$1" "${2:-}"
            dump_path="$2"
            shift
            ;;
        --command)
            require_value "$1" "${2:-}"
            custom_command="$2"
            shift
            ;;
        --personal-model)
            require_value "$1" "${2:-}"
            personal_model_path="$2"
            shift
            ;;
        --llama-command)
            require_value "$1" "${2:-}"
            llama_command="$2"
            shift
            ;;
        --llama-threads)
            require_value "$1" "${2:-}"
            llama_threads="$2"
            shift
            ;;
        --llama-context)
            require_value "$1" "${2:-}"
            llama_context="$2"
            shift
            ;;
        --api-key-env)
            require_value "$1" "${2:-}"
            api_key_env="$2"
            shift
            ;;
        --api-key-stdin)
            api_key_stdin=1
            ;;
        --clear-api-key)
            clear_api_key=1
            ;;
        --timeout)
            require_value "$1" "${2:-}"
            timeout_seconds="$2"
            shift
            ;;
        --http-timeout)
            require_value "$1" "${2:-}"
            http_timeout_seconds="$2"
            shift
            ;;
        --temperature)
            require_value "$1" "${2:-}"
            temperature="$2"
            shift
            ;;
        --max-tokens)
            require_value "$1" "${2:-}"
            max_tokens="$2"
            shift
            ;;
        --continuous)
            require_value "$1" "${2:-}"
            continuous_mode="$2"
            shift
            ;;
        --training-context)
            require_value "$1" "${2:-}"
            training_context="$2"
            shift
            ;;
        --training-surrounding)
            require_value "$1" "${2:-}"
            training_surrounding="$2"
            shift
            ;;
        --send-recent-input)
            require_value "$1" "${2:-}"
            send_recent_input="$2"
            shift
            ;;
        --send-surrounding)
            require_value "$1" "${2:-}"
            send_surrounding="$2"
            shift
            ;;
        --dry-run)
            dry_run=1
            ;;
        --test)
            test_config=1
            ;;
        --test-dry-run)
            test_config=1
            test_dry_run=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

if [[ -z "$mode" ]]; then
    mode="show"
fi

shell_quote() {
    printf '%q' "$1"
}

validate_int_range() {
    local name="$1"
    local value="$2"
    local min="$3"
    local max="$4"
    [[ "$value" =~ ^[0-9]+$ ]] || {
        echo "$name must be an integer" >&2
        exit 2
    }
    if (( value < min || value > max )); then
        echo "$name must be between $min and $max" >&2
        exit 2
    fi
}

validate_float_range() {
    local name="$1"
    local value="$2"
    local min="$3"
    local max="$4"
    [[ "$value" =~ ^([0-9]+)(\.[0-9]+)?$ ]] || {
        echo "$name must be a number" >&2
        exit 2
    }
    awk -v value="$value" -v min="$min" -v max="$max" 'BEGIN { exit !(value >= min && value <= max) }' || {
        echo "$name must be between $min and $max" >&2
        exit 2
    }
}

validate_key_env() {
    [[ -z "$api_key_env" || "$api_key_env" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || {
        echo "--api-key-env must be a shell environment variable name" >&2
        exit 2
    }
}

validate_custom_command() {
    if [[ "$mode" != "custom" ]]; then
        return
    fi
    if [[ -z "$custom_command" ]]; then
        echo "custom mode requires --command COMMAND" >&2
        exit 2
    fi
    if [[ ! "$custom_command" =~ ^[A-Za-z0-9_./:=+\ -]+$ ]]; then
        echo "--command may only contain safe path, word, assignment, and space characters" >&2
        exit 2
    fi
    read -r -a custom_tokens <<< "$custom_command"
    if [[ "${#custom_tokens[@]}" -eq 0 ]]; then
        echo "custom mode requires --command COMMAND" >&2
        exit 2
    fi
    local token
    for token in "${custom_tokens[@]}"; do
        if [[ "$token" == *=* && "$token" =~ ^[A-Za-z_][A-Za-z0-9_]*=.+$ ]]; then
            continue
        fi
        [[ "$token" != *=* || "$token" == */* || "$token" == *.* ]] || {
            echo "--command contains an invalid environment assignment: $token" >&2
            exit 2
        }
    done
}

validate_personal_model_path() {
    if [[ "$mode" != "personal" || -z "$personal_model_path" ]]; then
        return
    fi
    [[ "$personal_model_path" != *[$'\t\r\n']* ]] || {
        echo "--personal-model must not contain tabs or newlines" >&2
        exit 2
    }
}

validate_endpoint_settings() {
    case "$mode" in
        llama-cpp|ollama|openai|openai-compatible)
            ;;
        *)
            return
            ;;
    esac
    if [[ -n "$base_url" ]]; then
        [[ "$base_url" =~ ^https?://[^[:space:][:cntrl:]]+$ ]] || {
            echo "--base-url must be an http(s) URL without whitespace" >&2
            exit 2
        }
    fi
    if [[ -n "$chat_path" ]]; then
        [[ "$chat_path" == /* && "$chat_path" != *[$'\t\r\n ']* ]] || {
            echo "--chat-path must start with / and contain no whitespace" >&2
            exit 2
        }
    fi
    if [[ -n "$model_name" ]]; then
        [[ "$model_name" != *[$'\t\r\n']* ]] || {
            echo "--model must not contain tabs or newlines" >&2
            exit 2
        }
    fi
}

validate_llama_settings() {
    [[ "$mode" == "llama-cpp" ]] || return 0
    local path="${model_name:-$llama_model_default}"
    [[ "$path" == /* && "$path" != *[$'\t\r\n ']* ]] || {
        echo "llama-cpp --model must be an absolute GGUF path without whitespace" >&2
        exit 2
    }
    local command_path="${llama_command:-/usr/bin/llama-cli}"
    [[ "$command_path" == /* && "$command_path" != *[$'\t\r\n ']* ]] || {
        echo "--llama-command must be an absolute path without whitespace" >&2
        exit 2
    }
}

personal_model_helper() {
    if [[ -x "$script_dir/tipe-personal-model" ]]; then
        printf '%s\n' "$script_dir/tipe-personal-model"
    elif [[ -x "$script_dir/personal-model.py" ]]; then
        printf '%s\n' "$script_dir/personal-model.py"
    elif [[ -x "$HOME/.local/bin/tipe-personal-model" ]]; then
        printf '%s\n' "$HOME/.local/bin/tipe-personal-model"
    else
        return 1
    fi
}

print_personal_model_status() {
    local path="$1"
    local helper inspection key value
    printf 'model-status\tpersonal-model\t%s\n' "$path"
    if [[ ! -r "$path" ]]; then
        printf 'model-status\tpersonal-model-status\tuntrained\n'
        return
    fi
    helper=$(personal_model_helper) || {
        printf 'model-status\tpersonal-model-status\tunavailable\n'
        return
    }
    if ! inspection=$("$helper" inspect --model "$path" 2>/dev/null); then
        printf 'model-status\tpersonal-model-status\tinvalid\n'
        return
    fi
    printf 'model-status\tpersonal-model-status\tready\n'
    while IFS=$'\t' read -r key value _; do
        case "$key" in
            name|architecture|feature-version|pinyin-prior-entries|training-pinyin-prior-sources|training-samples|training-ranking-samples|training-chinese-ranking-samples|training-correction-only-samples|training-non-leading-samples|pair-evidence|active-pair-evidence|raw-token-evidence|active-raw-token-evidence|training-raw-token-evidence-entries|training-active-raw-token-evidence|active-correction-patterns|active-key-habits|keyboard-correction-safe|raw-profile-safe|generic-ranking-safe|component-update-safe|evidence-merge-strategy|training-validation-strategy|training-validation-accuracy|training-validation-baseline-accuracy|training-validation-gain|training-validation-non-leading-samples|training-validation-non-leading-correct|training-validation-non-leading-accuracy|training-validation-leading-samples|training-validation-leading-correct|training-validation-generic-non-leading-samples|training-validation-generic-non-leading-correct|training-validation-generic-non-leading-accuracy|training-validation-generic-excluded-direct-evidence|training-validation-generic-excluded-seen-preedit|training-validation-generic-excluded-raw-candidate|training-validation-generic-excluded-derived-prefix|training-raw-profile-samples|training-raw-profile-accepted-samples|training-raw-profile-rejected-samples|training-raw-profile-auxiliary-positive-samples|training-raw-profile-validation-samples|training-raw-profile-validation-correct|training-raw-profile-validation-false-promotions|training-raw-profile-validation-accuracy|training-raw-profile-recommendation|training-recommendation|promotion-margin)
                printf 'model-status\tpersonal-model-%s\t%s\n' "$key" "$value"
                ;;
        esac
    done <<< "$inspection"
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
        "$current_command"|*/tipe-model-current|*/model-current.sh)
            return 0
            ;;
        *)
            return 1
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

print_show() {
    local configured_mode="heuristic"
    local configured_backend="heuristic"
    local configured_custom_command=""
    local configured_personal_model_path=""
    local configured_base_url=""
    local configured_model_name=""
    local configured_chat_path=""
    local configured_api_key_env=""
    local configured_api_key_file=""
    local configured_dump_path=""
    local configured_continuous_mode="0"
    local configured_training_context="0"
    local configured_training_surrounding="0"
    local configured_send_recent_input="0"
    local configured_send_surrounding="0"
    local configured_model_command=""
    local configured_timeout=""
    local configured_http_timeout=""
    local configured_temperature=""
    local configured_max_tokens=""
    local configured_llama_command=""
    local configured_llama_threads=""
    local configured_llama_context=""
    echo "config	$config_file"
    if [[ -r "$config_file" ]]; then
        sed 's/^/config-line\t/' "$config_file"
        configured_mode=$(config_export_value TIPE_MODEL_MODE "$config_file" || true)
        configured_backend=$(config_export_value TIPE_MODEL_BACKEND "$config_file" || true)
        configured_custom_command=$(config_export_value TIPE_MODEL_CUSTOM_COMMAND "$config_file" || true)
        configured_personal_model_path=$(config_export_value TIPE_PERSONAL_MODEL_PATH "$config_file" || true)
        configured_base_url=$(config_export_value TIPE_MODEL_BASE_URL "$config_file" || true)
        configured_model_name=$(config_export_value TIPE_MODEL_NAME "$config_file" || true)
        configured_chat_path=$(config_export_value TIPE_MODEL_CHAT_PATH "$config_file" || true)
        configured_api_key_env=$(config_api_key_env "$config_file" || true)
        configured_api_key_file=$(config_api_key_file "$config_file" || true)
        configured_dump_path=$(config_export_value TIPE_MODEL_DUMP_PATH "$config_file" || true)
        configured_continuous_mode=$(config_export_value TIPE_CONTINUOUS_MODE "$config_file" || true)
        configured_training_context=$(config_export_value TIPE_PERSONAL_TRAIN_CONTEXT "$config_file" || true)
        configured_training_surrounding=$(config_export_value TIPE_PERSONAL_TRAIN_SURROUNDING "$config_file" || true)
        configured_send_recent_input=$(config_export_value TIPE_MODEL_SEND_RECENT_INPUT "$config_file" || true)
        configured_send_surrounding=$(config_export_value TIPE_MODEL_SEND_SURROUNDING "$config_file" || true)
        configured_model_command=$(config_export_value TIPE_MODEL_COMMAND "$config_file" || true)
        configured_timeout=$(config_export_value TIPE_MODEL_TIMEOUT_SECONDS "$config_file" || true)
        configured_http_timeout=$(config_export_value TIPE_MODEL_HTTP_TIMEOUT_SECONDS "$config_file" || true)
        configured_temperature=$(config_export_value TIPE_MODEL_TEMPERATURE "$config_file" || true)
        configured_max_tokens=$(config_export_value TIPE_MODEL_MAX_TOKENS "$config_file" || true)
        configured_llama_command=$(config_export_value TIPE_LLAMA_CPP_COMMAND "$config_file" || true)
        configured_llama_threads=$(config_export_value TIPE_LLAMA_CPP_THREADS "$config_file" || true)
        configured_llama_context=$(config_export_value TIPE_LLAMA_CPP_CONTEXT "$config_file" || true)
        configured_mode="${configured_mode:-heuristic}"
        configured_backend="${configured_backend:-heuristic}"
        configured_continuous_mode="${configured_continuous_mode:-0}"
        configured_training_context="${configured_training_context:-0}"
        configured_training_surrounding="${configured_training_surrounding:-0}"
        configured_send_recent_input="${configured_send_recent_input:-0}"
        configured_send_surrounding="${configured_send_surrounding:-0}"
    else
        echo "status	missing"
    fi
    if [[ -x "$current_command" ]]; then
        echo "command	$current_command"
    else
        echo "command-missing	$current_command"
    fi
    echo "model-status	configured-mode	$configured_mode"
    echo "model-status	backend	$configured_backend"
    echo "model-status	kind	$(model_kind_for "$configured_mode" "$configured_backend")"
    echo "model-status	timeout	${configured_timeout:-2}"
    echo "model-status	http-timeout	${configured_http_timeout:-8}"
    echo "model-status	temperature	${configured_temperature:-0}"
    echo "model-status	max-tokens	${configured_max_tokens:-128}"
    if [[ "$configured_mode" == "personal" ]]; then
        configured_personal_model_path="${configured_personal_model_path:-$personal_model_default}"
        print_personal_model_status "$configured_personal_model_path"
    fi
    if [[ -n "$configured_custom_command" ]]; then
        echo "model-status	custom-command	$configured_custom_command"
    fi
    if [[ -n "$configured_base_url" ]]; then
        echo "model-status	base-url	$configured_base_url"
    fi
    if [[ -n "$configured_model_name" ]]; then
        echo "model-status	model	$configured_model_name"
    fi
    if [[ "$configured_mode" == "llama-cpp" ]]; then
        configured_llama_command="${configured_llama_command:-${TIPE_LLAMA_CPP_COMMAND:-/usr/bin/llama-cli}}"
        echo "model-status	invocation	on-demand-single-process"
        echo "model-status	llama-command	$configured_llama_command"
        [[ -x "$configured_llama_command" ]] && echo "model-status	llama-command-valid	1" ||
            echo "model-status	llama-command-valid	0"
        [[ -r "$configured_model_name" ]] && echo "model-status	llama-model-readable	1" ||
            echo "model-status	llama-model-readable	0"
        echo "model-status	llama-threads	${configured_llama_threads:-6}"
        echo "model-status	llama-context	${configured_llama_context:-8192}"
    fi
    if [[ -n "$configured_chat_path" ]]; then
        echo "model-status	chat-path	$configured_chat_path"
    fi
    if [[ -n "$configured_api_key_file" ]]; then
        echo "model-status	api-key-file	$configured_api_key_file"
        echo "model-status	api-key-source	stored-file"
        echo "model-status	api-key-runtime	$(api_key_file_runtime_status "$configured_api_key_file")"
    elif [[ -n "$configured_api_key_env" ]]; then
        echo "model-status	api-key-env	$configured_api_key_env"
        echo "model-status	api-key-source	environment"
        if [[ -n "${!configured_api_key_env:-}" ]]; then
            echo "model-status	api-key-runtime	set"
        else
            echo "model-status	api-key-runtime	unset"
        fi
    fi
    if [[ -n "$configured_dump_path" ]]; then
        echo "model-status	dump-path	$configured_dump_path"
    fi
    echo "model-status	configured-command	${configured_model_command:-unset}"
    if is_model_current_command "${configured_model_command:-}"; then
        echo "model-status	configured-command-valid	1"
    else
        echo "model-status	configured-command-valid	0"
    fi
    echo "model-status	click-trigger	F9"
    echo "model-status	continuous-mode	local-light-rerank"
    echo "model-status	continuous-default	$configured_continuous_mode"
    echo "model-status	training-context	$configured_training_context"
    echo "model-status	training-surrounding	$configured_training_surrounding"
    echo "model-status	send-recent-input	$configured_send_recent_input"
    echo "model-status	send-surrounding	$configured_send_surrounding"
    echo "model-status	continuous-toggle	Shift+F9"
    echo "model-status	analyze-window	$analyze_window_command"
    echo "model-status	supervision-window	$supervision_window_command"
    echo "model-status	analyze-learn	$analyze_window_command --learn-output"
    echo "model-status	self-test-command	$self_test_command --current --config $config_file"
    echo "model-status	dry-run-test-command	$self_test_command --current --config $config_file --adapter-dry-run"
    case "$configured_mode" in
        llama-cpp|ollama|openai|openai-compatible)
            echo "model-status	dry-run-test-supported	1"
            ;;
        *)
            echo "model-status	dry-run-test-supported	0"
            ;;
    esac
    echo "model-status	process-command	${TIPE_MODEL_COMMAND:-unset}"
    echo "model-status	process-command-scope	current-shell-environment"
    echo "model-status	process-command-active-scope	current-shell-only-not-fcitx5-runtime"
    echo "model-status	runtime-verification	tipe-doctor"
    if is_model_current_command "${TIPE_MODEL_COMMAND:-}"; then
        echo "model-status	process-command-active	1"
    else
        echo "model-status	process-command-active	0"
        echo "model-status	activation-hint	tipe-restart-fcitx5"
    fi
    echo "restart-env	TIPE_MODEL_COMMAND=$current_command"
}

run_config_test() {
    local test_config_file="$1"
    local dry_run_model="${2:-0}"
    local self_test=""
    if [[ -x "$script_dir/model-self-test.sh" ]]; then
        self_test="$script_dir/model-self-test.sh"
    elif [[ -x "$script_dir/tipe-model-self-test" ]]; then
        self_test="$script_dir/tipe-model-self-test"
    elif [[ -x "$HOME/.local/bin/tipe-model-self-test" ]]; then
        self_test="$HOME/.local/bin/tipe-model-self-test"
    else
        echo "tipe-model-self-test helper is not available" >&2
        exit 1
    fi
    if [[ "$dry_run_model" == "1" ]]; then
        "$self_test" --current --config "$test_config_file" --adapter-dry-run
    else
        "$self_test" --current --config "$test_config_file"
    fi
}

emit_api_key_reference() {
    local fallback_env="${1:-}"
    local stored_path="${api_key_reference_path:-$api_key_file_default}"
    if [[ "$api_key_stdin" == "1" ]]; then
        echo "export TIPE_MODEL_API_KEY_FILE=$(shell_quote "$stored_path")"
    elif [[ -n "$api_key_env" ]]; then
        printf 'export TIPE_MODEL_API_KEY="${%s:-}"\n' "$api_key_env"
    elif [[ "$clear_api_key" != "1" && -f "$api_key_file_default" ]]; then
        echo "export TIPE_MODEL_API_KEY_FILE=$(shell_quote "$api_key_file_default")"
    elif [[ -n "$fallback_env" ]]; then
        printf 'export TIPE_MODEL_API_KEY="${%s:-}"\n' "$fallback_env"
    fi
}

emit_config() {
    local selected_mode="$1"
    cat <<EOF
# Generated by tipe-model-config. Source this file before starting fcitx5,
# or use TIPE_MODEL_COMMAND=$current_command so tipe-model-current can read it.
export TIPE_MODEL_COMMAND=$(shell_quote "$current_command")
export TIPE_MODEL_MODE=$(shell_quote "$selected_mode")
EOF
    case "$selected_mode" in
        off|disabled|none)
            ;;
        heuristic)
            echo "export TIPE_MODEL_BACKEND=heuristic"
            ;;
        personal)
            echo "export TIPE_MODEL_BACKEND=personal"
            if [[ -n "$personal_model_path" ]]; then
                echo "export TIPE_PERSONAL_MODEL_PATH=$(shell_quote "$personal_model_path")"
            fi
            ;;
        dump)
            if [[ -n "$dump_path" ]]; then
                echo "export TIPE_MODEL_DUMP_PATH=$(shell_quote "$dump_path")"
            fi
            ;;
        custom)
            echo "export TIPE_MODEL_CUSTOM_COMMAND=$(shell_quote "$custom_command")"
            ;;
        llama-cpp)
            echo "export TIPE_MODEL_BACKEND=llama-cpp"
            echo "export TIPE_MODEL_NAME=$(shell_quote "${model_name:-$llama_model_default}")"
            echo "export TIPE_LLAMA_CPP_COMMAND=$(shell_quote "${llama_command:-/usr/bin/llama-cli}")"
            echo "export TIPE_LLAMA_CPP_THREADS=$(shell_quote "${llama_threads:-6}")"
            echo "export TIPE_LLAMA_CPP_CONTEXT=$(shell_quote "${llama_context:-8192}")"
            ;;
        ollama)
            echo "export TIPE_MODEL_BACKEND=ollama"
            echo "export TIPE_MODEL_BASE_URL=$(shell_quote "${base_url:-http://127.0.0.1:11434/v1}")"
            echo "export TIPE_MODEL_NAME=$(shell_quote "${model_name:-qwen2.5:0.5b}")"
            echo "export TIPE_MODEL_CHAT_PATH=$(shell_quote "${chat_path:-/chat/completions}")"
            ;;
        openai)
            echo "export TIPE_MODEL_BACKEND=openai-compatible"
            echo "export TIPE_MODEL_BASE_URL=$(shell_quote "${base_url:-https://api.openai.com/v1}")"
            echo "export TIPE_MODEL_NAME=$(shell_quote "${model_name:-your-openai-model-name}")"
            echo "export TIPE_MODEL_CHAT_PATH=$(shell_quote "${chat_path:-/chat/completions}")"
            emit_api_key_reference OPENAI_API_KEY
            ;;
        openai-compatible)
            echo "export TIPE_MODEL_BACKEND=openai-compatible"
            echo "export TIPE_MODEL_BASE_URL=$(shell_quote "${base_url:-https://api.example.com/v1}")"
            echo "export TIPE_MODEL_NAME=$(shell_quote "${model_name:-your-model-name}")"
            echo "export TIPE_MODEL_CHAT_PATH=$(shell_quote "${chat_path:-/chat/completions}")"
            emit_api_key_reference
            ;;
        *)
            echo "unknown model mode: $selected_mode" >&2
            exit 2
            ;;
    esac
    if [[ "$selected_mode" == "openai" || "$selected_mode" == "openai-compatible" ]]; then
        case "$send_recent_input" in
            on|1|true)
                echo "export TIPE_MODEL_SEND_RECENT_INPUT=1"
                ;;
            *)
                echo "export TIPE_MODEL_SEND_RECENT_INPUT=0"
                ;;
        esac
        case "$send_surrounding" in
            on|1|true)
                echo "export TIPE_MODEL_SEND_SURROUNDING=1"
                ;;
            *)
                echo "export TIPE_MODEL_SEND_SURROUNDING=0"
                ;;
        esac
    fi
    if [[ -n "$timeout_seconds" ]]; then
        echo "export TIPE_MODEL_TIMEOUT_SECONDS=$(shell_quote "$timeout_seconds")"
    elif [[ "$selected_mode" == "llama-cpp" ]]; then
        echo "export TIPE_MODEL_TIMEOUT_SECONDS=30"
    fi
    if [[ -n "$http_timeout_seconds" ]]; then
        echo "export TIPE_MODEL_HTTP_TIMEOUT_SECONDS=$(shell_quote "$http_timeout_seconds")"
    fi
    if [[ -n "$temperature" ]]; then
        echo "export TIPE_MODEL_TEMPERATURE=$(shell_quote "$temperature")"
    fi
    if [[ -n "$max_tokens" ]]; then
        echo "export TIPE_MODEL_MAX_TOKENS=$(shell_quote "$max_tokens")"
    fi
    case "$continuous_mode" in
        on|1|true)
            echo "export TIPE_CONTINUOUS_MODE=1"
            ;;
        off|0|false)
            echo "export TIPE_CONTINUOUS_MODE=0"
            ;;
    esac
    case "$training_context" in
        on|1|true)
            echo "export TIPE_PERSONAL_TRAIN_CONTEXT=1"
            ;;
        off|0|false|"")
            echo "export TIPE_PERSONAL_TRAIN_CONTEXT=0"
            ;;
    esac
    case "$training_surrounding" in
        on|1|true)
            echo "export TIPE_PERSONAL_TRAIN_SURROUNDING=1"
            ;;
        off|0|false|"")
            echo "export TIPE_PERSONAL_TRAIN_SURROUNDING=0"
            ;;
    esac
}

if [[ "$mode" == "show" ]]; then
    print_show
    exit 0
fi

case "$mode" in
    off|disabled|none|heuristic|personal|llama-cpp|dump|custom|ollama|openai|openai-compatible)
        ;;
    *)
        echo "unknown model mode: $mode" >&2
        exit 2
        ;;
esac

if [[ "$api_key_stdin" == "1" && ( -n "$api_key_env" || "$clear_api_key" == "1" ) ]]; then
    echo "--api-key-stdin cannot be combined with --api-key-env or --clear-api-key" >&2
    exit 2
fi
if [[ "$api_key_stdin" == "1" ]]; then
    IFS= read -r api_key_value || [[ -n "$api_key_value" ]] || {
        echo "--api-key-stdin requires a non-empty key on stdin" >&2
        exit 2
    }
    if IFS= read -r _extra_api_key_line; then
        echo "API key input must contain exactly one line" >&2
        exit 2
    fi
    if [[ -z "$api_key_value" || "$api_key_value" == *$'\r'* || ${#api_key_value} -gt 16384 ]]; then
        echo "API key must be a non-empty single line of at most 16384 characters" >&2
        exit 2
    fi
fi

validate_key_env
validate_custom_command
validate_personal_model_path
validate_endpoint_settings
validate_llama_settings
[[ -z "$llama_threads" ]] || validate_int_range "--llama-threads" "$llama_threads" 1 256
[[ -z "$llama_context" ]] || validate_int_range "--llama-context" "$llama_context" 512 262144
[[ -z "$timeout_seconds" ]] || validate_int_range "--timeout" "$timeout_seconds" 1 30
[[ -z "$http_timeout_seconds" ]] || validate_int_range "--http-timeout" "$http_timeout_seconds" 1 60
[[ -z "$max_tokens" ]] || validate_int_range "--max-tokens" "$max_tokens" 1 4096
[[ -z "$temperature" ]] || validate_float_range "--temperature" "$temperature" 0 2
case "$continuous_mode" in
    ""|on|off|1|0|true|false)
        ;;
    *)
        echo "--continuous must be on or off" >&2
        exit 2
        ;;
esac
case "$training_context" in
    ""|on|off|1|0|true|false)
        ;;
    *)
        echo "--training-context must be on or off" >&2
        exit 2
        ;;
esac
case "$training_surrounding" in
    ""|on|off|1|0|true|false)
        ;;
    *)
        echo "--training-surrounding must be on or off" >&2
        exit 2
        ;;
esac
case "$send_recent_input" in
    ""|on|off|1|0|true|false)
        ;;
    *)
        echo "--send-recent-input must be on or off" >&2
        exit 2
        ;;
esac
case "$send_surrounding" in
    ""|on|off|1|0|true|false)
        ;;
    *)
        echo "--send-surrounding must be on or off" >&2
        exit 2
        ;;
esac

config_payload=$(emit_config "$mode")
if [[ "$dry_run" == "1" ]]; then
    printf '%s\n' "$config_payload"
    if [[ "$test_config" == "1" ]]; then
        test_config_file=$(mktemp)
        test_api_key_file=""
        trap 'rm -f "$test_config_file" "$test_api_key_file"' EXIT
        if [[ "$api_key_stdin" == "1" ]]; then
            test_api_key_file=$(mktemp)
            chmod 0600 "$test_api_key_file"
            printf '%s\n' "$api_key_value" >"$test_api_key_file"
            api_key_reference_path="$test_api_key_file"
            emit_config "$mode" >"$test_config_file"
            api_key_reference_path=""
        else
            printf '%s\n' "$config_payload" >"$test_config_file"
        fi
        run_config_test "$test_config_file" "$test_dry_run"
    fi
    exit 0
fi

config_dir=$(dirname -- "$config_file")
config_name=$(basename -- "$config_file")
umask 077
mkdir -p "$config_dir"
config_temporary=""
api_key_temporary=""
cleanup_config_temporary() {
    [[ -z "$config_temporary" ]] || rm -f -- "$config_temporary"
    [[ -z "$api_key_temporary" ]] || rm -f -- "$api_key_temporary"
}
trap cleanup_config_temporary EXIT
if [[ "$api_key_stdin" == "1" ]]; then
    api_key_name=$(basename -- "$api_key_file_default")
    api_key_temporary=$(mktemp "$config_dir/.${api_key_name}.tmp.XXXXXX")
    printf '%s\n' "$api_key_value" >"$api_key_temporary"
    chmod 0600 "$api_key_temporary"
fi
config_temporary=$(mktemp "$config_dir/.${config_name}.tmp.XXXXXX")
printf '%s\n' "$config_payload" >"$config_temporary"
chmod 0600 "$config_temporary"
if [[ "$test_config" == "1" ]]; then
    if [[ -n "$api_key_temporary" ]]; then
        api_key_reference_path="$api_key_temporary"
        emit_config "$mode" >"$config_temporary"
        api_key_reference_path=""
    fi
    run_config_test "$config_temporary" "$test_dry_run"
    if [[ -n "$api_key_temporary" ]]; then
        printf '%s\n' "$config_payload" >"$config_temporary"
    fi
fi
if [[ -n "$api_key_temporary" ]]; then
    mv -f -- "$api_key_temporary" "$api_key_file_default"
    api_key_temporary=""
fi
mv -f -- "$config_temporary" "$config_file"
config_temporary=""
if [[ "$clear_api_key" == "1" ]]; then
    rm -f -- "$api_key_file_default"
fi
trap - EXIT
echo "wrote	$config_file"
echo "restart-env	TIPE_MODEL_COMMAND=$current_command"
echo "self-test-command	$self_test_command --current --config $config_file"
echo "dry-run-test-command	$self_test_command --current --config $config_file --adapter-dry-run"
echo "activation-hint	tipe-restart-fcitx5"
echo "note	No fcitx5 restart or input-method switch was performed."
