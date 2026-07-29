#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 [--no-runtime]

Print a read-only TiPE status report. This does not restart fcitx5, switch input
methods, edit profile files, or call model endpoints.

  --no-runtime  skip fcitx5-remote and process checks
EOF
}

runtime=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-runtime)
            runtime=0
            shift
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
done

status() {
    printf '%s\t%s\t%s\n' "$1" "$2" "$3"
}

fcitx_remote_error=""
current_input_method_result=""
session_dbus_reachable() {
    local output status_code

    if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
        fcitx_remote_error="DBUS_SESSION_BUS_ADDRESS is not set"
        return 1
    fi
    if [[ -n "${TIPE_DOCTOR_FAKE_FCITX5_PID:-}" ]]; then
        return 0
    fi
    if ! command -v busctl >/dev/null 2>&1; then
        return 0
    fi

    set +e
    output=$(busctl --user --no-pager status org.fcitx.Fcitx5 2>&1)
    status_code=$?
    set -e
    if (( status_code != 0 )); then
        fcitx_remote_error="session D-Bus or fcitx5 service is not reachable"
        if [[ -n "$output" ]]; then
            fcitx_remote_error+=": $(one_line "$output")"
        fi
        return 1
    fi
    return 0
}

current_input_method() {
    local output status_code attempt
    fcitx_remote_error=""
    current_input_method_result=""
    session_dbus_reachable || return 1
    for attempt in 1 2 3; do
        set +e
        output=$(fcitx5-remote -n 2>&1)
        status_code=$?
        set -e
        if (( status_code == 0 )) && [[ -n "$output" ]]; then
            current_input_method_result="$output"
            return 0
        fi
        if [[ -n "$output" ]]; then
            fcitx_remote_error="$output"
        elif (( status_code == 0 )); then
            fcitx_remote_error="empty output"
        else
            fcitx_remote_error="exit status $status_code with empty output"
        fi
        sleep 0.2
    done
    return 1
}

one_line() {
    local text="$1"
    text="${text//$'\n'/ }"
    text="${text//$'\r'/ }"
    text="${text//$'\t'/ }"
    printf '%s\n' "$text"
}

path_status() {
    local label="$1"
    local path="$2"
    if [[ -e "$path" ]]; then
        if [[ -x "$path" ]]; then
            status path "$label" "$path executable"
        else
            status path "$label" "$path present"
        fi
    else
        status missing "$label" "$path"
    fi
}

optional_path_status() {
    local label="$1"
    local path="$2"
    local reason="${3:-optional; not configured}"
    if [[ -e "$path" ]]; then
        path_status "$label" "$path"
    else
        status skip "$label" "$path ($reason)"
    fi
}

emit_diagnostic_log_summary() {
    local label="$1"
    local path="$2"
    [[ -r "$path" ]] || return 0

    local size_bytes
    size_bytes=$(wc -c <"$path" | tr -d '[:space:]')
    status "$label" size-bytes "$size_bytes"
    status "$label" trim-limit-bytes 4194304
    if (( size_bytes > 4194304 )); then
        status warn "$label" "log exceeds the bounded runtime target; restart fcitx5 once to load the current logger"
    else
        status ok "$label" "log size is within the bounded runtime target"
    fi
}

split_tsv_preserve_empty() {
    local text="$1"
    local -n output_ref="$2"
    output_ref=()
    while [[ "$text" == *$'\t'* ]]; do
        output_ref+=("${text%%$'\t'*}")
        text="${text#*$'\t'}"
    done
    output_ref+=("$text")
}

emit_supervision_history_summary() {
    local path="$1"
    local label="${2:-supervision-history}"
    local trim_limit="${3:-262144}"
    [[ -r "$path" ]] || return 0

    local size_bytes
    size_bytes=$(wc -c <"$path" | tr -d '[:space:]')
    status "$label" size-bytes "$size_bytes"
    status "$label" trim-limit-bytes "$trim_limit"
    if mtime_epoch=$(stat -c '%Y' "$path" 2>/dev/null); then
        status "$label" mtime-epoch "$mtime_epoch"
    fi

    local line records=0 valid_requests=0 active_records=0 pass_through_records=0
    local in_record=0 record_has_protocol=0 record_mode=""
    local latest_unix_ms="" latest_program="" latest_preedit="" latest_candidates="" latest_expanded=""
    local first_content_line="" first_record_header=0

    while IFS= read -r line || [[ -n "$line" ]]; do
        if [[ -z "$first_content_line" && -n "$line" ]]; then
            first_content_line="$line"
        fi

        if [[ "$line" == $'---\t'* ]]; then
            if [[ "$in_record" == 1 ]]; then
                if [[ "$record_has_protocol" == 1 ]]; then
                    valid_requests=$((valid_requests + 1))
                fi
                case "$record_mode" in
                    active-preedit)
                        active_records=$((active_records + 1))
                        ;;
                    pass-through-only)
                        pass_through_records=$((pass_through_records + 1))
                        ;;
                esac
            fi

            in_record=1
            record_has_protocol=0
            record_mode=""
            records=$((records + 1))
            [[ "$records" == 1 ]] && first_record_header=1

            local fields=()
            local header_payload="${line#$'---\t'}"
            split_tsv_preserve_empty "$header_payload" fields
            local index
            for ((index = 0; index + 1 < ${#fields[@]}; index += 2)); do
                case "${fields[index]}" in
                    unix_ms)
                        latest_unix_ms="${fields[index + 1]}"
                        ;;
                    program)
                        latest_program="${fields[index + 1]}"
                        ;;
                    preedit)
                        latest_preedit="${fields[index + 1]}"
                        ;;
                    candidates)
                        latest_candidates="${fields[index + 1]}"
                        ;;
                    expanded)
                        latest_expanded="${fields[index + 1]}"
                        ;;
                esac
            done
        elif [[ "$in_record" == 1 && "$line" == $'protocol\t1' ]]; then
            record_has_protocol=1
        elif [[ "$in_record" == 1 && "$line" == $'supervision_state\tmode\t'* ]]; then
            local state_fields=()
            IFS=$'\t' read -r -a state_fields <<<"$line"
            if [[ "${state_fields[1]:-}" == "mode" ]]; then
                record_mode="${state_fields[2]:-}"
            fi
        fi
    done <"$path"

    if [[ "$in_record" == 1 ]]; then
        if [[ "$record_has_protocol" == 1 ]]; then
            valid_requests=$((valid_requests + 1))
        fi
        case "$record_mode" in
            active-preedit)
                active_records=$((active_records + 1))
                ;;
            pass-through-only)
                pass_through_records=$((pass_through_records + 1))
                ;;
        esac
    fi

    status "$label" records "$records"
    status "$label" valid-requests "$valid_requests"
    status "$label" active-records "$active_records"
    status "$label" pass-through-records "$pass_through_records"
    if [[ "$records" -gt 0 ]]; then
        status "$label" latest-unix-ms "${latest_unix_ms:-unknown}"
        status "$label" latest-program "${latest_program:-unknown}"
        status "$label" latest-preedit "${latest_preedit:-}"
        status "$label" latest-candidates "${latest_candidates:-unknown}"
        status "$label" latest-expanded "${latest_expanded:-unknown}"
    fi
    if [[ "$size_bytes" -gt "$trim_limit" ]]; then
        status warn "$label" "larger than trim limit; live engine should trim on next append"
    fi
    if [[ -n "$first_content_line" && "$first_record_header" == 0 ]]; then
        status warn "$label" "first non-empty row is not a history record header"
    fi
    if [[ "$records" -gt 0 && "$valid_requests" == 0 ]]; then
        status warn "$label" "no complete protocol 1 requests found"
    fi
}

default_data_home() {
    if [[ -n "${XDG_DATA_HOME:-}" ]]; then
        printf '%s\n' "$XDG_DATA_HOME"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.local/share"
    else
        return 1
    fi
}

default_cache_home() {
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        printf '%s\n' "$XDG_CACHE_HOME"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.cache"
    else
        return 1
    fi
}

