#!/usr/bin/env bash
set -euo pipefail

request_path=""
model_command=""
config_path=""
check_output=0
explain_request=0
dry_run_model=0
explain_output=0
learn_output=0
preferences_path=""
cleanup_paths=()

cleanup() {
    if [[ "${#cleanup_paths[@]}" -gt 0 ]]; then
        rm -f "${cleanup_paths[@]}"
    fi
}
trap cleanup EXIT

usage() {
    cat <<'EOF'
Usage:
  tipe-model-replay [--request REQUEST_TSV] [--command COMMAND] [--config PATH] [--check] [--explain] [--dry-run-model|--dry-run] [--explain-output] [--learn-output] [--preferences PATH]

Replays a live or dumped TiPE model request through the configured model helper
or a specific wrapper command. By default it reads the live supervision snapshot
when available, otherwise the latest request from supervision-history.tsv when
available, otherwise the same request path used by tipe-model-dump. It prints
the model output on stdout.

Options:
  --request PATH   request TSV to replay
  --command CMD    model command to run, default tipe-model-current
  --config PATH    TIPE_MODEL_CONFIG path for tipe-model-current
  --check          validate the wrapper output with tipe-model-wrapper-check
  --explain        print a compact request summary to stderr before replaying
  --dry-run-model  set TIPE_MODEL_DRY_RUN=1 and validate adapter request-json output
  --dry-run        alias for --dry-run-model
  --explain-output print accepted model learning/ranking summary rows to stderr
  --learn-output   persist accepted candidate/correction/preference/segment-chain rows into TiPE preferences
  --preferences PATH preference TSV to update with --learn-output
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
        --request)
            require_value "$1" "${2:-}"
            request_path="$2"
            shift
            ;;
        --command)
            require_value "$1" "${2:-}"
            model_command="$2"
            shift
            ;;
        --config)
            require_value "$1" "${2:-}"
            config_path="$2"
            shift
            ;;
        --check)
            check_output=1
            ;;
        --explain)
            explain_request=1
            ;;
        --dry-run-model|--dry-run)
            dry_run_model=1
            ;;
        --explain-output)
            explain_output=1
            check_output=1
            ;;
        --learn-output)
            learn_output=1
            check_output=1
            ;;
        --preferences)
            require_value "$1" "${2:-}"
            preferences_path="$2"
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
    shift
done

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

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
    elif [[ -x "${HOME:-}/.local/bin/$helper_name" ]]; then
        printf '%s\n' "$HOME/.local/bin/$helper_name"
    else
        return 1
    fi
}

