#!/usr/bin/env bash
set -euo pipefail

wrapper=""
request_path=""
output_path=""
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
  tipe-model-wrapper-check --command PATH [--request REQUEST_TSV] [--output OUTPUT_TSV]

Runs a custom TiPE model wrapper against a sample or supplied model request and
checks that stdout contains only safe TiPE protocol rows. With --output, checks
an already captured wrapper output instead of running the wrapper.
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
        --command)
            require_value "$1" "${2:-}"
            wrapper="$2"
            shift
            ;;
        --request)
            require_value "$1" "${2:-}"
            request_path="$2"
            shift
            ;;
        --output)
            require_value "$1" "${2:-}"
            output_path="$2"
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

if [[ -z "$wrapper" ]]; then
    echo "--command is required" >&2
    exit 2
fi
if [[ "$request_path" == "-" ]]; then
    tmp_request=$(mktemp)
    cleanup_paths+=("$tmp_request")
    cat >"$tmp_request"
    request_path="$tmp_request"
fi
if [[ -n "$request_path" && ! -r "$request_path" ]]; then
    echo "cannot read request: $request_path" >&2
    exit 1
fi

command_tokens=()
command_env=()
parse_wrapper_command() {
    if [[ -z "$wrapper" || ! "$wrapper" =~ ^[A-Za-z0-9_./:=+\ -]+$ ]]; then
        echo "wrapper command may only contain safe path, word, assignment, and space characters" >&2
        exit 2
    fi
    read -r -a command_tokens <<< "$wrapper"
    while [[ "${#command_tokens[@]}" -gt 0 && "${command_tokens[0]}" =~ ^[A-Za-z_][A-Za-z0-9_]*=.+$ ]]; do
        command_env+=("${command_tokens[0]}")
        command_tokens=("${command_tokens[@]:1}")
    done
    if [[ "${#command_tokens[@]}" -eq 0 ]]; then
        echo "wrapper command is missing an executable" >&2
        exit 2
    fi
    if [[ ! -x "${command_tokens[0]}" ]]; then
        echo "wrapper is not executable: ${command_tokens[0]}" >&2
        exit 1
    fi
}

parse_wrapper_command
if [[ -n "$output_path" && ! -r "$output_path" ]]; then
    echo "cannot read output: $output_path" >&2
    exit 1
fi

sample_request() {
    cat <<'EOF'
protocol	1
preedit	nihao
application	Alacritty
candidates	你好	你号	拟好
candidate_metadata	0	consumed_prefix	0	source	full	score	1000000
candidate_metadata	1	consumed_prefix	0	source	full	score	999999
candidate_metadata	2	consumed_prefix	0	source	full	score	500000
state	preedit_cursor	5	candidate_cursor	0	expanded	0
runtime_state	continuous	0
selected_candidate	0	你好
visible_candidates	0:你好	1:你号	2:拟好
numbered_candidates	1:0:你好	2:1:你号	3:2:拟好
events	letter:n	letter:i	letter:h	letter:a	letter:o
correction_events	letter:i	letter:h	letter:a	letter:o	backspace:	backspace:	backspace:	backspace:	letter:n	letter:i	letter:h	letter:a	letter:o
context	刚才
segment_chain	nihao	ni	你	hao	nihao	你好
preference	nihao	你号	3
correction	ihao	nihao	2
EOF
}

request_content=""
if [[ -n "$request_path" ]]; then
    request_content=$(cat "$request_path")
else
    request_content=$(sample_request)
fi

if [[ -n "$output_path" ]]; then
    output=$(cat "$output_path")
else
    output=$(printf '%s\n' "$request_content" | env "${command_env[@]}" "${command_tokens[@]}")
fi

preedit=$(
    printf '%s\n' "$request_content" |
        awk -F '\t' '$1 == "preedit" {print $2; exit}'
)

mapfile -t allowed_candidates < <(
    printf '%s\n' "$request_content" |
        awk -F '\t' '$1 == "candidates" {for (i = 2; i <= NF; ++i) print $i; exit}'
)

mapfile -t prefix_only_candidates < <(
    REQUEST_CONTENT="$request_content" python3 - <<'PY'
import os

lines = os.environ["REQUEST_CONTENT"].splitlines()
preedit = ""
candidates = []
candidate_consumed_prefixes = {}
for raw_line in lines:
    fields = raw_line.removesuffix("\r").split("\t")
    if not fields:
        continue
    if fields[0] == "candidates":
        candidates = fields[1:]
    elif fields[0] == "preedit" and len(fields) >= 2:
        preedit = fields[1]
    elif fields[0] == "candidate_metadata" and len(fields) >= 4:
        try:
            index = int(fields[1])
            consumed = int(fields[3]) if fields[2] == "consumed_prefix" else 0
        except ValueError:
            continue
        if index >= 0:
            candidate_consumed_prefixes[index] = consumed
prefix_only = set()
metadata_candidates = set()
full_consumed = set()
for index, consumed in candidate_consumed_prefixes.items():
    if index >= len(candidates):
        continue
    candidate = candidates[index]
    metadata_candidates.add(candidate)
    if 0 < consumed < len(preedit):
        prefix_only.add(candidate)
    else:
        full_consumed.add(candidate)
prefix_only.difference_update(full_consumed)
for candidate in candidates:
    if (
        candidate
        and candidate not in metadata_candidates
        and any(other != candidate and other.startswith(candidate) for other in candidates)
    ):
        prefix_only.add(candidate)
for candidate in sorted(prefix_only):
    print(candidate)
PY
)