helper_path() {
    local name="$1"
    local source_helper=""
    if [[ "${TIPE_DOCTOR_SOURCE_HELPERS:-0}" != "1" ]] && command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    if [[ -n "${BASH_SOURCE[0]:-}" ]]; then
        local script_dir
        script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
        case "$name" in
            tipe-state-probe)
                [[ -x "$script_dir/../build/tipe-state-probe" ]] && source_helper="$script_dir/../build/tipe-state-probe"
                ;;
            tipe-check-user-dictionary)
                [[ -x "$script_dir/check-user-dictionary.sh" ]] && source_helper="$script_dir/check-user-dictionary.sh"
                ;;
            tipe-check-preferences)
                [[ -x "$script_dir/check-preferences.sh" ]] && source_helper="$script_dir/check-preferences.sh"
                ;;
            tipe-personal-model)
                [[ -x "$script_dir/personal-model.py" ]] && source_helper="$script_dir/personal-model.py"
                ;;
        esac
        if [[ -n "$source_helper" ]]; then
            printf '%s\n' "$source_helper"
            return 0
        fi
    fi
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    return 1
}

custom_command_executable() {
    local command_line="$1"
    command_line="${command_line//\\ / }"
    [[ -n "$command_line" && "$command_line" =~ ^[A-Za-z0-9_./:=+\ -]+$ ]] || return 1

    local tokens=()
    read -r -a tokens <<< "$command_line"
    while [[ "${#tokens[@]}" -gt 0 && "${tokens[0]}" =~ ^[A-Za-z_][A-Za-z0-9_]*=.+$ ]]; do
        tokens=("${tokens[@]:1}")
    done
    [[ "${#tokens[@]}" -gt 0 ]] || return 1
    printf '%s\n' "${tokens[0]}"
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
            printf '%s\n' "disabled"
            ;;
        dump)
            printf '%s\n' "request-dump"
            ;;
        custom)
            printf '%s\n' "custom-wrapper"
            ;;
        personal)
            printf '%s\n' "personal-reranker"
            ;;
        llama-cpp)
            printf '%s\n' "local-llama-cpp"
            ;;
        ollama)
            printf '%s\n' "local-http:$selected_backend"
            ;;
        openai)
            printf '%s\n' "official-openai:$selected_backend"
            ;;
        openai-compatible)
            printf '%s\n' "openai-compatible:$selected_backend"
            ;;
        heuristic|"")
            printf '%s\n' "offline-heuristic"
            ;;
        *)
            printf '%s\n' "unknown"
            ;;
    esac
}

process_env_value() {
    local pid="$1"
    local name="$2"
    local env_file="/proc/$pid/environ"
    [[ -r "$env_file" ]] || return 1
    tr '\0' '\n' <"$env_file" | sed -n "s/^$name=//p" | sed -n '1p'
}

system_rime_dictionary_paths() {
    if [[ -n "${TIPE_SYSTEM_RIME_DICTIONARY:-}" ]]; then
        printf '%s\n' "$TIPE_SYSTEM_RIME_DICTIONARY"
        return 0
    fi
    printf '%s\n' /usr/share/rime-data/pinyin_simp.dict.yaml
    printf '%s\n' /usr/share/rime-data/luna_pinyin.dict.yaml
}

rime_dictionary_entry_count() {
    local path="$1"
    [[ -r "$path" ]] || return 1
    awk -F '\t' '
        BEGIN { in_body = 0; count = 0 }
        !in_body {
            if ($0 == "...") {
                in_body = 1
            }
            next
        }
        /^[[:space:]]*$/ || /^#/ {
            next
        }
        NF >= 2 && $2 ~ /^[a-z]+( [a-z]+)*$/ {
            count += 1
        }
        END { print count }
    ' "$path"
}

first_probe_candidate() {
    local probe="$1"
    local pinyin="$2"
    shift 2
    "$probe" "$pinyin" "$@" 2>/dev/null | awk -F '\t' '$1 == "candidate" && $2 == "0" { print $3; exit }'
}

emit_dictionary_summary() {
    status section dictionary ""

    local libime_dictionary=/usr/share/libime/sc.dict
    local libime_language_model=""
    if [[ -r /usr/lib64/libime/zh_CN.lm ]]; then
        libime_language_model=/usr/lib64/libime/zh_CN.lm
    elif [[ -r /usr/lib/libime/zh_CN.lm ]]; then
        libime_language_model=/usr/lib/libime/zh_CN.lm
    fi
    if [[ -r "$libime_dictionary" && -n "$libime_language_model" ]]; then
        status ok libime-pinyin "$libime_dictionary language-model=$libime_language_model"
    else
        status skip libime-pinyin "LibIME dictionary or zh_CN language model not found; fallback backends remain available"
    fi

    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libpinyin; then
        status dictionary libpinyin "$(pkg-config --modversion libpinyin 2>/dev/null || printf present)"
    else
        status warn dictionary "libpinyin pkg-config metadata not found"
    fi
    if [[ -d /usr/lib64/libpinyin/data ]]; then
        status ok dictionary-data "/usr/lib64/libpinyin/data"
    elif [[ -d /usr/lib/libpinyin/data ]]; then
        status ok dictionary-data "/usr/lib/libpinyin/data"
    else
        status warn dictionary-data "libpinyin data directory not found"
    fi

    local path count found_system=0
    while IFS= read -r path; do
        [[ -n "$path" ]] || continue
        if [[ -r "$path" ]]; then
            found_system=1
            count=$(rime_dictionary_entry_count "$path" 2>/dev/null || true)
            status ok system-rime "$path entries=${count:-unknown}"
        else
            status skip system-rime "$path (not readable)"
        fi
    done < <(system_rime_dictionary_paths)
    if [[ "$found_system" == 0 ]]; then
        status warn system-rime "no readable system Rime pinyin dictionary found"
    fi

    local probe
    probe=$(helper_path tipe-state-probe || true)
    if [[ -z "$probe" || ! -x "$probe" ]]; then
        status skip dictionary-sample "tipe-state-probe not available"
        return 0
    fi

    local sample base learned
    for sample in n nihao github woc woxiangyo shenglue shurufa zhexiedoushiganmade; do
        base=$(first_probe_candidate "$probe" "$sample" || true)
        learned=$(first_probe_candidate "$probe" "$sample" --user-data || true)
        if [[ -n "$base" || -n "$learned" ]]; then
            status dictionary-sample "$sample" "base=${base:-none} user=${learned:-none}"
        else
            status warn dictionary-sample "$sample produced no candidates"
        fi
    done
}

find_process_pids() {
    local command_name="$1"
    if command -v pgrep >/dev/null 2>&1; then
        pgrep -x "$command_name" 2>/dev/null && return 0
    fi
    ps -eo pid=,comm=,args= 2>/dev/null | awk -v name="$command_name" '
        $2 == name {
            print $1
            next
        }
        $0 ~ ("(^|/)" name "($| )") {
            print $1
        }
    '
}