default_request_path() {
    local live_request="" last_request=""
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        live_request="$XDG_CACHE_HOME/tipe/supervision-current.tsv"
        last_request="$XDG_CACHE_HOME/tipe/supervision-last.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        live_request="$HOME/.cache/tipe/supervision-current.tsv"
        last_request="$HOME/.cache/tipe/supervision-last.tsv"
    fi
    if [[ -n "$live_request" && -r "$live_request" ]]; then
        printf '%s\n' "$live_request"
    elif [[ -n "$last_request" && -r "$last_request" ]]; then
        printf '%s\n' "$last_request"
    elif latest_history_request_path=$(materialize_latest_history_request); then
        printf '%s\n' "$latest_history_request_path"
    elif [[ -n "${TIPE_MODEL_DUMP_PATH:-}" ]]; then
        printf '%s\n' "$TIPE_MODEL_DUMP_PATH"
    elif [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        printf '%s\n' "$XDG_CACHE_HOME/tipe/model-request.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.cache/tipe/model-request.tsv"
    else
        printf '%s\n' "/tmp/tipe/model-request.tsv"
    fi
}

default_history_path() {
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        printf '%s\n' "$XDG_CACHE_HOME/tipe/supervision-history.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.cache/tipe/supervision-history.tsv"
    else
        printf '%s\n' "/tmp/tipe/supervision-history.tsv"
    fi
}

materialize_latest_history_request() {
    local history_path output_path
    history_path=$(default_history_path)
    [[ -r "$history_path" ]] || return 1
    output_path=$(mktemp)
    cleanup_paths+=("$output_path")
    awk '
        BEGIN {
            record = ""
            current = ""
        }
        /^---\t/ {
            if (current ~ /(^|\n)protocol\t1(\n|$)/ && current ~ /(^|\n)preedit\t/) {
                record = current
            }
            current = ""
            next
        }
        {
            current = current $0 "\n"
        }
        END {
            if (current ~ /(^|\n)protocol\t1(\n|$)/ && current ~ /(^|\n)preedit\t/) {
                record = current
            }
            if (record != "") {
                printf "%s", record
            }
        }
    ' "$history_path" >"$output_path"
    if [[ -s "$output_path" ]]; then
        printf '%s\n' "$output_path"
        return 0
    fi
    rm -f "$output_path"
    return 1
}

default_preferences_path() {
    if [[ -n "${XDG_DATA_HOME:-}" ]]; then
        printf '%s\n' "$XDG_DATA_HOME/tipe/candidate-preferences.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.local/share/tipe/candidate-preferences.tsv"
    else
        printf '%s\n' "/tmp/tipe/candidate-preferences.tsv"
    fi
}

if [[ -z "$request_path" ]]; then
    request_path=$(default_request_path)
fi
if [[ "$request_path" == "-" ]]; then
    tmp_request=$(mktemp)
    cleanup_paths+=("$tmp_request")
    cat >"$tmp_request"
    request_path="$tmp_request"
fi
if [[ ! -r "$request_path" ]]; then
    echo "cannot read request: $request_path" >&2
    exit 1
fi
if [[ -z "$model_command" ]]; then
    model_command=$(helper_path tipe-model-current) || {
        echo "tipe-model-current helper is not available" >&2
        exit 1
    }
fi
if [[ -n "$config_path" && ! -r "$config_path" ]]; then
    echo "cannot read config: $config_path" >&2
    exit 1
fi
if [[ "$learn_output" == "1" && -z "$preferences_path" ]]; then
    preferences_path=$(default_preferences_path)
fi

model_tokens=()
model_env=()
parse_model_command() {
    if [[ -z "$model_command" || ! "$model_command" =~ ^[A-Za-z0-9_./:=+\ -]+$ ]]; then
        echo "model command may only contain safe path, word, assignment, and space characters" >&2
        exit 2
    fi
    read -r -a model_tokens <<< "$model_command"
    while [[ "${#model_tokens[@]}" -gt 0 && "${model_tokens[0]}" =~ ^[A-Za-z_][A-Za-z0-9_]*=.+$ ]]; do
        model_env+=("${model_tokens[0]}")
        model_tokens=("${model_tokens[@]:1}")
    done
    if [[ "${#model_tokens[@]}" -eq 0 ]]; then
        echo "model command is missing an executable" >&2
        exit 2
    fi
    if [[ ! -x "${model_tokens[0]}" ]]; then
        echo "model command is not executable: ${model_tokens[0]}" >&2
        exit 1
    fi
}

run_model_command() {
    if [[ "$dry_run_model" == "1" && -n "$config_path" ]]; then
        TIPE_MODEL_DRY_RUN=1 TIPE_MODEL_CONFIG="$config_path" env "${model_env[@]}" "${model_tokens[@]}"
    elif [[ "$dry_run_model" == "1" ]]; then
        TIPE_MODEL_DRY_RUN=1 env "${model_env[@]}" "${model_tokens[@]}"
    elif [[ -n "$config_path" ]]; then
        TIPE_MODEL_CONFIG="$config_path" env "${model_env[@]}" "${model_tokens[@]}"
    else
        env "${model_env[@]}" "${model_tokens[@]}"
    fi
}

parse_model_command

validate_adapter_dry_run() {
    local output_path="$1"
    local request_url request_json
    request_url=$(sed -n 's/^request\t//p' "$output_path" | sed -n '1p')
    request_json=$(sed -n 's/^request-json\t//p' "$output_path" | sed -n '1p')
    if [[ -z "$request_url" || -z "$request_json" ]]; then
        echo "adapter dry-run output must include request and request-json rows" >&2
        if [[ -n "$config_path" ]]; then
            echo "hint: check $config_path; --dry-run-model only validates HTTP adapter modes such as ollama, openai, or openai-compatible" >&2
        else
            echo "hint: --dry-run-model only validates HTTP adapter modes such as ollama, openai, or openai-compatible" >&2
        fi
        return 1
    fi
    command -v python3 >/dev/null 2>&1 || {
        echo "python3 is required to validate adapter dry-run JSON" >&2
        return 1
    }
    TIPE_REPLAY_JSON="$request_json" TIPE_REPLAY_REQUEST="$request_path" python3 - <<'PY'
import json
import os
import pathlib
import sys

try:
    payload = json.loads(os.environ["TIPE_REPLAY_JSON"])
except Exception as exc:
    print(f"invalid request-json: {exc}", file=sys.stderr)
    sys.exit(1)

messages = payload.get("messages")
if not isinstance(messages, list) or len(messages) < 2:
    print("request-json should contain chat messages", file=sys.stderr)
    sys.exit(1)
try:
    prompt = json.loads(messages[-1].get("content", ""))
except Exception as exc:
    print(f"invalid prompt JSON in final message: {exc}", file=sys.stderr)
    sys.exit(1)
required = {
    "preedit",
    "candidates",
    "candidate_metadata",
    "behavior_summary",
    "runtime_state",
    "recent_events",
    "correction_events",
    "pending_segments",
    "recent_segment_chains",
    "known_preferences",
    "known_corrections",
}
missing = sorted(required.difference(prompt))
if missing:
    print("prompt missing fields: " + ", ".join(missing), file=sys.stderr)
    sys.exit(1)
request_preedit = None
declared_mode = None
request_path = pathlib.Path(os.environ["TIPE_REPLAY_REQUEST"])
for raw_line in request_path.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
    line = raw_line.removesuffix("\r")
    fields = line.split("\t")
    if fields and fields[0] == "preedit":
        request_preedit = fields[1] if len(fields) > 1 else ""
    elif fields and fields[0] == "supervision_state":
        for index in range(1, len(fields) - 1, 2):
            if fields[index] == "mode" and fields[index + 1] in {"active-preedit", "pass-through-only"}:
                declared_mode = fields[index + 1]
if request_preedit is None:
    print("request TSV should contain a preedit row", file=sys.stderr)
    sys.exit(1)
expected_mode = "active-preedit" if request_preedit else "pass-through-only"
if declared_mode is not None:
    expected_mode = declared_mode
if prompt.get("preedit") != request_preedit:
    print("prompt preedit does not match replay request", file=sys.stderr)
    sys.exit(1)
if prompt.get("supervision_mode") != expected_mode:
    print(f"unexpected supervision_mode: {prompt.get('supervision_mode')!r}", file=sys.stderr)
    sys.exit(1)
if not isinstance(prompt.get("candidate_metadata"), list):
    print("candidate_metadata should be a list", file=sys.stderr)
    sys.exit(1)
behavior = prompt.get("behavior_summary")
if not isinstance(behavior, dict):
    print("behavior_summary should be an object", file=sys.stderr)
    sys.exit(1)
behavior_required = {
    "recent_event_counts",
    "correction_event_counts",
    "preedit_leading_context",
    "possible_corrections",
    "edit_summary",
    "correction_patterns",
    "realtime_correction_decisions",
    "raw_english_hint",
    "learning_signals",
    "supervised_learning_signals",
}
missing_behavior = sorted(behavior_required.difference(behavior))
if missing_behavior:
    print("behavior_summary missing fields: " + ", ".join(missing_behavior), file=sys.stderr)
    sys.exit(1)
if not isinstance(behavior.get("preedit_leading_context"), dict):
    print("behavior_summary.preedit_leading_context should be an object", file=sys.stderr)
    sys.exit(1)
if not isinstance(behavior.get("supervised_learning_signals"), list):
    print("behavior_summary.supervised_learning_signals should be a list", file=sys.stderr)
    sys.exit(1)
PY
    printf 'model-dry-run-request\t%s\n' "$request_url" >&2
    printf 'model-dry-run-ok\trequest-json\t%s\n' "$request_path" >&2
}

explain_model_output() {
    local output_path="$1"
    local accepted_rows=0
    local candidate_rows=0
    local correction_rows=0
    local preference_rows=0
    local segment_chain_rows=0
    local request_preedit="" selected_candidate="" selected_index="" selected_text=""
    local line kind first second extra
    local -a fields
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        IFS=$'\t' read -r kind first second extra <<< "$line"
        case "$kind" in
            preedit)
                request_preedit="${first:-}"
                ;;
            selected_candidate)
                selected_candidate="${line#selected_candidate	}"
                selected_index="${first:-}"
                selected_text="${second:-}"
                ;;
        esac
    done <"$request_path"
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        [[ -z "$line" ]] && continue
        IFS=$'\t' read -r -a fields <<< "$line"
        IFS=$'\t' read -r kind first second extra <<< "$line"
        case "$kind" in
            candidate)
                candidate_rows=$((candidate_rows + 1))
                accepted_rows=$((accepted_rows + 1))
                printf 'model-output-accepted\tcandidate\t%s\t%s\n' "$candidate_rows" "$first" >&2
                ;;
            correction)
                correction_rows=$((correction_rows + 1))
                accepted_rows=$((accepted_rows + 1))
                printf 'model-output-accepted\tcorrection\t%s\t%s\t%s\n' "$correction_rows" "$first" "$second" >&2
                ;;
            preference)
                preference_rows=$((preference_rows + 1))
                accepted_rows=$((accepted_rows + 1))
                printf 'model-output-accepted\tpreference\t%s\t%s\t%s\n' "$preference_rows" "${fields[1]:-}" "${fields[2]:-}" >&2
                ;;
            segment_chain)
                segment_chain_rows=$((segment_chain_rows + 1))
                accepted_rows=$((accepted_rows + 1))
                printf 'model-output-accepted\tsegment_chain\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "$segment_chain_rows" "${fields[1]:-}" "${fields[2]:-}" "${fields[3]:-}" \
                    "${fields[4]:-}" "${fields[5]:-}" "${fields[6]:-}" >&2
                ;;
            *)
                candidate_rows=$((candidate_rows + 1))
                accepted_rows=$((accepted_rows + 1))
                printf 'model-output-accepted\tcandidate\t%s\t%s\n' "$candidate_rows" "$kind" >&2
                ;;
        esac
    done <"$output_path"
    printf 'model-output-summary\trows\t%s\tcandidates\t%s\tcorrections\t%s\tpreferences\t%s\tsegment-chains\t%s\n' \
        "$accepted_rows" "$candidate_rows" "$correction_rows" "$preference_rows" "$segment_chain_rows" >&2
    if [[ "$accepted_rows" == "0" ]]; then
        if [[ -n "$selected_candidate" ]]; then
            printf 'model-output-note\tselected-candidate-already-top\t%s\t%s\t%s\n' \
                "${request_preedit:-}" "${selected_index:-}" "${selected_text:-}" >&2
        else
            printf 'model-output-note\tno-safe-learning-signal\t%s\n' "${request_preedit:-}" >&2
        fi
    elif [[ "$preference_rows" -gt 0 && -n "$selected_candidate" ]]; then
        printf 'model-output-note\tselected-candidate-learned\t%s\t%s\t%s\n' \
            "${request_preedit:-}" "${selected_index:-}" "${selected_text:-}" >&2
    fi
}