mapfile -t pending_segments < <(
    printf '%s\n' "$request_content" |
        awk -F '\t' '$1 == "pending_segment" && NF >= 5 {print $2 "\t" $3 "\t" $4 "\t" $5}'
)

selected_candidate=$(
    printf '%s\n' "$request_content" |
        awk -F '\t' '$1 == "selected_candidate" && NF >= 3 {print $2 "\t" $3; exit}'
)

is_allowed_candidate() {
    local candidate="$1"
    local allowed
    for allowed in "${allowed_candidates[@]}"; do
        [[ "$candidate" == "$allowed" ]] && return 0
    done
    return 1
}

looks_like_raw_english_preedit() {
    local candidate="$1"
    [[ -n "$preedit" && "$candidate" == "$preedit" ]] || return 1
    local lowered="${candidate,,}"
    case "$lowered" in
        typescript|flatpak|github|docker|cursor|openai|python|vscode|wayland|cargo|cmake|codex|fcitx|linux|react|bash|javascript|cargobuild|cmakebuild|hyprland|chatgpt|ollama|waybar|systemd|gnome|dbus|build|json|node|niri|npm|rust|vue|api|gtk|gpt4|qwen2|qwen3|ipv4|ipv6|git|gpt|tipe)
            return 0
            ;;
    esac
    [[ "$candidate" =~ ^[A-Za-z]{2,}$ ]] || return 1
    case "$lowered" in
        er|lv|nv|lve|nve)
            return 1
            ;;
        *g)
            [[ "$lowered" == *ng ]] || return 0
            ;;
        *[bcdfhjklmpqrstvwxyz])
            return 0
            ;;
    esac
    case "$lowered" in
        v*|*[!ln]v*|*x[!iu]*|*q[!iu]*)
            return 0
            ;;
        *ck*|*cl*|*cr*|*ct*|*dr*|*ea*|*ee*|*fl*|*ft*|*gr*|*ld*|*ll*|*lt*|*mp*|*nd*|*nt*|*oo*|*ph*|*pl*|*pr*|*pt*|*rb*|*rd*|*rk*|*rn*|*rs*|*rt*|*sk*|*sl*|*sm*|*sn*|*sp*|*ss*|*st*|*sv*|*sw*|*th*|*tr*|*ts*|*tw*|*xt*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_allowed_candidate_output() {
    local candidate="$1"
    is_allowed_candidate "$candidate" || looks_like_raw_english_preedit "$candidate"
}

is_pending_segment_chain_output() {
    local original="$1"
    local consumed="$2"
    local committed="$3"
    local remaining="$4"
    local corrected="$5"
    local combined="$6"
    local segment seg_original seg_consumed seg_committed seg_remaining suffix_candidate
    local selected_index selected_text selected_extra
    [[ "$remaining" == "$preedit" ]] || return 1
    valid_segment_chain_shape "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" || return 1
    suffix_candidate="${combined#"$committed"}"
    [[ -n "$suffix_candidate" ]] || return 1
    is_allowed_candidate_output "$suffix_candidate" || return 1
    [[ -n "$selected_candidate" ]] || return 1
    IFS=$'\t' read -r selected_index selected_text selected_extra <<< "$selected_candidate"
    [[ "$selected_index" =~ ^[1-9][0-9]*$ && "$selected_text" == "$suffix_candidate" ]] || return 1
    for segment in "${pending_segments[@]}"; do
        IFS=$'\t' read -r seg_original seg_consumed seg_committed seg_remaining <<< "$segment"
        if [[ "$seg_original" == "$original" && "$seg_consumed" == "$consumed" &&
            "$seg_committed" == "$committed" && "$seg_remaining" == "$remaining" ]]; then
            return 0
        fi
    done
    return 1
}

is_prefix_only_candidate() {
    local candidate="$1"
    local prefix_only
    for prefix_only in "${prefix_only_candidates[@]}"; do
        [[ "$candidate" == "$prefix_only" ]] && return 0
    done
    return 1
}

plausible_correction() {
    local typo="$1"
    local corrected="$2"
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - "$typo" "$corrected" <<'PY'
import sys

typo = sys.argv[1]
corrected = sys.argv[2]

def plausible(typo, corrected):
    if len(typo) < 3 or len(corrected) < 3 or typo == corrected:
        return False
    if any(ch in typo + corrected for ch in "\t\r\n"):
        return False
    if len(corrected) == len(typo) + 1:
        for skipped in range(len(corrected)):
            if corrected[:skipped] + corrected[skipped + 1:] == typo:
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

sys.exit(0 if plausible(typo, corrected) else 1)
PY
}