emit_learning_summary() {
    local checker="$1"
    local preferences="$2"
    local summary_output line kind rest rank count field_a field_b field_c field_d

    if [[ ! -e "$preferences" ]]; then
        return 0
    fi
    if ! summary_output=$("$checker" --summary --top 3 "$preferences" 2>/dev/null); then
        status warn learning "summary unavailable: $preferences"
        return 0
    fi

    status section learning ""
    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ -z "$line" ]] && continue
        kind="${line%%$'\t'*}"
        if [[ "$line" == *$'\t'* ]]; then
            rest="${line#*$'\t'}"
        else
            rest=""
        fi
        case "$kind" in
            summary)
                IFS=$'\t' read -r field_a count field_b field_c field_d <<< "$rest"
                case "$field_a" in
                    rows)
                        status learning rows "$count"
                        ;;
                    preferences)
                        status learning preferences "rows=$count total=$field_b"
                        ;;
                    legacy-preferences)
                        status learning legacy-preferences "rows=$count total=$field_b"
                        ;;
                    preference-evidence)
                        status learning preference-evidence "active=$field_b inactive=$field_d"
                        ;;
                    supervised-raw-tokens)
                        status learning supervised-raw-tokens "rows=$count total=$field_b"
                        ;;
                    corrections)
                        status learning corrections "rows=$count total=$field_b"
                        ;;
                    correction-patterns)
                        status learning correction-patterns "patterns=$count total=$field_b"
                        ;;
                    runtime-correction-patterns)
                        status learning runtime-correction-patterns \
                            "rows=$count total=$field_b active=${field_d:-0}"
                        ;;
                    runtime-key-habits)
                        status learning runtime-key-habits \
                            "rows=$count total=$field_b active=${field_d:-0}"
                        ;;
                    segment-chains)
                        status learning segment-chains "rows=$count total=$field_b"
                        ;;
                esac
                ;;
            top-preference)
                IFS=$'\t' read -r rank count field_a field_b <<< "$rest"
                status learning "top-preference-$rank" "$field_a -> $field_b count=$count"
                ;;
            top-supervised-raw-token)
                IFS=$'\t' read -r rank count field_a <<< "$rest"
                status learning "top-supervised-raw-token-$rank" "$field_a count=$count"
                ;;
            top-correction)
                IFS=$'\t' read -r rank count field_a field_b <<< "$rest"
                status learning "top-correction-$rank" "$field_a -> $field_b count=$count"
                ;;
            top-correction-pattern)
                IFS=$'\t' read -r rank count field_a field_b field_c <<< "$rest"
                status learning "top-correction-pattern-$rank" "$field_a $field_b $field_c count=$count"
                ;;
            top-runtime-correction-pattern)
                local -a runtime_fields=()
                readarray -td $'\t' runtime_fields < <(printf '%s\t' "$rest")
                rank="${runtime_fields[0]:-}"
                count="${runtime_fields[1]:-}"
                field_a="${runtime_fields[2]:-}"
                field_b="${runtime_fields[3]:-}"
                field_c="${runtime_fields[4]:-}"
                field_d="${runtime_fields[5]:-}"
                local runtime_state="${runtime_fields[7]:-inactive-evidence}"
                local runtime_required="${runtime_fields[8]:-?}"
                status learning "top-runtime-correction-pattern-$rank" \
                    "$field_a ${field_b:--}->${field_c:--} position=$field_d count=$count status=$runtime_state requires=$runtime_required"
                ;;
            top-runtime-key-habit)
                local -a runtime_fields=()
                readarray -td $'\t' runtime_fields < <(printf '%s\t' "$rest")
                rank="${runtime_fields[0]:-}"
                count="${runtime_fields[1]:-}"
                field_a="${runtime_fields[2]:-}"
                field_b="${runtime_fields[3]:-}"
                field_c="${runtime_fields[4]:-}"
                local runtime_state="${runtime_fields[5]:-inactive-evidence}"
                local runtime_required="${runtime_fields[6]:-?}"
                status learning "top-runtime-key-habit-$rank" \
                    "$field_a ${field_b:--}->${field_c:--} count=$count status=$runtime_state requires=$runtime_required"
                ;;
            top-segment-chain)
                IFS=$'\t' read -r rank count field_a field_b <<< "$rest"
                status learning "top-segment-chain-$rank" "$field_a -> $field_b count=$count"
                ;;
        esac
    done <<< "$summary_output"
}