explain_rejected_model_output() {
    local output_path="$1"
    local status="$2"
    local line kind first second third fourth row_count=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        [[ -z "$line" ]] && continue
        row_count=$((row_count + 1))
        IFS=$'\t' read -r kind first second third fourth <<< "$line"
        case "$kind" in
            candidate)
                printf 'model-output-rejected-row\t%s\tcandidate\t%s\n' "$row_count" "${first:-}" >&2
                ;;
            correction)
                printf 'model-output-rejected-row\t%s\tcorrection\t%s\t%s\n' "$row_count" "${first:-}" "${second:-}" >&2
                ;;
            preference)
                printf 'model-output-rejected-row\t%s\tpreference\t%s\t%s\t%s\n' \
                    "$row_count" "${first:-}" "${second:-}" "${third:-}" >&2
                ;;
            segment_chain)
                printf 'model-output-rejected-row\t%s\tsegment_chain\t%s\t%s\t%s\t%s\n' \
                    "$row_count" "${first:-}" "${second:-}" "${third:-}" "${fourth:-}" >&2
                ;;
            *)
                printf 'model-output-rejected-row\t%s\t%s\n' "$row_count" "$line" >&2
                ;;
        esac
    done <"$output_path"
    printf 'model-output-rejected-summary\trows\t%s\tstatus\t%s\n' "$row_count" "$status" >&2
}