current_request_correction() {
    local typo="$1"
    local corrected="$2"
    [[ -n "$preedit" ]] || return 1
    [[ "$typo" == "$preedit" || "$corrected" == "$preedit" ]]
}

valid_segment_chain_shape() {
    local original="$1"
    local consumed="$2"
    local committed="$3"
    local remaining="$4"
    local corrected="$5"
    local combined="$6"
    [[ "$combined" == "$committed"* ]] || return 1
    [[ "$consumed$remaining" == "$original" || "$consumed$remaining" == "$corrected" ]] || return 1
}

line_number=0
valid_rows=0
while IFS= read -r line || [[ -n "$line" ]]; do
    ((++line_number))
    line="${line%$'\r'}"
    [[ -z "$line" ]] && continue
    IFS=$'\t' read -r kind first second extra <<< "$line"
    case "$kind" in
        candidate)
            if [[ -z "${first:-}" || -n "${second:-}" ]]; then
                echo "invalid candidate row at output line $line_number" >&2
                exit 1
            fi
            if ! is_allowed_candidate_output "$first"; then
                echo "candidate is not in request at output line $line_number: $first" >&2
                exit 1
            fi
            ;;
        correction)
            if [[ -z "${first:-}" || -z "${second:-}" || -n "${extra:-}" ]]; then
                echo "invalid correction row at output line $line_number" >&2
                exit 1
            fi
            if ! plausible_correction "$first" "$second"; then
                echo "correction is not plausible at output line $line_number: $first -> $second" >&2
                exit 1
            fi
            if ! current_request_correction "$first" "$second"; then
                echo "correction is not for current preedit at output line $line_number: $first -> $second" >&2
                exit 1
            fi
            ;;
        preference)
            IFS=$'\t' read -r _ preference_preedit preference_candidate preference_count preference_extra <<< "$line"
            if [[ -z "${preference_preedit:-}" || -z "${preference_candidate:-}" || -n "${preference_extra:-}" ]]; then
                echo "invalid preference row at output line $line_number" >&2
                exit 1
            fi
            if [[ "$preference_preedit" != "$preedit" ]]; then
                echo "preference row is not for current preedit at output line $line_number: $preference_preedit" >&2
                exit 1
            fi
            if ! is_allowed_candidate_output "$preference_candidate"; then
                echo "preference candidate is not in request at output line $line_number: $preference_candidate" >&2
                exit 1
            fi
            if is_prefix_only_candidate "$preference_candidate"; then
                echo "preference candidate is prefix-only at output line $line_number: $preference_candidate" >&2
                exit 1
            fi
            if [[ -n "${preference_count:-}" && ! "$preference_count" =~ ^[1-9][0-9]*$ ]]; then
                echo "invalid preference count at output line $line_number: $preference_count" >&2
                exit 1
            fi
            ;;
        segment_chain)
            IFS=$'\t' read -r _ chain_original chain_consumed chain_committed chain_remaining chain_corrected chain_combined chain_count chain_extra <<< "$line"
            if [[ -z "${chain_original:-}" || -z "${chain_consumed:-}" || -z "${chain_committed:-}" ||
                -z "${chain_remaining:-}" || -z "${chain_corrected:-}" || -z "${chain_combined:-}" ||
                -n "${chain_extra:-}" ]]; then
                echo "invalid segment_chain row at output line $line_number" >&2
                exit 1
            fi
            if [[ "$chain_original" != "$preedit" ]] &&
                ! is_pending_segment_chain_output "$chain_original" "$chain_consumed" "$chain_committed" "$chain_remaining" "$chain_corrected" "$chain_combined"; then
                echo "segment_chain row is not for current preedit at output line $line_number: $chain_original" >&2
                exit 1
            fi
            if ! is_allowed_candidate_output "$chain_combined" &&
                ! is_pending_segment_chain_output "$chain_original" "$chain_consumed" "$chain_committed" "$chain_remaining" "$chain_corrected" "$chain_combined"; then
                echo "segment_chain combined candidate is not in request at output line $line_number: $chain_combined" >&2
                exit 1
            fi
            if [[ "$chain_corrected" != "$chain_original" ]] && ! plausible_correction "$chain_original" "$chain_corrected"; then
                echo "segment_chain correction is not plausible at output line $line_number: $chain_original -> $chain_corrected" >&2
                exit 1
            fi
            if ! valid_segment_chain_shape "$chain_original" "$chain_consumed" "$chain_committed" "$chain_remaining" "$chain_corrected" "$chain_combined"; then
                echo "segment_chain row shape is not plausible at output line $line_number" >&2
                exit 1
            fi
            if [[ -n "${chain_count:-}" && ! "$chain_count" =~ ^[1-9][0-9]*$ ]]; then
                echo "invalid segment_chain count at output line $line_number: $chain_count" >&2
                exit 1
            fi
            ;;
        *)
            if ! is_allowed_candidate_output "$kind" || [[ -n "${first:-}" ]]; then
                echo "unknown output row at line $line_number: $line" >&2
                exit 1
            fi
            ;;
    esac
    ((++valid_rows))
done <<< "$output"

printf 'wrapper-ok\t%s\trows\t%s\n' "$wrapper" "$valid_rows"