ini_numbered_values() {
    local path="$1"
    local section="$2"
    awk -v wanted="[$section]" '
        /^[[:space:]]*\[/ {
            in_section = ($0 == wanted)
            next
        }
        in_section && /^[[:space:]]*[0-9]+=/ {
            value = $0
            sub(/^[[:space:]]*[0-9]+=/, "", value)
            print tolower(value)
        }
    ' "$path"
}

emit_input_switch_integration() {
    local keybinds_path="${TIPE_NIRI_KEYBINDS:-$HOME/.config/niri/keybinds.kdl}"
    local fcitx_config_path="${TIPE_FCITX5_CONFIG:-$HOME/.config/fcitx5/config}"
    status section integration ""
    if [[ ! -r "$keybinds_path" ]]; then
        status skip niri-mode-toggle "$keybinds_path is not readable; bind the Chinese/English shortcut to tipe-toggle"
    else
        status integration niri-keybinds "$keybinds_path"
        if grep -Eq '^[[:space:]]*(Mod|Super)\+Space[^\{]*\{[^}]*tipe-toggle([[:space:]";}]|$)' "$keybinds_path"; then
            status ok niri-mode-toggle "Mod+Space uses tipe-toggle; TiPE remains active for English supervision"
        elif grep -Eq '^[[:space:]]*(Mod|Super)\+Space[^\{]*\{[^}]*fcitx5-remote[[:space:]]+-(t|c)([[:space:]";}]|$)' "$keybinds_path"; then
            status warn niri-mode-toggle "Mod+Space deactivates fcitx5 with fcitx5-remote; English supervision cannot run (use tipe-toggle)"
        else
            status skip niri-mode-toggle "no supported Mod+Space binding found; bind it to tipe-toggle for supervised English mode"
        fi
        if grep -Eq '^[[:space:]]*(Ctrl|Control)\+Space[^\{]*\{[^}]*tipe-toggle([[:space:]";}]|$)' "$keybinds_path"; then
            status ok niri-control-toggle "Ctrl+Space uses tipe-toggle"
        else
            status skip niri-control-toggle "Ctrl+Space is not bound to tipe-toggle"
        fi
    fi

    if [[ ! -r "$fcitx_config_path" ]]; then
        status skip fcitx5-toggle-conflicts "$fcitx_config_path is not readable"
        return 0
    fi
    status integration fcitx5-config "$fcitx_config_path"
    local conflicts=()
    if ini_numbered_values "$fcitx_config_path" "Hotkey/TriggerKeys" |
        grep -Eq '^(control|ctrl)\+space$'; then
        conflicts+=("Ctrl+Space trigger")
    fi
    if ini_numbered_values "$fcitx_config_path" "Hotkey/AltTriggerKeys" |
        grep -Eq '^shift_(l|r)$'; then
        conflicts+=("bare Shift trigger")
    fi
    if ini_numbered_values "$fcitx_config_path" "Hotkey/EnumerateGroupForwardKeys" |
        grep -Eq '^(super|mod)\+space$'; then
        conflicts+=("Super+Space group switch")
    fi
    if ini_numbered_values "$fcitx_config_path" "Hotkey/EnumerateGroupBackwardKeys" |
        grep -Eq '^shift\+(super|mod)\+space$|^(super|mod)\+shift\+space$'; then
        conflicts+=("Shift+Super+Space group switch")
    fi
    if (( ${#conflicts[@]} > 0 )); then
        local joined
        joined=$(IFS=', '; printf '%s' "${conflicts[*]}")
        status warn fcitx5-toggle-conflicts "$joined can deactivate or rotate away from TiPE"
    else
        status ok fcitx5-toggle-conflicts "no common fcitx5 shortcut bypasses tipe-toggle"
    fi
}

if [[ -z "${HOME:-}" ]]; then
    echo "HOME is not set; TiPE user paths cannot be inspected" >&2
    exit 1
fi

data_home=$(default_data_home)
cache_home=$(default_cache_home)
dictionary_path="${TIPE_USER_DICTIONARY:-$data_home/tipe/user-dictionary.tsv}"
preferences_path="$data_home/tipe/candidate-preferences.tsv"
if [[ ${TIPE_LIBIME_USER_HISTORY+x} ]]; then
    libime_history_path="$TIPE_LIBIME_USER_HISTORY"
else
    libime_history_path="$data_home/tipe/libime/user.history"
fi
fcitx_log="$cache_home/tipe/fcitx5.log"
tipeui_log="$cache_home/tipe/tipeui.log"
candidate_window_log="$cache_home/tipe/candidate-window.log"
engine_trace_log="$cache_home/tipe/engine-trace.log"
slow_key_events_log="$cache_home/tipe/slow-key-events.log"
supervision_snapshot="$cache_home/tipe/supervision-current.tsv"
supervision_history="$cache_home/tipe/supervision-history.tsv"
supervision_training_history="$cache_home/tipe/supervision-training-history.tsv"
if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
    input_mode_path="$XDG_RUNTIME_DIR/tipe/input-mode"
else
    input_mode_path="$cache_home/tipe/input-mode"
fi
input_mode_applied_path="$(dirname "$input_mode_path")/input-mode-applied"
runtime_current_method=""
runtime_current_error=""
runtime_current_checked=0
fcitx_service_active=0
if [[ "$runtime" == 1 ]] && command -v fcitx5-remote >/dev/null 2>&1; then
    runtime_current_checked=1
    if current_input_method; then
        runtime_current_method="$current_input_method_result"
    else
        runtime_current_error="$fcitx_remote_error"
    fi
fi
if [[ "$runtime" == 1 ]] && command -v systemctl >/dev/null 2>&1 &&
    systemctl --user is-active --quiet fcitx5.service >/dev/null 2>&1; then
    fcitx_service_active=1
fi

status section environment ""
status env HOME "$HOME"
status env XDG_DATA_HOME "${XDG_DATA_HOME:-}"
status env XDG_CACHE_HOME "${XDG_CACHE_HOME:-}"
status env TIPE_USER_DICTIONARY "${TIPE_USER_DICTIONARY:-}"
status env TIPE_MODEL_COMMAND "${TIPE_MODEL_COMMAND:-}"
status env TIPE_RERANK_COMMAND "${TIPE_RERANK_COMMAND:-}"
status env TIPE_MODEL_CONFIG "${TIPE_MODEL_CONFIG:-}"
status env TIPE_MODEL_MODE "${TIPE_MODEL_MODE:-}"
status env TIPE_MODEL_BACKEND "${TIPE_MODEL_BACKEND:-heuristic}"
status env TIPE_MODEL_CUSTOM_COMMAND "${TIPE_MODEL_CUSTOM_COMMAND:-}"
status env TIPE_MODEL_BASE_URL "${TIPE_MODEL_BASE_URL:-}"
status env TIPE_MODEL_NAME "${TIPE_MODEL_NAME:-}"
status env TIPE_MODEL_TIMEOUT_SECONDS "${TIPE_MODEL_TIMEOUT_SECONDS:-2}"
status env TIPE_MODEL_HTTP_TIMEOUT_SECONDS "${TIPE_MODEL_HTTP_TIMEOUT_SECONDS:-8}"
status env TIPE_LLAMA_CPP_COMMAND "${TIPE_LLAMA_CPP_COMMAND:-}"
status env TIPE_LLAMA_CPP_THREADS "${TIPE_LLAMA_CPP_THREADS:-}"
status env TIPE_LLAMA_CPP_CONTEXT "${TIPE_LLAMA_CPP_CONTEXT:-}"
status env TIPE_CONTINUOUS_MODE "${TIPE_CONTINUOUS_MODE:-}"
status env TIPE_DEBUG "${TIPE_DEBUG:-0}"
status env TIPE_CANDIDATE_DEBUG "${TIPE_CANDIDATE_DEBUG:-0}"
status env TIPE_WAYLAND_POPUP_EDGE_FALLBACK "${TIPE_WAYLAND_POPUP_EDGE_FALLBACK:-0}"
status env TIPE_WAYLAND_POPUP_EDGE_LEFT "${TIPE_WAYLAND_POPUP_EDGE_LEFT:-1180}"
status env TIPE_WAYLAND_POPUP_EDGE_TOP "${TIPE_WAYLAND_POPUP_EDGE_TOP:-620}"
status env TIPE_STATUS_EDGE_FALLBACK "${TIPE_STATUS_EDGE_FALLBACK:-0}"
status env TIPE_STATUS_LEFT "${TIPE_STATUS_LEFT:-64}"
status env TIPE_STATUS_TOP "${TIPE_STATUS_TOP:-72}"

status section install ""
path_status libtipe "$HOME/.local/lib64/fcitx5/libtipe.so"
path_status libtipeui "$HOME/.local/lib64/fcitx5/libtipeui.so"
path_status addon-metadata "$HOME/.local/share/fcitx5/addon/tipe.conf"
path_status ui-metadata "$HOME/.local/share/fcitx5/addon/tipeui.conf"
path_status input-method "$HOME/.local/share/fcitx5/inputmethod/tipe.conf"
path_status candidate-window "$HOME/.local/bin/tipe-candidate-window"
optional_path_status wine-caret-bridge "$HOME/.local/libexec/tipe/tipe-wine-caret-bridge.exe" \
    "optional when no MinGW compiler was available at build time"
path_status state-probe "$HOME/.local/bin/tipe-state-probe"
path_status model-adapter "$HOME/.local/bin/tipe-model-adapter"
path_status model-dump "$HOME/.local/bin/tipe-model-dump"
path_status model-explain "$HOME/.local/bin/tipe-model-explain"
path_status learning-panel "$HOME/.local/bin/tipe-learning-panel"
path_status supervision-window "$HOME/.local/bin/tipe-supervision-window"
path_status analyze-window "$HOME/.local/bin/tipe-analyze-window"
path_status model-replay "$HOME/.local/bin/tipe-model-replay"
path_status model-current "$HOME/.local/bin/tipe-model-current"
path_status model-config "$HOME/.local/bin/tipe-model-config"
path_status model-self-test "$HOME/.local/bin/tipe-model-self-test"
path_status model-wrapper-new "$HOME/.local/bin/tipe-model-wrapper-new"
path_status model-wrapper-check "$HOME/.local/bin/tipe-model-wrapper-check"
path_status training-export "$HOME/.local/bin/tipe-training-export"
path_status personal-model "$HOME/.local/bin/tipe-personal-model"
path_status personal-model-train "$HOME/.local/bin/tipe-personal-model-train"
path_status dictionary-check "$HOME/.local/bin/tipe-check-user-dictionary"
path_status preferences-check "$HOME/.local/bin/tipe-check-preferences"
path_status doctor "$HOME/.local/bin/tipe-doctor"
path_status restart-helper "$HOME/.local/bin/tipe-restart-fcitx5"
path_status toggle-helper "$HOME/.local/bin/tipe-toggle"
path_status desktop-entry "$HOME/.local/share/applications/tipe-supervision.desktop"
path_status app-icon "$HOME/.local/share/icons/hicolor/scalable/apps/tipe.svg"
path_status app-icon-wm-class "$HOME/.local/share/icons/hicolor/scalable/apps/dev.tipe.LearningPanel.svg"

if [[ -e "$HOME/.local/share/fcitx5/addon/tipe.conf" ]]; then
    if grep -Fxq "Library=$HOME/.local/share/fcitx5/addon/libtipe" "$HOME/.local/share/fcitx5/addon/tipe.conf"; then
        status ok addon-library "$HOME/.local/share/fcitx5/addon/libtipe"
    else
        status warn addon-library "unexpected Library row in installed tipe.conf"
    fi
fi
if [[ -e "$HOME/.local/share/fcitx5/addon/tipeui.conf" ]]; then
    if grep -Fxq "Library=$HOME/.local/share/fcitx5/addon/libtipeui" "$HOME/.local/share/fcitx5/addon/tipeui.conf"; then
        status ok ui-library "$HOME/.local/share/fcitx5/addon/libtipeui"
    else
        status warn ui-library "unexpected Library row in installed tipeui.conf"
    fi
fi

xim_config_path="${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/conf/xim.conf"
if [[ -r "$xim_config_path" ]]; then
    if grep -Eiq '^[[:space:]]*UseOnTheSpot[[:space:]]*=[[:space:]]*(True|1|Yes|On)[[:space:]]*$' \
        "$xim_config_path"; then
        status ok xim-on-the-spot "$xim_config_path enabled"
    else
        status warn xim-on-the-spot "$xim_config_path does not enable UseOnTheSpot; Wine client preedit may be unavailable"
    fi
else
    status skip xim-on-the-spot "$xim_config_path not readable; Wine client preedit may require UseOnTheSpot=True"
fi

emit_input_switch_integration

status section data ""
input_mode="chinese"
input_mode_token=""
if [[ -r "$input_mode_path" ]]; then
    read -r input_mode input_mode_token <"$input_mode_path" || input_mode="chinese"
    case "${input_mode,,}" in
        english|eng) input_mode="english" ;;
        *) input_mode="chinese" ;;
    esac
    status path input-mode-file "$input_mode_path present"
else
    status skip input-mode-file "$input_mode_path (missing means Chinese mode)"
fi
status input-mode current "$input_mode"
if [[ -n "$input_mode_token" ]]; then
    applied_mode=""
    applied_token=""
    if [[ -r "$input_mode_applied_path" ]]; then
        read -r applied_mode applied_token <"$input_mode_applied_path" || true
    fi
    case "${applied_mode,,}" in
        english|eng) applied_mode="english" ;;
        chinese|zh) applied_mode="chinese" ;;
        *) applied_mode="" ;;
    esac
    if [[ "$applied_mode" == "$input_mode" && "$applied_token" == "$input_mode_token" ]]; then
        status ok input-mode-applied "$input_mode request acknowledged"
    else
        status warn input-mode-applied "$input_mode request is not acknowledged by the TiPE engine"
    fi