learn_model_output() {
    local output_path="$1"
    command -v python3 >/dev/null 2>&1 || {
        echo "python3 is required to learn model output" >&2
        return 1
    }
    TIPE_REPLAY_REQUEST="$request_path" TIPE_REPLAY_OUTPUT="$output_path" TIPE_REPLAY_PREFERENCES="$preferences_path" \
        python3 - <<'PY'
import os
import pathlib
import re
import sys
import fcntl

request_path = pathlib.Path(os.environ["TIPE_REPLAY_REQUEST"])
output_path = pathlib.Path(os.environ["TIPE_REPLAY_OUTPUT"])
preferences_path = pathlib.Path(os.environ["TIPE_REPLAY_PREFERENCES"])
max_saved_preference_rows = 2048
max_saved_raw_token_rows = 512
max_saved_correction_rows = 512
max_saved_segment_chain_rows = 512
max_saved_learning_count = 1000000


def safe_text(text):
    return bool(text) and "\t" not in text and "\r" not in text and "\n" not in text


def parse_count(text):
    if not re.fullmatch(r"[1-9][0-9]*", text or ""):
        return None
    count = int(text)
    return count if count <= max_saved_learning_count else None


def add_bounded(mapping, key, increment):
    mapping[key] = min(max_saved_learning_count, mapping.get(key, 0) + increment)


def preference_activation_count(preedit, candidate):
    return 3 if candidate == preedit else 2


def plausible(typo, corrected):
    if len(typo) < 3 or len(corrected) < 3 or typo == corrected:
        return False
    if len(corrected) == len(typo) + 1:
        for skipped in range(len(corrected)):
            if corrected[:skipped] + corrected[skipped + 1 :] == typo:
                return True
    if max(len(typo), len(corrected)) < 5:
        return False
    previous = list(range(len(corrected) + 1))
    for typo_index, typo_ch in enumerate(typo, 1):
        current = [typo_index] + [0] * len(corrected)
        row_best = current[0]
        for correction_index, correction_ch in enumerate(corrected, 1):
            cost = 0 if typo_ch == correction_ch else 1
            current[correction_index] = min(
                previous[correction_index] + 1,
                current[correction_index - 1] + 1,
                previous[correction_index - 1] + cost,
            )
            row_best = min(row_best, current[correction_index])
        if row_best > 2:
            return False
        previous = current
    return previous[len(corrected)] <= 2


def plausible_segment_chain(chain):
    original, consumed, committed, remaining, corrected_full, combined = chain
    return (
        all(safe_text(field) for field in chain)
        and combined.startswith(committed)
        and (consumed + remaining == original or consumed + remaining == corrected_full)
        and (corrected_full == original or plausible(original, corrected_full))
    )


def looks_like_english_identifier(preedit):
    if len(preedit) < 2 or not preedit.isascii():
        return False
    lowered = preedit.lower()
    known = {
        "typescript", "flatpak", "github", "docker", "cursor", "openai", "python", "vscode",
        "wayland", "cargo", "cmake", "codex", "fcitx", "linux", "react", "bash",
        "javascript", "cargobuild", "cmakebuild", "hyprland", "chatgpt", "ollama", "waybar", "systemd",
        "gnome", "dbus", "build", "json", "node", "niri", "npm", "rust",
        "vue", "api", "gtk", "gpt4", "qwen2", "qwen3", "ipv4", "ipv6", "gpt", "git", "tipe",
    }
    if lowered in known:
        return True
    if not preedit.isalpha():
        return False
    if lowered in {"er", "lv", "nv", "lve", "nve"}:
        return False
    if lowered.endswith("g") and not lowered.endswith("ng"):
        return True
    if lowered[-1] in "bcdfhjklmpqrstvwxyz":
        return True
    for index, ch in enumerate(lowered):
        next_ch = lowered[index + 1] if index + 1 < len(lowered) else ""
        prev_ch = lowered[index - 1] if index > 0 else ""
        if ch == "v" and prev_ch not in ("l", "n"):
            return True
        if ch == "x" and next_ch not in ("i", "u"):
            return True
        if ch == "q" and next_ch not in ("i", "u"):
            return True
    markers = (
        "ck", "cl", "cr", "ct", "dr", "ea", "ee", "fl", "ft", "gr", "ld", "ll",
        "lt", "mp", "nd", "nt", "oo", "ph", "pl", "pr", "pt", "rb", "rd", "rk",
        "rn", "rs", "rt", "sk", "sl", "sm", "sn", "sp", "ss", "st", "sv", "sw",
        "th", "tr", "ts", "tw", "xt",
    )
    return any(marker in lowered for marker in markers)


preedit = ""
candidates = []
candidate_consumed_prefixes = {}
request_preferences = set()
request_corrections = set()
request_segment_chains = set()
request_segment_candidates = set()
request_selected_candidate = ""
request_pending_segments = []
for raw_line in request_path.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
    line = raw_line.removesuffix("\r")
    fields = line.split("\t")
    if not fields:
        continue
    if fields[0] == "preedit" and len(fields) >= 2:
        preedit = fields[1]
    elif fields[0] == "candidates":
        candidates = fields[1:]
    elif fields[0] == "candidate_metadata" and len(fields) >= 4:
        try:
            candidate_index = int(fields[1])
            consumed_prefix = int(fields[3]) if fields[2] == "consumed_prefix" else 0
        except ValueError:
            continue
        if candidate_index >= 0:
            candidate_consumed_prefixes[candidate_index] = consumed_prefix
    elif fields[0] == "selected_candidate" and len(fields) >= 3:
        request_selected_candidate = fields[2]
    elif fields[0] == "pending_segment" and len(fields) >= 5:
        request_pending_segments.append(tuple(fields[1:5]))
    elif fields[0] == "preference" and len(fields) >= 4:
        count = parse_count(fields[3])
        if (
            count
            and count >= preference_activation_count(fields[1], fields[2])
            and (fields[1] != fields[2] or looks_like_english_identifier(fields[1]))
        ):
            request_preferences.add((fields[1], fields[2]))
    elif fields[0] == "correction" and len(fields) >= 3:
        request_corrections.add((fields[1], fields[2]))
    elif fields[0] == "segment_chain" and len(fields) == 7:
        chain = tuple(fields[1:7])
        request_segment_chains.add(chain)
        if fields[1] == preedit:
            request_segment_candidates.add((fields[1], fields[6]))

allowed = set(candidates)
prefix_only_candidates = set()
metadata_candidates = set()
full_metadata_candidates = set()
for candidate_index, consumed_prefix in candidate_consumed_prefixes.items():
    if candidate_index >= len(candidates):
        continue
    candidate = candidates[candidate_index]
    metadata_candidates.add(candidate)
    if 0 < consumed_prefix < len(preedit):
        prefix_only_candidates.add(candidate)
    else:
        full_metadata_candidates.add(candidate)
prefix_only_candidates.difference_update(full_metadata_candidates)
for candidate in candidates:
    if (
        candidate
        and candidate not in metadata_candidates
        and any(other != candidate and other.startswith(candidate) for other in candidates)
    ):
        prefix_only_candidates.add(candidate)


def request_pending_segment_chain(chain):
    original, consumed, committed, remaining, _corrected_full, combined = chain
    if remaining != preedit or not combined.startswith(committed):
        return False
    suffix_candidate = combined[len(committed):]
    if not suffix_candidate or suffix_candidate not in allowed:
        return False
    return (original, consumed, committed, remaining) in request_pending_segments


def current_request_correction(typo, corrected):
    return bool(preedit) and (typo == preedit or corrected == preedit)


selected = {}
raw_tokens = {}
corrections = {}
segment_chains = {}
runtime_rules = set()
preferences_path.parent.mkdir(parents=True, exist_ok=True)
lock_path = preferences_path.with_name(preferences_path.name + ".lock")
lock_file = lock_path.open("a+b")
fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
if preferences_path.exists():
    for raw_line in preferences_path.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
        line = raw_line.removesuffix("\r")
        fields = line.split("\t")
        if len(fields) == 3 and fields[0] == "__raw_token__":
            count = parse_count(fields[2])
            if count and re.fullmatch(r"[a-z]{2,}", fields[1]):
                raw_tokens[fields[1]] = count
        elif len(fields) == 4 and fields[0] == "__correction__":
            count = parse_count(fields[3])
            if count and safe_text(fields[1]) and safe_text(fields[2]) and plausible(fields[1], fields[2]):
                corrections[(fields[1], fields[2])] = count
        elif len(fields) == 8 and fields[0] == "__segment_chain__":
            count = parse_count(fields[7])
            chain = tuple(fields[1:7])
            if count and plausible_segment_chain(chain):
                segment_chains[chain] = count
        elif fields and fields[0] == "__correction_pattern__":
            if (
                len(fields) == 7
                and fields[1] in {"missing", "extra", "replace", "transpose"}
                and fields[2].isascii()
                and fields[3].isascii()
                and (not fields[2] or fields[2].isalnum())
                and (not fields[3] or fields[3].isalnum())
                and (len(fields[2]), len(fields[3])) == {
                    "missing": (0, 1), "extra": (1, 0), "replace": (1, 1), "transpose": (2, 2)
                }[fields[1]]
                and fields[4].isdigit()
                and 0 <= int(fields[4]) <= 63
                and fields[5] in {"0", "1"}
                and parse_count(fields[6])
            ):
                runtime_rules.add(line)
        elif fields and fields[0] == "__key_habit__":
            if (
                len(fields) == 5
                and fields[1] in {"missing", "extra", "replace", "transpose"}
                and fields[2].isascii()
                and fields[3].isascii()
                and (not fields[2] or fields[2].isalnum())
                and (not fields[3] or fields[3].isalnum())
                and (len(fields[2]), len(fields[3])) == {
                    "missing": (0, 1), "extra": (1, 0), "replace": (1, 1), "transpose": (2, 2)
                }[fields[1]]
                and parse_count(fields[4])
            ):
                runtime_rules.add(line)
        elif len(fields) == 3:
            count = parse_count(fields[2])
            if count and safe_text(fields[0]) and safe_text(fields[1]):
                selected[(fields[0], fields[1])] = count
        elif len(fields) == 2:
            count = parse_count(fields[1])
            if count and safe_text(fields[0]):
                selected[(fields[0], "")] = count
blocked_selected = {
    key for key, count in selected.items()
    if key[1] and count >= preference_activation_count(key[0], key[1])
}
blocked_corrections = set(corrections)
blocked_segment_chains = set(segment_chains)

accepted_candidates = 0
accepted_corrections = 0
accepted_preferences = 0
accepted_segment_chains = 0
rank = 0
for raw_line in output_path.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
    line = raw_line.removesuffix("\r")
    if not line:
        continue
    fields = line.split("\t")
    if fields[0] == "candidate" and len(fields) >= 2:
        candidate = fields[1]
    elif fields[0] == "correction" and len(fields) >= 3:
        typo, corrected = fields[1], fields[2]
        if (typo, corrected) in request_corrections or (typo, corrected) in blocked_corrections:
            continue
        if safe_text(typo) and safe_text(corrected) and plausible(typo, corrected) and current_request_correction(typo, corrected):
            add_bounded(corrections, (typo, corrected), 2)
            blocked_corrections.add((typo, corrected))
            accepted_corrections += 1
        continue
    elif fields[0] == "preference" and len(fields) in (3, 4):
        learned_preedit, candidate = fields[1], fields[2]
        count = parse_count(fields[3]) if len(fields) == 4 else 2
        if (
            count
            and learned_preedit == preedit
            and (candidate in allowed or (candidate == preedit and looks_like_english_identifier(preedit)))
            and (candidate != learned_preedit or looks_like_english_identifier(learned_preedit))
            and candidate not in prefix_only_candidates
            and safe_text(learned_preedit)
            and safe_text(candidate)
        ):
            if (
                (learned_preedit, candidate) in request_preferences
                or (learned_preedit, candidate) in request_segment_candidates
                or (learned_preedit, candidate) in blocked_selected
            ):
                continue
            add_bounded(selected, (learned_preedit, candidate), min(count, 20))
            blocked_selected.add((learned_preedit, candidate))
            accepted_preferences += 1
        continue
    elif fields[0] == "segment_chain" and len(fields) in (7, 8):
        chain = tuple(fields[1:7])
        if chain in request_segment_chains or chain in blocked_segment_chains:
            continue
        count = parse_count(fields[7]) if len(fields) == 8 else 1
        if count and plausible_segment_chain(chain):
            original, _consumed, _committed, _remaining, corrected_full, combined = chain
            if (
                (
                    original == preedit
                    and (combined in allowed or (combined == preedit and looks_like_english_identifier(preedit)))
                )
                or request_pending_segment_chain(chain)
            ):
                add_bounded(segment_chains, chain, min(count, 20))
                blocked_segment_chains.add(chain)
                add_bounded(selected, (original, combined), min(count, 20))
                if corrected_full != original:
                    add_bounded(corrections, (original, corrected_full), 2)
                accepted_segment_chains += 1
        continue
    else:
        candidate = fields[0]
    if candidate not in allowed and not (candidate == preedit and looks_like_english_identifier(preedit)):
        continue
    if candidate == preedit and not looks_like_english_identifier(preedit):
        continue
    if not safe_text(preedit) or not safe_text(candidate):
        continue
    if candidate in prefix_only_candidates:
        continue
    rank += 1
    if rank != 1:
        continue
    if candidates and candidate == candidates[0] and candidate != preedit:
        continue
    if (preedit, candidate) in request_preferences or (preedit, candidate) in request_segment_candidates:
        continue
    if (preedit, candidate) in blocked_selected:
        continue
    weight = 3 if candidate == preedit else 2
    add_bounded(selected, (preedit, candidate), weight)
    blocked_selected.add((preedit, candidate))
    accepted_candidates += 1

tmp_path = preferences_path.with_name(preferences_path.name + f".tmp.{os.getpid()}")
selected_rows = sorted(selected.items(), key=lambda item: (-item[1], item[0][0], item[0][1]))[
    :max_saved_preference_rows
]
correction_rows = sorted(corrections.items(), key=lambda item: (-item[1], item[0][0], item[0][1]))[
    :max_saved_correction_rows
]
segment_chain_rows = sorted(segment_chains.items(), key=lambda item: (-item[1], item[0]))[
    :max_saved_segment_chain_rows
]
raw_token_rows = sorted(raw_tokens.items(), key=lambda item: (-item[1], item[0]))[
    :max_saved_raw_token_rows
]
with tmp_path.open("w", encoding="utf-8", errors="surrogateescape") as output:
    for (stored_preedit, candidate), count in selected_rows:
        if candidate:
            output.write(f"{stored_preedit}\t{candidate}\t{count}\n")
        else:
            output.write(f"{stored_preedit}\t{count}\n")
    for token, count in raw_token_rows:
        output.write(f"__raw_token__\t{token}\t{count}\n")
    for rule in sorted(runtime_rules):
        output.write(rule + "\n")
    for (typo, corrected), count in correction_rows:
        output.write(f"__correction__\t{typo}\t{corrected}\t{count}\n")
    for chain, count in segment_chain_rows:
        output.write("__segment_chain__\t" + "\t".join(chain) + f"\t{count}\n")
tmp_path.replace(preferences_path)
fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
lock_file.close()
print(
    f"model-output-learned\tpreferences\t{accepted_candidates + accepted_preferences}\tcorrections\t{accepted_corrections}\tsegment-chains\t{accepted_segment_chains}\tpath\t{preferences_path}",
    file=sys.stderr,
)
if accepted_candidates + accepted_preferences + accepted_corrections + accepted_segment_chains == 0:
    reason = "already-known-or-no-new-safe-row" if output_path.read_text(encoding="utf-8", errors="surrogateescape").strip() else "empty-output"
    print(f"model-output-note\tno-new-learning\t{preedit}\t{reason}", file=sys.stderr)
for rank, ((stored_preedit, candidate), count) in enumerate(selected_rows[:3], 1):
    print(
        f"model-output-learned-top-preference\t{rank}\t{stored_preedit}\t{candidate}\t{count}",
        file=sys.stderr,
    )
for rank, ((typo, corrected), count) in enumerate(correction_rows[:3], 1):
    print(
        f"model-output-learned-top-correction\t{rank}\t{typo}\t{corrected}\t{count}",
        file=sys.stderr,
    )
pattern_counts = {}
for (typo, corrected), count in correction_rows:
    if len(corrected) == len(typo) + 1:
        for index, ch in enumerate(corrected):
            if corrected[:index] + corrected[index + 1:] == typo:
                offset = len(typo) - index
                position = f"end-{offset}" if offset <= 2 else str(index)
                key = ("missing", ch, position)
                pattern_counts[key] = pattern_counts.get(key, 0) + count
                break
    elif len(typo) == len(corrected) + 1:
        for index, ch in enumerate(typo):
            if typo[:index] + typo[index + 1:] == corrected:
                offset = len(typo) - index - 1
                position = f"end-{offset}" if offset <= 1 else str(index)
                key = ("extra", ch, position)
                pattern_counts[key] = pattern_counts.get(key, 0) + count
                break
    elif len(typo) == len(corrected):
        diffs = [(index, wrong, right) for index, (wrong, right) in enumerate(zip(typo, corrected)) if wrong != right]
        if len(diffs) == 1:
            index, wrong, right = diffs[0]
            offset = len(typo) - index - 1
            position = f"end-{offset}" if offset <= 1 else str(index)
            key = ("replace", f"{wrong}->{right}", position)
            pattern_counts[key] = pattern_counts.get(key, 0) + count
for rank, ((kind, text, position), count) in enumerate(
    sorted(pattern_counts.items(), key=lambda item: (-item[1], item[0][0], item[0][2], item[0][1]))[:3],
    1,
):
    print(
        f"model-output-learned-top-correction-pattern\t{rank}\t{count}\t{kind}\t{text}\t{position}",
        file=sys.stderr,
    )
for rank, (chain, count) in enumerate(segment_chain_rows[:3], 1):
    print(
        "model-output-learned-top-segment-chain\t"
        f"{rank}\t{chain[0]}\t{chain[1]}\t{chain[2]}\t{chain[3]}\t{chain[4]}\t{chain[5]}\t{count}",
        file=sys.stderr,
    )
PY
    emit_preferences_summary
}