else
    status skip input-mode-applied "no tokenized mode request yet"
fi
optional_path_status user-dictionary "$dictionary_path" "optional user dictionary"
optional_path_status preferences "$preferences_path" "created after learning"
if [[ -n "$libime_history_path" ]]; then
    optional_path_status libime-user-history "$libime_history_path" "created after a LibIME candidate commit"
else
    status skip libime-user-history "disabled by TIPE_LIBIME_USER_HISTORY"
fi
emit_dictionary_summary
model_config_path="${TIPE_MODEL_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/tipe/model-env}"
optional_path_status model-config "$model_config_path" "heuristic defaults are active"
model_config_mode="heuristic"
model_config_backend="heuristic"
model_config_custom_command=""
model_config_personal_model_path=""
model_config_base_url=""
model_config_name=""
model_config_chat_path=""
model_config_api_key_env=""
model_config_api_key_file=""
model_config_dump_path=""
model_config_continuous_mode="0"
model_config_training_context="0"
model_config_training_surrounding="0"
model_config_send_recent_input="0"
model_config_send_surrounding="0"
model_config_command=""
model_config_timeout=""
model_config_http_timeout=""
model_config_temperature=""
model_config_max_tokens=""
model_config_llama_command=""
model_config_llama_threads=""
model_config_llama_context=""
if [[ -r "$model_config_path" ]]; then
    if grep -Eq 'TIPE_MODEL_API_KEY=' "$model_config_path" &&
        ! grep -Eq 'TIPE_MODEL_API_KEY=.*\$\{[A-Za-z_][A-Za-z0-9_]*:-\}' "$model_config_path"; then
        status warn model-config "may contain a literal API key: $model_config_path"
    else
        status ok model-config "$model_config_path"
    fi
    model_config_mode=$(config_export_value TIPE_MODEL_MODE "$model_config_path" || true)
    model_config_backend=$(config_export_value TIPE_MODEL_BACKEND "$model_config_path" || true)
    model_config_custom_command=$(config_export_value TIPE_MODEL_CUSTOM_COMMAND "$model_config_path" || true)
    model_config_personal_model_path=$(config_export_value TIPE_PERSONAL_MODEL_PATH "$model_config_path" || true)
    model_config_base_url=$(config_export_value TIPE_MODEL_BASE_URL "$model_config_path" || true)
    model_config_name=$(config_export_value TIPE_MODEL_NAME "$model_config_path" || true)
    model_config_chat_path=$(config_export_value TIPE_MODEL_CHAT_PATH "$model_config_path" || true)
    model_config_api_key_env=$(config_api_key_env "$model_config_path" || true)
    model_config_api_key_file=$(config_api_key_file "$model_config_path" || true)
    model_config_dump_path=$(config_export_value TIPE_MODEL_DUMP_PATH "$model_config_path" || true)
    model_config_continuous_mode=$(config_export_value TIPE_CONTINUOUS_MODE "$model_config_path" || true)
    model_config_training_context=$(config_export_value TIPE_PERSONAL_TRAIN_CONTEXT "$model_config_path" || true)
    model_config_training_surrounding=$(config_export_value TIPE_PERSONAL_TRAIN_SURROUNDING "$model_config_path" || true)
    model_config_send_recent_input=$(config_export_value TIPE_MODEL_SEND_RECENT_INPUT "$model_config_path" || true)
    model_config_send_surrounding=$(config_export_value TIPE_MODEL_SEND_SURROUNDING "$model_config_path" || true)
    model_config_command=$(config_export_value TIPE_MODEL_COMMAND "$model_config_path" || true)
    model_config_timeout=$(config_export_value TIPE_MODEL_TIMEOUT_SECONDS "$model_config_path" || true)
    model_config_http_timeout=$(config_export_value TIPE_MODEL_HTTP_TIMEOUT_SECONDS "$model_config_path" || true)
    model_config_temperature=$(config_export_value TIPE_MODEL_TEMPERATURE "$model_config_path" || true)
    model_config_max_tokens=$(config_export_value TIPE_MODEL_MAX_TOKENS "$model_config_path" || true)
    model_config_llama_command=$(config_export_value TIPE_LLAMA_CPP_COMMAND "$model_config_path" || true)
    model_config_llama_threads=$(config_export_value TIPE_LLAMA_CPP_THREADS "$model_config_path" || true)
    model_config_llama_context=$(config_export_value TIPE_LLAMA_CPP_CONTEXT "$model_config_path" || true)
    model_config_mode="${model_config_mode:-heuristic}"
    model_config_backend="${model_config_backend:-heuristic}"
    model_config_continuous_mode="${model_config_continuous_mode:-0}"
    model_config_training_context="${model_config_training_context:-0}"
    model_config_training_surrounding="${model_config_training_surrounding:-0}"
    model_config_send_recent_input="${model_config_send_recent_input:-0}"
    model_config_send_surrounding="${model_config_send_surrounding:-0}"
    if [[ "$model_config_mode" == "custom" ]]; then
        if [[ -z "$model_config_custom_command" ]]; then
            status warn model-custom "custom mode is missing TIPE_MODEL_CUSTOM_COMMAND"
        elif custom_model_executable=$(custom_command_executable "$model_config_custom_command") &&
            [[ -x "$custom_model_executable" ]]; then
            status ok model-custom "$model_config_custom_command"
        else
            status warn model-custom "not executable: $model_config_custom_command"
        fi
    fi
fi
status section model ""
status model configured-mode "$model_config_mode"
status model backend "$model_config_backend"
status model kind "$(model_kind_for "$model_config_mode" "$model_config_backend")"
status model config "$model_config_path"
status model timeout "${model_config_timeout:-2}"
status model http-timeout "${model_config_http_timeout:-8}"
status model temperature "${model_config_temperature:-0}"
status model max-tokens "${model_config_max_tokens:-128}"
personal_model_path="${model_config_personal_model_path:-$data_home/tipe/personal-reranker.json}"
status model personal-model "$personal_model_path"
if [[ -r "$personal_model_path" ]]; then
    if personal_model_helper=$(helper_path tipe-personal-model) &&
        personal_model_inspection=$("$personal_model_helper" inspect --model "$personal_model_path" 2>/dev/null); then
        status model personal-model-status ready
        while IFS=$'\t' read -r personal_model_key personal_model_value _; do
            case "$personal_model_key" in
                name|architecture|feature-version|pinyin-prior-entries|training-pinyin-prior-sources|training-samples|training-ranking-samples|training-chinese-ranking-samples|training-correction-only-samples|training-non-leading-samples|pair-evidence|active-pair-evidence|raw-token-evidence|active-raw-token-evidence|training-raw-token-evidence-entries|training-active-raw-token-evidence|active-correction-patterns|active-key-habits|keyboard-correction-safe|raw-profile-safe|generic-ranking-safe|component-update-safe|evidence-merge-strategy|training-validation-strategy|training-validation-accuracy|training-validation-baseline-accuracy|training-validation-gain|training-validation-non-leading-samples|training-validation-non-leading-correct|training-validation-non-leading-accuracy|training-validation-leading-samples|training-validation-leading-correct|training-validation-generic-non-leading-samples|training-validation-generic-non-leading-correct|training-validation-generic-non-leading-accuracy|training-validation-generic-excluded-direct-evidence|training-validation-generic-excluded-seen-preedit|training-validation-generic-excluded-raw-candidate|training-validation-generic-excluded-derived-prefix|training-raw-profile-samples|training-raw-profile-accepted-samples|training-raw-profile-rejected-samples|training-raw-profile-auxiliary-positive-samples|training-raw-profile-validation-samples|training-raw-profile-validation-correct|training-raw-profile-validation-false-promotions|training-raw-profile-validation-accuracy|training-raw-profile-recommendation|training-recommendation|promotion-margin)
                    status model "personal-model-$personal_model_key" "$personal_model_value"
                    ;;
            esac
        done <<< "$personal_model_inspection"
    else
        status model personal-model-status invalid
    fi
else
    status model personal-model-status untrained
fi
status model configured-command "${model_config_command:-unset}"
if is_model_current_command "${model_config_command:-}"; then
    status model configured-command-valid 1
else
    status model configured-command-valid 0
fi
status model click-trigger "F9"
status model continuous-mode "local-light-rerank"
status model continuous-default "$model_config_continuous_mode"
status model training-context "$model_config_training_context"
status model training-surrounding "$model_config_training_surrounding"
status model send-recent-input "$model_config_send_recent_input"
status model send-surrounding "$model_config_send_surrounding"
status model continuous-toggle "Shift+F9"
status model analyze-window "$HOME/.local/bin/tipe-analyze-window"
status model supervision-window "$HOME/.local/bin/tipe-supervision-window"
status model live-supervision "$supervision_snapshot"
status model analyze-learn "$HOME/.local/bin/tipe-analyze-window --learn-output"
status model training-export "$HOME/.local/bin/tipe-training-export --stats"
status model personal-model-train "$HOME/.local/bin/tipe-personal-model-train"
status model self-test-command "$HOME/.local/bin/tipe-model-self-test --current --config $model_config_path"
status model dry-run-test-command "$HOME/.local/bin/tipe-model-self-test --current --config $model_config_path --adapter-dry-run"
case "$model_config_mode" in
    llama-cpp|ollama|openai|openai-compatible)
        status model dry-run-test-supported 1
        ;;
    *)
        status model dry-run-test-supported 0
        ;;
esac
status model restart-env "TIPE_MODEL_COMMAND=$HOME/.local/bin/tipe-model-current"
status model process-command "${TIPE_MODEL_COMMAND:-unset}"
status model process-command-scope "current-shell-environment"
status model process-command-active-scope "current-shell-only-not-fcitx5-runtime"
status model runtime-verification "runtime section below is authoritative when available"
if is_model_current_command "${TIPE_MODEL_COMMAND:-}"; then
    status model process-command-active 1
else
    status model process-command-active 0
    status model activation-hint "tipe-restart-fcitx5"
fi
if [[ "$runtime" == 0 ]]; then
    status model runtime-check "skipped; run without --no-runtime to inspect fcitx5 process environment when visible"
fi
if [[ -n "$model_config_custom_command" ]]; then
    status model custom-command "$model_config_custom_command"
fi
if [[ -n "$model_config_base_url" ]]; then
    status model base-url "$model_config_base_url"
fi
if [[ -n "$model_config_name" ]]; then
    status model model "$model_config_name"
    status model name "$model_config_name"
fi
if [[ "$model_config_mode" == "llama-cpp" ]]; then
    llama_command="${model_config_llama_command:-${TIPE_LLAMA_CPP_COMMAND:-/usr/bin/llama-cli}}"
    status model invocation "on-demand-single-process"
    status model llama-command "$llama_command"
    [[ -x "$llama_command" ]] && status model llama-command-valid 1 || status model llama-command-valid 0
    [[ -r "$model_config_name" ]] && status model llama-model-readable 1 || status model llama-model-readable 0
    status model llama-threads "${model_config_llama_threads:-${TIPE_LLAMA_CPP_THREADS:-6}}"
    status model llama-context "${model_config_llama_context:-${TIPE_LLAMA_CPP_CONTEXT:-8192}}"
fi
if [[ -n "$model_config_chat_path" ]]; then
    status model chat-path "$model_config_chat_path"
fi
if [[ -n "$model_config_api_key_file" ]]; then
    status model api-key-file "$model_config_api_key_file"
    status model api-key-source stored-file
    status model api-key-runtime "$(api_key_file_runtime_status "$model_config_api_key_file")"
elif [[ -n "$model_config_api_key_env" ]]; then
    status model api-key-env "$model_config_api_key_env"
    status model api-key-source environment
    if [[ -n "${!model_config_api_key_env:-}" ]]; then
        status model api-key-runtime set
    else
        status model api-key-runtime unset
    fi
fi
if [[ -n "$model_config_dump_path" ]]; then
    status model dump-path "$model_config_dump_path"
fi
if [[ -e "$dictionary_path" ]]; then
    if dictionary_checker=$(helper_path tipe-check-user-dictionary); [[ -n "${dictionary_checker:-}" ]]; then
        if "$dictionary_checker" "$dictionary_path" >/dev/null; then
            status ok user-dictionary "$dictionary_path"
        else
            status warn user-dictionary "validation failed: $dictionary_path"
        fi
    else
        status warn user-dictionary "checker not found"
    fi
else
    status skip user-dictionary "not found; validation skipped"
fi
if [[ -e "$preferences_path" ]]; then
    if preferences_checker=$(helper_path tipe-check-preferences); [[ -n "${preferences_checker:-}" ]]; then
        if "$preferences_checker" "$preferences_path" >/dev/null; then
            status ok preferences "$preferences_path"
            emit_learning_summary "$preferences_checker" "$preferences_path"
        else
            status warn preferences "validation failed: $preferences_path"
        fi
    else
        status warn preferences "checker not found"
    fi
else
    status skip preferences "not found; validation skipped"
fi

status section logs ""
optional_path_status direct-fcitx5-log "$fcitx_log" "created only by the direct fallback launcher"
optional_path_status tipeui-log "$tipeui_log" "created only by an explicit TiPE debug run"
optional_path_status candidate-window-log "$candidate_window_log" "created after GTK fallback candidate/status windows"
optional_path_status engine-trace "$engine_trace_log" "created after TiPE engine activity"
optional_path_status slow-key-events "$slow_key_events_log" "created only when one key event takes at least 50 ms"
optional_path_status live-supervision "$supervision_snapshot" "no active TiPE composition"
optional_path_status supervision-history "$supervision_history" "created after supervised TiPE input"
optional_path_status supervision-training-history "$supervision_training_history" "created after terminal TiPE choices"
emit_diagnostic_log_summary tipeui-log "$tipeui_log"
emit_diagnostic_log_summary engine-trace "$engine_trace_log"
emit_diagnostic_log_summary candidate-window-log "$candidate_window_log"
emit_diagnostic_log_summary slow-key-events "$slow_key_events_log"
if [[ -r "$slow_key_events_log" ]]; then
    slow_key_event_count=$(grep -c '^slow-key-event' "$slow_key_events_log" || true)
    if (( slow_key_event_count > 0 )); then
        status warn slow-key-events "$slow_key_event_count slow key event(s) recorded"
        latest_slow_key_event=$(tail -n 1 "$slow_key_events_log")
        status info slow-key-events "$(one_line "$latest_slow_key_event")"
    else
        status ok slow-key-events "no slow key event recorded"
    fi
fi
emit_supervision_history_summary "$supervision_history" supervision-history 262144
emit_supervision_history_summary "$supervision_training_history" supervision-training-history 1048576
if [[ -e "$fcitx_log" && "$fcitx_service_active" == "0" ]]; then
    if grep -Eq 'Loaded addon tipe([[:space:]]|$)' "$fcitx_log"; then
        status ok direct-fcitx5-log "Loaded addon tipe found"
    else
        status warn direct-fcitx5-log "Loaded addon tipe not found"
    fi
    if grep -q "Loaded addon tipeui" "$fcitx_log"; then
        status ok direct-fcitx5-log "Loaded addon tipeui found"
    else
        status warn direct-fcitx5-log "Loaded addon tipeui not found"
    fi
elif [[ -e "$fcitx_log" ]]; then
    status skip direct-fcitx5-log "fcitx5.service is active; this file may be stale and is not used for runtime addon checks"