emit_preferences_summary() {
    local preferences_checker line kind rest
    [[ -n "${preferences_path:-}" && -r "$preferences_path" ]] || return 0
    preferences_checker=$(helper_path tipe-check-preferences) || return 0
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        [[ -z "$line" ]] && continue
        IFS=$'\t' read -r kind rest <<< "$line"
        case "$kind" in
            summary)
                printf 'model-output-preferences-summary\t%s\n' "${rest:-}" >&2
                ;;
            top-preference)
                printf 'model-output-preferences-top-preference\t%s\n' "${rest:-}" >&2
                ;;
            top-legacy-preference)
                printf 'model-output-preferences-top-legacy-preference\t%s\n' "${rest:-}" >&2
                ;;
            top-correction)
                printf 'model-output-preferences-top-correction\t%s\n' "${rest:-}" >&2
                ;;
            top-correction-pattern)
                printf 'model-output-preferences-top-correction-pattern\t%s\n' "${rest:-}" >&2
                ;;
            top-segment-chain)
                printf 'model-output-preferences-top-segment-chain\t%s\n' "${rest:-}" >&2
                ;;
        esac
    done < <("$preferences_checker" --summary --top 3 "$preferences_path" 2>/dev/null || true)
}

if [[ "$explain_request" == "1" ]]; then
    if explain_helper=$(helper_path tipe-model-explain); then
        "$explain_helper" "$request_path" >&2
    else
        echo "tipe-model-explain helper is not available" >&2
        exit 1
    fi
fi

if [[ "$dry_run_model" == "1" ]]; then
    output_path=$(mktemp)
    cleanup_paths+=("$output_path")
    run_model_command <"$request_path" >"$output_path"
    validate_adapter_dry_run "$output_path"
    cat "$output_path"
elif [[ "$check_output" == "1" ]]; then
    checker=$(helper_path tipe-model-wrapper-check) || {
        echo "tipe-model-wrapper-check helper is not available" >&2
        exit 1
    }
    output_path=$(mktemp)
    check_stderr=$(mktemp)
    cleanup_paths+=("$output_path" "$check_stderr")
    run_model_command <"$request_path" >"$output_path"
    if "$checker" --command "$model_command" --request "$request_path" --output "$output_path" >"$check_stderr" 2>&1; then
        cat "$check_stderr" >&2
    else
        status=$?
        cat "$check_stderr" >&2
        explain_rejected_model_output "$output_path" "$status"
        exit "$status"
    fi
    if [[ "$explain_output" == "1" ]]; then
        explain_model_output "$output_path"
    fi
    if [[ "$learn_output" == "1" ]]; then
        learn_model_output "$output_path"
    fi
    cat "$output_path"
else
    run_model_command <"$request_path"
fi