fi
if [[ -e "$tipeui_log" ]]; then
    fallback_pointer_ready=0
    if grep -q $'popup\tcandidate-frontend-fallback-start\t.*events=ready' "$tipeui_log"; then
        fallback_pointer_ready=1
    fi
    if latest_wayland_created=$(grep -n $'wayland\tcreated\t' "$tipeui_log" | tail -n 1); [[ -n "$latest_wayland_created" ]]; then
        latest_wayland_created_line=${latest_wayland_created%%:*}
        latest_pointer_ready=$(grep -n $'wayland\tpointer-ready\t' "$tipeui_log" | tail -n 1 || true)
        latest_pointer_ready_line=${latest_pointer_ready%%:*}
        if { [[ "$latest_pointer_ready_line" =~ ^[0-9]+$ ]] &&
              (( latest_pointer_ready_line >= latest_wayland_created_line )); } ||
            (( fallback_pointer_ready )); then
            status ok tipeui-log "candidate pointer input is available"
        else
            status warn tipeui-log "candidate pointer input is unavailable; mouse selection cannot work"
        fi
    elif (( fallback_pointer_ready )); then
        status ok tipeui-log "candidate pointer input is available through GTK fallback"
    else
        status skip tipeui-log "candidate pointer channel not observed yet"
    fi
    if grep -q $'popup\tcandidate-click\t' "$tipeui_log"; then
        status ok tipeui-log "candidate mouse selection observed"
    else
        status skip tipeui-log "candidate mouse selection not observed yet"
    fi
    if grep -q $'popup\tstatus-rendered' "$tipeui_log"; then
        status ok tipeui-log "status popup rendered"
        if grep -q $'popup\tstatus-render-with-candidate-stale-rect' "$tipeui_log"; then
            latest_status_stale_candidate=$(grep $'popup\tstatus-render-with-candidate-stale-rect' "$tipeui_log" | tail -n 1)
            status info tipeui-log "$latest_status_stale_candidate"
        fi
    elif grep -q $'popup\tstatus-frontend-fallback' "$tipeui_log"; then
        latest_status_frontend_fallback=$(grep $'popup\tstatus-frontend-fallback' "$tipeui_log" | tail -n 1)
        status ok tipeui-log "status GTK frontend fallback observed"
        status info tipeui-log "$latest_status_frontend_fallback"
    elif grep -q $'popup\tstatus-edge-fallback' "$tipeui_log"; then
        latest_status_edge_fallback=$(grep $'popup\tstatus-edge-fallback' "$tipeui_log" | tail -n 1)
        status ok tipeui-log "status popup edge fallback observed"
        status info tipeui-log "$latest_status_edge_fallback"
    elif grep -q $'popup\tpending-status' "$tipeui_log" || grep -q $'popup\tdefer-status-render' "$tipeui_log"; then
        latest_pending_status=$(grep -E $'popup\t(pending-status|defer-status-render)' "$tipeui_log" | tail -n 1)
        status warn tipeui-log "status popup is waiting for a fresh Wayland text rect"
        status info tipeui-log "$latest_pending_status"
    else
        status warn tipeui-log "no status popup render row"
    fi
    if grep -q $'popup\trendered' "$tipeui_log"; then
        status ok tipeui-log "candidate popup rendered"
        latest_rendered=$(grep $'popup\trendered' "$tipeui_log" | tail -n 1)
        if [[ "$latest_rendered" == *$'\tboundsOk=0'* ]]; then
            status warn tipeui-log "candidate popup draw bounds exceeded panel padding"
        elif [[ "$latest_rendered" == *$'\tboundsOk=1'* ]]; then
            status ok tipeui-log "candidate popup draw bounds ok"
        else
            status skip tipeui-log "candidate popup draw bounds not reported yet"
        fi
    elif grep -q $'popup\tcandidate-frontend-fallback$' "$tipeui_log"; then
        status ok tipeui-log "candidate GTK frontend fallback rendered"
    else
        status warn tipeui-log "no candidate popup render row yet"
    fi
    if grep -q $'popup\thidden\ttext-rect-stale=1' "$tipeui_log"; then
        status ok tipeui-log "popup hide invalidated stale text rect"
    else
        status skip tipeui-log "popup hide stale-text-rect row not observed yet"
    fi
    if grep -q $'popup\tfrontend-fallback-anchor\t' "$tipeui_log"; then
        latest_frontend_anchor=$(grep $'popup\tfrontend-fallback-anchor\t' "$tipeui_log" | tail -n 1)
        status info tipeui-log "$latest_frontend_anchor"
    fi
fi
if [[ -e "$candidate_window_log" ]]; then
    if grep -q $'fallback\tposition\tmode=candidate\t' "$candidate_window_log"; then
        latest_fallback_position=$(grep $'fallback\tposition\tmode=candidate\t' "$candidate_window_log" | tail -n 1)
        if [[ "$latest_fallback_position" == *$'\tboundsOk=0'* ]]; then
            status warn candidate-window-log "GTK fallback window position exceeded monitor bounds"
            status info candidate-window-log "$latest_fallback_position"
        elif [[ "$latest_fallback_position" == *$'\tboundsOk=1'* ]]; then
            status ok candidate-window-log "GTK fallback window position inside monitor"
            status info candidate-window-log "$latest_fallback_position"
        else
            status skip candidate-window-log "GTK fallback position bounds not reported yet"
        fi
    else
        status skip candidate-window-log "no GTK candidate fallback position row yet"
    fi
    if grep -Eq $'fallback\tposition\tmode=status(-fixed)?\t' "$candidate_window_log"; then
        latest_status_fallback_position=$(grep -E $'fallback\tposition\tmode=status(-fixed)?\t' "$candidate_window_log" | tail -n 1)
        if [[ "$latest_status_fallback_position" == *$'\tcursor=0,0,0,0\t'* ]]; then
            if [[ -e "$engine_trace_log" ]] && grep -q "status-panel-show" "$engine_trace_log"; then
                status info candidate-window-log "legacy GTK status fallback used invalid cursor rect before panel status path"
            else
                status warn candidate-window-log "GTK status fallback used invalid cursor rect"
            fi
        elif [[ "$latest_status_fallback_position" == *$'\tboundsOk=0'* ]]; then
            status warn candidate-window-log "GTK status fallback position exceeded monitor bounds"
        elif [[ "$latest_status_fallback_position" == *$'\tboundsOk=1'* ]]; then
            status ok candidate-window-log "GTK status fallback position inside monitor"
        else
            status info candidate-window-log "GTK status fallback position observed"
        fi
        status info candidate-window-log "$latest_status_fallback_position"
    fi
fi
if [[ -e "$engine_trace_log" ]]; then
    if grep -q "wayland-popup-edge-fallback" "$engine_trace_log"; then
        if [[ "${TIPE_WAYLAND_POPUP_EDGE_FALLBACK:-0}" =~ ^(1|true|on)$ ]]; then
            status warn engine-trace "diagnostic candidate edge fallback is enabled"
        else
            status info engine-trace "historical candidate edge fallback observed; disabled by default"
        fi
    else
        status ok engine-trace "no candidate edge fallback observed"
    fi
    if grep -q "status-panel-show" "$engine_trace_log"; then
        latest_status_panel=$(grep "status-panel-show" "$engine_trace_log" | tail -n 1)
        status ok engine-trace "TiPE input-mode status activation observed"
        status info engine-trace "$latest_status_panel"
        if grep -q "status-window-fallback" "$engine_trace_log"; then
            latest_status_window_fallback=$(grep "status-window-fallback" "$engine_trace_log" | tail -n 1)
            status ok engine-trace "status GTK fallback shown for non-wayland frontend"
            status info engine-trace "$latest_status_window_fallback"
        fi
    elif grep -q "status-window-edge-fallback rect=0,0,0,0" "$engine_trace_log"; then
        latest_invalid_status_fallback=$(grep "status-window-edge-fallback rect=0,0,0,0" "$engine_trace_log" | tail -n 1)
        status warn engine-trace "status popup used invalid cursor rect"
        status info engine-trace "$latest_invalid_status_fallback"
    elif grep -q "status-window-skip-invalid-snapshot" "$engine_trace_log"; then
        latest_status_skip=$(grep "status-window-skip-invalid-snapshot" "$engine_trace_log" | tail -n 1)
        status info engine-trace "status fallback waited for a usable cursor rectangle"
        status info engine-trace "$latest_status_skip"
    else
        status skip engine-trace "status popup not observed yet"
    fi
    if grep -q "preserve-state reason=" "$engine_trace_log"; then
        latest_preserve_state=$(grep "preserve-state reason=" "$engine_trace_log" | tail -n 1)
        status ok engine-trace "preedit preserve observed"
        status info engine-trace "$latest_preserve_state"
    else
        status skip engine-trace "preedit preserve not observed yet"
    fi
    if grep -q "restore-state preedit=" "$engine_trace_log"; then
        latest_restore_state=$(grep "restore-state preedit=" "$engine_trace_log" | tail -n 1)
        status ok engine-trace "preedit restore observed"
        status info engine-trace "$latest_restore_state"
    elif grep -q "restore-state rejected" "$engine_trace_log"; then
        latest_restore_rejected=$(grep "restore-state rejected" "$engine_trace_log" | tail -n 1)
        status warn engine-trace "preedit restore was rejected"
        status info engine-trace "$latest_restore_rejected"
    else
        status skip engine-trace "preedit restore not observed yet"
    fi
    if grep -q "restore-state allowing-program-change" "$engine_trace_log"; then
        latest_program_fallback=$(grep "restore-state allowing-program-change" "$engine_trace_log" | tail -n 1)
        status ok engine-trace "preedit restore allowed changed program metadata"
        status info engine-trace "$latest_program_fallback"
    fi
    if grep -q "restore-state allowing-same-input-context" "$engine_trace_log"; then
        latest_context_fallback=$(grep "restore-state allowing-same-input-context" "$engine_trace_log" | tail -n 1)
        status ok engine-trace "preedit restore allowed same input context"
        status info engine-trace "$latest_context_fallback"
    fi
    if grep -q "model-async-start" "$engine_trace_log"; then
        latest_async_model_start=$(grep "model-async-start" "$engine_trace_log" | tail -n 1)
        status ok engine-trace "asynchronous model request observed"
        status info engine-trace "$latest_async_model_start"
        if latest_async_model_result=$(grep -E "model-async-(finish|discard)" "$engine_trace_log" | tail -n 1); [[ -n "$latest_async_model_result" ]]; then
            status info engine-trace "$latest_async_model_result"
        fi
    else
        status skip engine-trace "asynchronous model request not observed yet"
    fi
    if grep -q "continuous-mode toggled=" "$engine_trace_log"; then
        latest_continuous_toggle=$(grep "continuous-mode toggled=" "$engine_trace_log" | tail -n 1)
        status ok engine-trace "continuous mode toggle observed"
        status info engine-trace "$latest_continuous_toggle"
    else
        status skip engine-trace "continuous mode toggle not observed yet"
    fi
fi
if [[ -e "$tipeui_log" ]]; then
    if grep -q $'popup\tcandidate-edge-fallback' "$tipeui_log"; then
        if [[ "${TIPE_WAYLAND_POPUP_EDGE_FALLBACK:-0}" =~ ^(1|true|on)$ ]]; then
            status warn tipeui-log "diagnostic candidate edge fallback is enabled; its text rectangle is surface-local and cannot reliably place a global layer-shell window"
        else
            status info tipeui-log "historical candidate edge fallback observed; normal Wayland input now stays on the compositor popup path"
        fi
    else
        status ok tipeui-log "no candidate edge fallback observed"
    fi
    if grep -q $'popup\tcandidate-edge-fallback-terminate' "$tipeui_log"; then
        status ok tipeui-log "candidate fallback termination observed"
    else
        status skip tipeui-log "candidate fallback termination not observed yet"
    fi
fi

if [[ "$runtime" == 1 ]]; then
    status section runtime ""
    if [[ "$fcitx_service_active" == "1" ]]; then
        status runtime fcitx5-log-source "user-journal"
        if command -v journalctl >/dev/null 2>&1; then
            fcitx_service_journal=$(journalctl --user -u fcitx5.service -n 400 --no-pager --output=cat 2>/dev/null || true)
            if [[ -n "$fcitx_service_journal" ]]; then
                if grep -Eq 'Loaded addon tipe([[:space:]]|$)' <<<"$fcitx_service_journal"; then
                    status ok fcitx5-journal "Loaded addon tipe found"
                else
                    status warn fcitx5-journal "Loaded addon tipe not found in the latest 400 journal rows"
                fi
                if grep -q "Loaded addon tipeui" <<<"$fcitx_service_journal"; then
                    status ok fcitx5-journal "Loaded addon tipeui found"
                else
                    status warn fcitx5-journal "Loaded addon tipeui not found in the latest 400 journal rows"
                fi
            else
                status skip fcitx5-journal "user service is active but its journal is unavailable"
            fi
        else
            status skip fcitx5-journal "journalctl unavailable"
        fi
    else
        status runtime fcitx5-log-source "direct-fallback-file:$fcitx_log"
    fi
    if command -v fcitx5-remote >/dev/null 2>&1; then
        if [[ "$runtime_current_checked" == 1 && -n "$runtime_current_method" ]]; then
            current_im="$runtime_current_method"
            status runtime current-input-method "$current_im"
            status runtime current-input-method-source "early-startup-snapshot"
            if [[ "$current_im" == "tipe" ]]; then
                status ok runtime "TiPE is the current input method"
            else
                status warn runtime "current input method is $current_im, not tipe"
            fi
        else
            status skip current-input-method "fcitx5-remote -n failed: $(one_line "${runtime_current_error:-unknown error}")"
        fi
    else
        status skip current-input-method "fcitx5-remote unavailable"
    fi
    if pids=$(find_process_pids fcitx5) && [[ -n "$pids" ]]; then
        while IFS= read -r pid; do
            [[ -z "$pid" ]] && continue
            if process=$(ps -o pid=,comm=,args= -p "$pid" 2>/dev/null); then
                status runtime process "$process"
            else
                status runtime process "$pid fcitx5"
            fi
            runtime_model_command=$(process_env_value "$pid" TIPE_MODEL_COMMAND || true)
            runtime_model_config=$(process_env_value "$pid" TIPE_MODEL_CONFIG || true)
            runtime_model_mode=$(process_env_value "$pid" TIPE_MODEL_MODE || true)
            runtime_model_backend=$(process_env_value "$pid" TIPE_MODEL_BACKEND || true)
            runtime_continuous_mode=$(process_env_value "$pid" TIPE_CONTINUOUS_MODE || true)
            runtime_debug=$(process_env_value "$pid" TIPE_DEBUG || true)
            runtime_candidate_debug=$(process_env_value "$pid" TIPE_CANDIDATE_DEBUG || true)
            status runtime model-command "${pid}:${runtime_model_command:-unset}"
            status runtime model-config "${pid}:${runtime_model_config:-default}"
            status runtime model-mode "${pid}:${runtime_model_mode:-default}"
            status runtime model-backend "${pid}:${runtime_model_backend:-default}"
            status runtime continuous-mode "${pid}:${runtime_continuous_mode:-default}"
            if [[ "$runtime_debug" == "1" || "$runtime_candidate_debug" == "1" ]]; then
                status runtime diagnostic-logging "${pid}:enabled"
            else
                status runtime diagnostic-logging "${pid}:disabled"
            fi
            if is_model_current_command "$runtime_model_command"; then
                status runtime model-config-active "${pid}:1"
                runtime_effective_model_config="${runtime_model_config:-${XDG_CONFIG_HOME:-$HOME/.config}/tipe/model-env}"
                if [[ "$runtime_effective_model_config" == "$model_config_path" ]]; then
                    status runtime model-config-path-active "${pid}:1"
                else
                    status runtime model-config-path-active "${pid}:0:$runtime_effective_model_config"
                fi
            else
                status runtime model-config-active "${pid}:0"
                status runtime model-config-path-active "${pid}:0"
            fi
        done <<< "$pids"
    else
        status skip process "fcitx5 process not visible from this environment"
    fi
    if pids=$(find_process_pids tipe-candidate-window) && [[ -n "$pids" ]]; then
        while IFS= read -r pid; do
            [[ -z "$pid" ]] && continue
            if process=$(ps -o pid=,comm=,args= -p "$pid" 2>/dev/null); then
                status warn legacy-window "$process"
            else
                status warn legacy-window "$pid tipe-candidate-window"
            fi
        done <<< "$pids"
    else
        status ok legacy-window "not running"
    fi
fi
