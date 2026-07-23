#!/usr/bin/env bash
set -euo pipefail

backend="${TIPE_MODEL_BACKEND:-heuristic}"
model="${TIPE_MODEL_NAME:-qwen2.5:0.5b}"
base_url="${TIPE_MODEL_BASE_URL:-http://127.0.0.1:11434/v1}"
chat_path="${TIPE_MODEL_CHAT_PATH:-/chat/completions}"
api_key="${TIPE_MODEL_API_KEY:-}"
api_key_file="${TIPE_MODEL_API_KEY_FILE:-}"
timeout_seconds="${TIPE_MODEL_HTTP_TIMEOUT_SECONDS:-8}"
temperature="${TIPE_MODEL_TEMPERATURE:-0}"
max_tokens="${TIPE_MODEL_MAX_TOKENS:-128}"
dry_run="${TIPE_MODEL_DRY_RUN:-0}"
send_recent_input="${TIPE_MODEL_SEND_RECENT_INPUT:-}"
send_surrounding="${TIPE_MODEL_SEND_SURROUNDING:-}"
llama_command="${TIPE_LLAMA_CPP_COMMAND:-/usr/bin/llama-cli}"
llama_threads="${TIPE_LLAMA_CPP_THREADS:-6}"
llama_context="${TIPE_LLAMA_CPP_CONTEXT:-8192}"
llama_tmp_dir=""
http_header_file=""

if [[ -z "$send_recent_input" ]]; then
    case "${TIPE_MODEL_MODE:-}" in
        openai|openai-compatible) send_recent_input=0 ;;
        *) send_recent_input=1 ;;
    esac
fi
if [[ -z "$send_surrounding" ]]; then
    case "${TIPE_MODEL_MODE:-}" in
        openai|openai-compatible) send_surrounding=0 ;;
        *) send_surrounding=1 ;;
    esac
fi
case "$send_recent_input" in
    1|on|true) send_recent_input=1 ;;
    0|off|false) send_recent_input=0 ;;
    *) echo "TIPE_MODEL_SEND_RECENT_INPUT must be 0 or 1" >&2; exit 2 ;;
esac
case "$send_surrounding" in
    1|on|true) send_surrounding=1 ;;
    0|off|false) send_surrounding=0 ;;
    *) echo "TIPE_MODEL_SEND_SURROUNDING must be 0 or 1" >&2; exit 2 ;;
esac

if [[ -z "$api_key" && -n "$api_key_file" ]]; then
    if [[ ! -f "$api_key_file" || -L "$api_key_file" || ! -r "$api_key_file" ]]; then
        echo "TIPE_MODEL_API_KEY_FILE is not a readable regular file" >&2
        exit 2
    fi
    api_key_size=$(stat -c '%s' "$api_key_file")
    api_key_owner=$(stat -c '%u' "$api_key_file")
    api_key_mode=$(stat -c '%a' "$api_key_file")
    if (( api_key_size <= 0 || api_key_size > 16385 )) ||
        [[ "$api_key_owner" != "$(id -u)" ]] || (( (8#$api_key_mode & 077) != 0 )); then
        echo "TIPE_MODEL_API_KEY_FILE must be user-owned, non-empty, at most 16385 bytes, and inaccessible to group/other" >&2
        exit 2
    fi
    api_key=$(<"$api_key_file")
fi

preedit=""
application=""
surrounding_before=""
surrounding_after=""
candidates=()
candidate_metadata=()
input_state=""
runtime_state=""
supervision_state=""
selected_candidate=""
visible_candidates=()
numbered_candidates=()
events=()
event_counts=()
correction_events=()
correction_event_counts=()
context=()
segment_chains=()
pending_segments=()
preferences=()
corrections=()

lower_ascii() {
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

looks_like_english_identifier_value() {
    local value="$1"
    local lowered
    lowered=$(lower_ascii "$value")
    case "$lowered" in
        typescript|flatpak|github|docker|cursor|openai|python|vscode|wayland|cargo|cmake|codex|fcitx|linux|react|bash|javascript|cargobuild|cmakebuild|hyprland|chatgpt|ollama|waybar|systemd|gnome|dbus|build|json|node|niri|npm|rust|vue|api|gtk|gpt4|qwen2|qwen3|ipv4|ipv6|git|gpt|tipe)
            return 0
            ;;
    esac
    [[ "$value" =~ ^[A-Za-z]{2,}$ ]] || return 1
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

is_active_preference_evidence() {
    local row="$1"
    local learned_preedit learned_candidate learned_count extra
    IFS=$'\t' read -r learned_preedit learned_candidate learned_count extra <<< "$row"
    [[ -n "$learned_preedit" && -n "$learned_candidate" &&
        "$learned_count" =~ ^[1-9][0-9]*$ ]] || return 1
    if [[ "$learned_candidate" == "$learned_preedit" ]]; then
        (( learned_count >= 3 )) && looks_like_english_identifier_value "$learned_preedit"
    else
        (( learned_count >= 2 ))
    fi
}

while IFS=$'\t' read -r kind rest || [[ -n "${kind:-}" || -n "${rest:-}" ]]; do
    case "$kind" in
        preedit)
            preedit="$rest"
            ;;
        application)
            application="$rest"
            ;;
        surrounding_before)
            surrounding_before="$rest"
            ;;
        surrounding_after)
            surrounding_after="$rest"
            ;;
        candidates)
            IFS=$'\t' read -r -a candidates <<< "$rest"
            ;;
        candidate_metadata)
            candidate_metadata+=("$rest")
            ;;
        state)
            input_state="$rest"
            ;;
        runtime_state)
            runtime_state="$rest"
            ;;
        supervision_state)
            supervision_state="$rest"
            ;;
        selected_candidate)
            selected_candidate="$rest"
            ;;
        visible_candidates)
            IFS=$'\t' read -r -a visible_candidates <<< "$rest"
            ;;
        numbered_candidates)
            IFS=$'\t' read -r -a numbered_candidates <<< "$rest"
            ;;
        events)
            IFS=$'\t' read -r -a events <<< "$rest"
            ;;
        event_counts)
            IFS=$'\t' read -r -a event_counts <<< "$rest"
            ;;
        correction_events)
            IFS=$'\t' read -r -a correction_events <<< "$rest"
            ;;
        correction_event_counts)
            IFS=$'\t' read -r -a correction_event_counts <<< "$rest"
            ;;
        context)
            IFS=$'\t' read -r -a context <<< "$rest"
            ;;
        segment_chain)
            segment_chains+=("$rest")
            ;;
        pending_segment)
            pending_segments+=("$rest")
            ;;
        preference)
            if is_active_preference_evidence "$rest"; then
                preferences+=("$rest")
            fi
            ;;
        correction)
            corrections+=("$rest")
            ;;
    esac
done

is_plausible_correction() {
    local typo="$1"
    local corrected="$2"
    [[ -n "$typo" && -n "$corrected" ]] || return 1
    [[ "$typo" != *$'\t'* && "$typo" != *$'\r'* && "$typo" != *$'\n'* ]] || return 1
    [[ "$corrected" != *$'\t'* && "$corrected" != *$'\r'* && "$corrected" != *$'\n'* ]] || return 1
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - "$typo" "$corrected" <<'PY'
import sys

typo = sys.argv[1]
corrected = sys.argv[2]

def plausible(typo, corrected):
    if len(typo) < 3 or len(corrected) < 3 or typo == corrected:
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

is_current_request_correction() {
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

infer_correction_from_events() {
    command -v python3 >/dev/null 2>&1 || return 0
    TIPE_HEURISTIC_PREEDIT="$preedit" \
    TIPE_HEURISTIC_CORRECTION_EVENTS="$(printf '%s\n' "${correction_events[@]}")" \
    python3 - <<'PY'
import os

corrected_preedit = os.environ.get("TIPE_HEURISTIC_PREEDIT", "")
events = [line for line in os.environ.get("TIPE_HEURISTIC_CORRECTION_EVENTS", "").splitlines() if line]

def plausible(typo, corrected):
    if len(typo) < 3 or len(corrected) < 3 or typo == corrected:
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

current = ""
cursor = 0
erased_original = ""
last_fully_erased = None
last_edited_original = None
in_preedit_edit_original = None
erasing = False

def remember_in_preedit_edit():
    global in_preedit_edit_original
    if current and in_preedit_edit_original is None:
        in_preedit_edit_original = current

for event in events[-192:]:
    kind, _, text = event.partition(":")
    if kind in {"letter", "digit", "symbol"}:
        if erasing and erased_original:
            last_edited_original = erased_original
        erasing = False
        cursor = max(0, min(cursor, len(current)))
        if cursor < len(current):
            remember_in_preedit_edit()
        current = current[:cursor] + text + current[cursor:]
        cursor += len(text)
    elif kind == "backspace":
        cursor = max(0, min(cursor, len(current)))
        if not current or cursor == 0:
            erasing = False
            erased_original = ""
            continue
        if cursor < len(current):
            remember_in_preedit_edit()
        if not erasing:
            erasing = True
            erased_original = current
        current = current[:cursor - 1] + current[cursor:]
        cursor -= 1
        if not current and erased_original:
            last_fully_erased = erased_original
    elif kind == "delete":
        cursor = max(0, min(cursor, len(current)))
        if cursor >= len(current):
            continue
        remember_in_preedit_edit()
        if not erasing:
            erasing = True
            erased_original = current
        current = current[:cursor] + current[cursor + 1:]
        if not current and erased_original:
            last_fully_erased = erased_original
    elif kind == "cursor-move":
        if text in {"Left", "KP_Left"}:
            cursor = max(0, cursor - 1)
        elif text in {"Right", "KP_Right"}:
            cursor = min(len(current), cursor + 1)
    elif kind in {"candidate-selected", "raw-committed", "escape"}:
        current = ""
        cursor = 0
        erased_original = ""
        last_fully_erased = None
        last_edited_original = None
        in_preedit_edit_original = None
        erasing = False

if current == corrected_preedit:
    seen = set()
    for typo in (last_fully_erased, last_edited_original, in_preedit_edit_original):
        if typo and typo not in seen and plausible(typo, corrected_preedit):
            print(f"correction\t{typo}\t{corrected_preedit}")
            seen.add(typo)
PY
}

is_code_or_terminal_application() {
    local lowered
    lowered=$(lower_ascii "$application")
    case "$lowered" in
        *alacritty*|*code*|*codium*|*cursor*|*emacs*|*ghostty*|*jetbrains*|*kitty*|*konsole*|*neovide*|*terminal*|*vscode*|*wezterm*|*xterm*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

looks_like_code_surrounding() {
    local before_tail after_head nearby lowered
    before_tail="$surrounding_before"
    after_head="$surrounding_after"
    if ((${#before_tail} > 96)); then
        before_tail="${before_tail: -96}"
    fi
    if ((${#after_head} > 32)); then
        after_head="${after_head:0:32}"
    fi
    nearby="${before_tail}${after_head}"
    [[ -n "$nearby" ]] || return 1
    lowered=$(lower_ascii "$nearby")
    case "$lowered" in
        *://*|*::*|*'->'*|*'=>'*|*./*|*../*|*/home/*|*/usr/*|*' --'*|*' -'*|*'git '*|*'npm '*|*'cargo '*|*'cmake '*|*'python '*|*'docker '*|*'const '*|*'let '*|*'var '*|*'fn '*|*'def '*|*'class '*|*'import '*|*'export '*|*'return '*|*'std::'*|*'#include'*)
            return 0
            ;;
    esac
    [[ "${surrounding_before: -1}" =~ [A-Za-z0-9_./:-] ]] && return 0
    [[ "${surrounding_after:0:1}" =~ [A-Za-z0-9_./:-] ]] && return 0
    return 1
}

looks_like_english_identifier() {
    looks_like_english_identifier_value "$preedit"
}

is_allowed_candidate_output() {
    local candidate="$1"
    local allowed
    for allowed in "${candidates[@]}"; do
        [[ "$candidate" == "$allowed" ]] && return 0
    done
    [[ "$candidate" == "$preedit" ]] && looks_like_english_identifier
}

is_pending_segment_chain_output() {
    local original="$1"
    local consumed="$2"
    local committed="$3"
    local remaining="$4"
    local corrected="$5"
    local combined="$6"
    local segment seg_original seg_consumed seg_committed seg_remaining extra suffix_candidate
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
        IFS=$'\t' read -r seg_original seg_consumed seg_committed seg_remaining extra <<< "$segment"
        if [[ "$seg_original" == "$original" && "$seg_consumed" == "$consumed" &&
            "$seg_committed" == "$committed" && "$seg_remaining" == "$remaining" ]]; then
            return 0
        fi
    done
    return 1
}

is_prefix_only_candidate() {
    local candidate="$1"
    local item index key_consumed consumed _key_source _source _key_score _score other
    for item in "${candidate_metadata[@]}"; do
        IFS=$'\t' read -r index key_consumed consumed _key_source _source _key_score _score <<< "$item"
        if [[ "$key_consumed" == "consumed_prefix" && "$consumed" =~ ^[0-9]+$ &&
              "$index" =~ ^[0-9]+$ && "${candidates[$index]:-}" == "$candidate" ]] &&
            (( consumed == 0 || consumed >= ${#preedit} )); then
            return 1
        fi
    done
    for item in "${candidate_metadata[@]}"; do
        IFS=$'\t' read -r index key_consumed consumed _key_source _source _key_score _score <<< "$item"
        if [[ "$key_consumed" == "consumed_prefix" && "$consumed" =~ ^[1-9][0-9]*$ &&
              "$consumed" -lt "${#preedit}" &&
              "$index" =~ ^[0-9]+$ && "${candidates[$index]:-}" == "$candidate" ]]; then
            return 0
        fi
    done
    if [[ "$preedit" =~ ^[A-Za-z]+$ && "${#preedit}" -ge 8 &&
          "$candidate" != *[A-Za-z0-9]* && $(( ${#candidate} * 3 )) -lt "${#preedit}" ]]; then
        return 0
    fi
    for other in "${candidates[@]}"; do
        if [[ "$other" != "$candidate" && "$other" == "$candidate"* ]]; then
            return 0
        fi
    done
    return 1
}

emit_safe_protocol_lines() {
    local line candidate correction_rest typo corrected extra preference_rest learned_preedit learned_candidate learned_count
    local segment_rest original consumed committed remaining combined
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        [[ -z "$line" ]] && continue
        if [[ "$line" == candidate$'\t'* ]]; then
            candidate="${line#candidate$'\t'}"
            if is_allowed_candidate_output "$candidate"; then
                printf 'candidate\t%s\n' "$candidate"
            fi
        elif [[ "$line" == correction$'\t'* ]]; then
            correction_rest="${line#correction$'\t'}"
            if [[ "$correction_rest" == *$'\t'* ]]; then
                IFS=$'\t' read -r typo corrected extra <<< "$correction_rest"
                if is_plausible_correction "$typo" "$corrected" &&
                    is_current_request_correction "$typo" "$corrected"; then
                    printf 'correction\t%s\t%s\n' "$typo" "$corrected"
                fi
            fi
        elif [[ "$line" == preference$'\t'* ]]; then
            preference_rest="${line#preference$'\t'}"
            IFS=$'\t' read -r learned_preedit learned_candidate learned_count extra <<< "$preference_rest"
            if [[ "$learned_preedit" == "$preedit" && "$learned_count" =~ ^[1-9][0-9]*$ ]] &&
                is_allowed_candidate_output "$learned_candidate" && ! is_prefix_only_candidate "$learned_candidate"; then
                printf 'preference\t%s\t%s\t%s\n' "$learned_preedit" "$learned_candidate" "$learned_count"
            fi
        elif [[ "$line" == segment_chain$'\t'* ]]; then
            segment_rest="${line#segment_chain$'\t'}"
            IFS=$'\t' read -r original consumed committed remaining corrected combined learned_count extra <<< "$segment_rest"
            if [[ "$learned_count" =~ ^[1-9][0-9]*$ ]] &&
                { { [[ "$original" == "$preedit" ]] && is_allowed_candidate_output "$combined"; } ||
                    is_pending_segment_chain_output "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined"; } &&
                valid_segment_chain_shape "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" &&
                { [[ "$corrected" == "$original" ]] || is_plausible_correction "$original" "$corrected"; }; then
                printf 'segment_chain\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" "$learned_count"
            fi
        else
            candidate="$line"
            if is_allowed_candidate_output "$candidate"; then
                printf 'candidate\t%s\n' "$candidate"
            fi
        fi
    done
}

emit_segment_chain_hints() {
    local chain original consumed committed remaining corrected combined extra last_context suffix_candidate
    last_context=""
    if ((${#context[@]} > 0)); then
        last_context="${context[$((${#context[@]} - 1))]}"
    fi
    for chain in "${segment_chains[@]}"; do
        IFS=$'\t' read -r original consumed committed remaining corrected combined extra <<< "$chain"
        [[ -n "${original:-}" && -n "${combined:-}" ]] || continue
        valid_segment_chain_shape "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" || continue
        if [[ "$original" == "$preedit" ]] && is_allowed_candidate_output "$combined"; then
            printf 'candidate\t%s\n' "$combined"
        elif [[ -n "$last_context" && "$last_context" == "$committed" && "$remaining" == "$preedit" &&
            "$combined" == "$committed"* ]]; then
            suffix_candidate="${combined#"$committed"}"
            if [[ -n "$suffix_candidate" ]] && is_allowed_candidate_output "$suffix_candidate"; then
                printf 'candidate\t%s\n' "$suffix_candidate"
            fi
        fi
        if [[ -n "${corrected:-}" && "$corrected" != "$original" ]] &&
            is_plausible_correction "$original" "$corrected"; then
            printf 'correction\t%s\t%s\n' "$original" "$corrected"
        fi
    done
}

emit_preference_hints() {
    local row learned_preedit learned_candidate learned_count extra
    for row in "${preferences[@]}"; do
        IFS=$'\t' read -r learned_preedit learned_candidate learned_count extra <<< "$row"
        [[ "$learned_preedit" == "$preedit" && "${learned_count:-}" =~ ^[1-9][0-9]*$ ]] || continue
        if is_allowed_candidate_output "$learned_candidate" && ! is_prefix_only_candidate "$learned_candidate"; then
            printf 'candidate\t%s\n' "$learned_candidate"
        fi
    done
}

supervision_history_path() {
    if [[ -n "${TIPE_SUPERVISION_HISTORY:-}" ]]; then
        printf '%s\n' "$TIPE_SUPERVISION_HISTORY"
    elif [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        printf '%s\n' "$XDG_CACHE_HOME/tipe/supervision-history.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.cache/tipe/supervision-history.tsv"
    else
        printf '%s\n' "/tmp/tipe/supervision-history.tsv"
    fi
}

emit_history_preference_hints() {
    [[ -n "$preedit" ]] || return 0
    command -v python3 >/dev/null 2>&1 || return 0
    local history_path line candidate count weight
    history_path=$(supervision_history_path)
    [[ -r "$history_path" ]] || return 0
    while IFS=$'\t' read -r candidate count || [[ -n "${candidate:-}" || -n "${count:-}" ]]; do
        [[ -n "${candidate:-}" && "${count:-}" =~ ^[0-9]+$ && "$count" -ge 2 ]] || continue
        is_allowed_candidate_output "$candidate" || continue
        is_prefix_only_candidate "$candidate" && continue
        if [[ "${candidates[0]:-}" == "$candidate" && "$candidate" != "$preedit" ]]; then
            continue
        fi
        weight="$count"
        if (( weight > 10 )); then
            weight=10
        fi
        printf 'candidate\t%s\n' "$candidate"
        printf 'preference\t%s\t%s\t%s\n' "$preedit" "$candidate" "$weight"
    done < <(
        TIPE_HEURISTIC_HISTORY="$history_path" TIPE_HEURISTIC_PREEDIT="$preedit" python3 - <<'PY'
import collections
import os
from pathlib import Path

path = Path(os.environ.get("TIPE_HEURISTIC_HISTORY", ""))
preedit = os.environ.get("TIPE_HEURISTIC_PREEDIT", "")
try:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
except OSError:
    raise SystemExit(0)

records = []
header = ""
body = []
for line in lines:
    if line.startswith("---\t"):
        if body:
            records.append((header, body))
        header = line
        body = []
    else:
        body.append(line)
if body:
    records.append((header, body))

counts = collections.Counter()
last_seen = {}
for index, (_header, body) in enumerate(records[-64:]):
    record_preedit = ""
    supervision_mode = ""
    selected = ""
    selected_index = ""
    candidates = []
    candidate_metadata = []
    events = []
    for line in body:
        parts = line.split("\t")
        if not parts:
            continue
        if parts[0] == "preedit" and len(parts) >= 2:
            record_preedit = parts[1]
        elif parts[0] == "candidates":
            candidates = parts[1:]
        elif parts[0] == "candidate_metadata" and len(parts) >= 4:
            candidate_metadata.append(parts[1:])
        elif parts[0] == "supervision_state":
            for part_index in range(1, len(parts) - 1, 2):
                if parts[part_index] == "mode":
                    supervision_mode = parts[part_index + 1]
        elif parts[0] == "selected_candidate" and len(parts) >= 3:
            selected_index = parts[1]
            selected = parts[2]
        elif parts[0] == "events":
            events = parts[1:]
    if supervision_mode not in {"active-preedit", "pass-through-only"}:
        supervision_mode = "active-preedit" if record_preedit else "pass-through-only"
    if supervision_mode != "active-preedit":
        continue
    prefix_only = set()
    metadata_candidates = set()
    full_consumed = set()
    if record_preedit and candidates:
        for metadata in candidate_metadata:
            if len(metadata) < 3 or metadata[1] != "consumed_prefix":
                continue
            try:
                candidate_index = int(metadata[0])
                consumed = int(metadata[2])
            except ValueError:
                continue
            if candidate_index < 0 or candidate_index >= len(candidates):
                continue
            candidate = candidates[candidate_index]
            metadata_candidates.add(candidate)
            if 0 < consumed < len(record_preedit):
                prefix_only.add(candidate)
            else:
                full_consumed.add(candidate)
        prefix_only.difference_update(full_consumed)
        if record_preedit.isalpha() and len(record_preedit) >= 8:
            for candidate in candidates:
                if (
                    candidate not in metadata_candidates
                    and not any(ch.isalnum() for ch in candidate)
                    and len(candidate) * 3 < len(record_preedit)
                ):
                    prefix_only.add(candidate)
        for candidate in candidates:
            if candidate in metadata_candidates:
                continue
            for other in candidates:
                if candidate != other and other.startswith(candidate):
                    prefix_only.add(candidate)
                    break
    explicit_non_top = selected_index.isdigit() and int(selected_index) > 0
    raw_preedit_choice = bool(record_preedit and selected == record_preedit)
    confirmed = any(
        (event.partition(":")[0] == "candidate-selected" and event.partition(":")[2] == selected)
        or (
            event.partition(":")[0] == "raw-committed"
            and selected == record_preedit
            and event.partition(":")[2] == record_preedit
        )
        for event in events
        if ":" in event
    )
    if (
        record_preedit == preedit
        and selected
        and confirmed
        and selected not in prefix_only
        and (explicit_non_top or raw_preedit_choice)
    ):
        counts[selected] += 1
        last_seen[selected] = index

for candidate, count in sorted(counts.items(), key=lambda item: (-item[1], -last_seen.get(item[0], -1), item[0]))[:5]:
    print(f"{candidate}\t{count}")
PY
    )
}

emit_history_correction_hints() {
    [[ -n "$preedit" ]] || return 0
    command -v python3 >/dev/null 2>&1 || return 0
    local history_path typo corrected count
    history_path=$(supervision_history_path)
    [[ -r "$history_path" ]] || return 0
    while IFS=$'\t' read -r typo corrected count || [[ -n "${typo:-}" || -n "${corrected:-}" || -n "${count:-}" ]]; do
        [[ -n "${typo:-}" && -n "${corrected:-}" && "${count:-}" =~ ^[0-9]+$ && "$count" -ge 2 ]] || continue
        [[ "$typo" == "$preedit" || "$corrected" == "$preedit" ]] || continue
        if is_plausible_correction "$typo" "$corrected"; then
            printf 'correction\t%s\t%s\n' "$typo" "$corrected"
        fi
    done < <(
        TIPE_HEURISTIC_HISTORY="$history_path" python3 - <<'PY'
import collections
import os
from pathlib import Path

path = Path(os.environ.get("TIPE_HEURISTIC_HISTORY", ""))
try:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
except OSError:
    raise SystemExit(0)

def plausible(typo, corrected):
    if len(typo) < 3 or len(corrected) < 3 or typo == corrected:
        return False
    if "\t" in typo or "\r" in typo or "\n" in typo:
        return False
    if "\t" in corrected or "\r" in corrected or "\n" in corrected:
        return False
    if len(corrected) == len(typo) + 1:
        return any(corrected[:skipped] + corrected[skipped + 1:] == typo for skipped in range(len(corrected)))
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

def event_kind(event):
    return event.split(":", 1)[0]

def event_text(event):
    parts = event.split(":", 1)
    return parts[1] if len(parts) == 2 else ""

def possible_corrections_from_events(items, corrected_preedit):
    items = list(items[-192:])
    if items and event_kind(items[-1]) in {"candidate-selected", "raw-committed", "escape"}:
        items.pop()
    current = ""
    cursor = 0
    erased_original = ""
    last_fully_erased = None
    last_edited_original = None
    in_preedit_edit_original = None
    erasing = False

    def remember_middle_edit():
        nonlocal in_preedit_edit_original
        if current and in_preedit_edit_original is None:
            in_preedit_edit_original = current

    for item in items:
        kind = event_kind(item)
        text = event_text(item)
        if kind in {"letter", "digit", "symbol"}:
            if erasing and erased_original:
                last_edited_original = erased_original
            erasing = False
            cursor = max(0, min(cursor, len(current)))
            if cursor < len(current):
                remember_middle_edit()
            current = current[:cursor] + text + current[cursor:]
            cursor += len(text)
        elif kind == "backspace":
            cursor = max(0, min(cursor, len(current)))
            if not current or cursor == 0:
                erasing = False
                erased_original = ""
                continue
            if cursor < len(current):
                remember_middle_edit()
            if not erasing:
                erasing = True
                erased_original = current
            current = current[:cursor - 1] + current[cursor:]
            cursor -= 1
            if not current and erased_original:
                last_fully_erased = erased_original
        elif kind == "delete":
            cursor = max(0, min(cursor, len(current)))
            if cursor >= len(current):
                continue
            remember_middle_edit()
            if not erasing:
                erasing = True
                erased_original = current
            current = current[:cursor] + current[cursor + 1:]
            if not current and erased_original:
                last_fully_erased = erased_original
        elif kind == "cursor-move":
            if text in {"Left", "KP_Left"}:
                cursor = max(0, cursor - 1)
            elif text in {"Right", "KP_Right"}:
                cursor = min(len(current), cursor + 1)
        elif kind in {"candidate-selected", "raw-committed", "escape"}:
            current = ""
            cursor = 0
            erased_original = ""
            last_fully_erased = None
            last_edited_original = None
            in_preedit_edit_original = None
            erasing = False

    if current != corrected_preedit:
        return []
    result = []
    seen = set()
    for typo in (last_fully_erased, last_edited_original, in_preedit_edit_original):
        if typo and typo not in seen and plausible(typo, corrected_preedit):
            result.append((typo, corrected_preedit))
            seen.add(typo)
    return result

records = []
header = ""
body = []
for line in lines:
    if line.startswith("---\t"):
        if body:
            records.append((header, body))
        header = line
        body = []
    else:
        body.append(line)
if body:
    records.append((header, body))

counts = collections.Counter()
last_seen = {}
for index, (_header, body) in enumerate(records[-64:]):
    record_preedit = ""
    supervision_mode = ""
    correction_events = []
    for line in body:
        parts = line.split("\t")
        if not parts:
            continue
        if parts[0] == "preedit" and len(parts) >= 2:
            record_preedit = parts[1]
        elif parts[0] == "supervision_state":
            for part_index in range(1, len(parts) - 1, 2):
                if parts[part_index] == "mode":
                    supervision_mode = parts[part_index + 1]
        elif parts[0] == "correction_events":
            correction_events = parts[1:]
    if supervision_mode not in {"active-preedit", "pass-through-only"}:
        supervision_mode = "active-preedit" if record_preedit else "pass-through-only"
    if supervision_mode != "active-preedit":
        continue
    if not record_preedit or not correction_events:
        continue
    for pair in possible_corrections_from_events(correction_events, record_preedit):
        counts[pair] += 1
        last_seen[pair] = index

for (typo, corrected), count in sorted(counts.items(), key=lambda item: (-item[1], -last_seen.get(item[0], -1), item[0][0], item[0][1]))[:8]:
    print(f"{typo}\t{corrected}\t{count}")
PY
    )
}

emit_selected_candidate_learning() {
    local selected_index selected_text extra
    [[ -n "$selected_candidate" ]] || return 0
    IFS=$'\t' read -r selected_index selected_text extra <<< "$selected_candidate"
    [[ "$selected_index" =~ ^[0-9]+$ && -n "${selected_text:-}" ]] || return 0
    if [[ "$selected_index" == "0" ]]; then
        return 0
    fi
    if is_allowed_candidate_output "$selected_text" && ! is_prefix_only_candidate "$selected_text"; then
        printf 'candidate\t%s\n' "$selected_text"
        printf 'preference\t%s\t%s\t2\n' "$preedit" "$selected_text"
    fi
}

emit_pending_segment_confirmation_learning() {
    local selected_index selected_text extra segment original consumed committed remaining corrected combined
    [[ -n "$selected_candidate" ]] || return 0
    IFS=$'\t' read -r selected_index selected_text extra <<< "$selected_candidate"
    [[ "$selected_index" =~ ^[1-9][0-9]*$ && -n "${selected_text:-}" ]] || return 0
    is_allowed_candidate_output "$selected_text" || return 0
    for segment in "${pending_segments[@]}"; do
        IFS=$'\t' read -r original consumed committed remaining extra <<< "$segment"
        [[ -n "${original:-}" && -n "${consumed:-}" && -n "${committed:-}" && "$remaining" == "$preedit" ]] || continue
        corrected="$consumed$preedit"
        combined="$committed$selected_text"
        valid_segment_chain_shape "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" || continue
        [[ "$corrected" == "$original" ]] || is_plausible_correction "$original" "$corrected" || continue
        printf 'segment_chain\t%s\t%s\t%s\t%s\t%s\t%s\t1\n' \
            "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined"
    done
}

emit_known_correction_hints() {
    local row typo corrected learned_count extra
    local best_corrected="" best_count=0 best_tied=0
    for row in "${corrections[@]}"; do
        IFS=$'\t' read -r typo corrected learned_count extra <<< "$row"
        [[ "$typo" == "$preedit" && "${learned_count:-}" =~ ^[1-9][0-9]*$ ]] || continue
        is_plausible_correction "$typo" "$corrected" || continue
        if (( learned_count > best_count )); then
            best_corrected="$corrected"
            best_count=$learned_count
            best_tied=0
        elif (( learned_count == best_count )) && [[ -n "$best_corrected" && "$corrected" != "$best_corrected" ]]; then
            best_tied=1
        fi
    done
    if [[ -n "$best_corrected" && "$best_tied" == 0 ]]; then
        printf 'correction\t%s\t%s\n' "$preedit" "$best_corrected"
    fi
}

emit_correction_pattern_hints() {
    command -v python3 >/dev/null 2>&1 || return 0
    (( ${#preedit} <= 16 )) || return 0
    TIPE_HEURISTIC_PREEDIT="$preedit" \
    TIPE_HEURISTIC_CORRECTIONS="$(printf '%s\n' "${corrections[@]}")" \
    python3 - <<'PY'
import os

preedit = os.environ.get("TIPE_HEURISTIC_PREEDIT", "")
rows = [line for line in os.environ.get("TIPE_HEURISTIC_CORRECTIONS", "").splitlines() if line]

KNOWN_ENGLISH_TOKENS = {
    "typescript", "flatpak", "github", "docker", "cursor", "openai", "python", "vscode",
    "wayland", "cargo", "cmake", "codex", "fcitx", "linux", "react", "bash",
    "javascript", "cargobuild", "cmakebuild", "hyprland", "chatgpt", "ollama", "waybar", "systemd",
    "gnome", "dbus", "build", "json", "node", "niri", "npm", "rust", "vue", "api", "gtk",
    "gpt4", "qwen2", "qwen3", "ipv4", "ipv6", "git", "gpt", "tipe",
}

def looks_like_english_identifier(text):
    if len(text) < 4 or not text.isascii():
        return False
    lowered = text.lower()
    if lowered in KNOWN_ENGLISH_TOKENS:
        return True
    if not text.isalpha():
        return False
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
    if any(marker in lowered for marker in markers):
        return True
    return any(ch in lowered for ch in "aeiouy") and lowered[-1] in "bdfjklmpqtvxz"

if looks_like_english_identifier(preedit):
    raise SystemExit(0)

def plausible(typo, corrected):
    if len(typo) < 3 or len(corrected) < 3 or typo == corrected:
        return False
    if "\t" in typo or "\r" in typo or "\n" in typo:
        return False
    if "\t" in corrected or "\r" in corrected or "\n" in corrected:
        return False
    if len(corrected) == len(typo) + 1:
        return any(corrected[:skipped] + corrected[skipped + 1:] == typo for skipped in range(len(corrected)))
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

patterns = {}

def add_pattern(kind, text, position, count, relative_to_end=False):
    key = (kind, text, position, relative_to_end)
    patterns[key] = patterns.get(key, 0) + count

for row in rows:
    fields = row.split("\t")
    if len(fields) < 3:
        continue
    typo, corrected, count_text = fields[:3]
    try:
        count = int(count_text)
    except ValueError:
        continue
    if count <= 0 or not plausible(typo, corrected):
        continue
    if len(corrected) == len(typo) + 1:
        for index, ch in enumerate(corrected):
            if corrected[:index] + corrected[index + 1:] == typo:
                offset = len(typo) - index
                if offset <= 2:
                    add_pattern("missing", ch, offset, count, True)
                else:
                    add_pattern("missing", ch, index, count)
                break
    elif len(typo) == len(corrected) + 1:
        for index, ch in enumerate(typo):
            if typo[:index] + typo[index + 1:] == corrected:
                offset = len(typo) - index - 1
                if offset <= 1:
                    add_pattern("extra", ch, offset, count, True)
                else:
                    add_pattern("extra", ch, index, count)
                break
    elif len(typo) == len(corrected):
        diffs = [(index, wrong, right) for index, (wrong, right) in enumerate(zip(typo, corrected)) if wrong != right]
        if len(diffs) == 1:
            index, wrong, right = diffs[0]
            offset = len(typo) - index - 1
            if offset <= 1:
                add_pattern("replace", wrong + "\0" + right, offset, count, True)
            else:
                add_pattern("replace", wrong + "\0" + right, index, count)
        elif (
            len(diffs) == 2
            and diffs[1][0] == diffs[0][0] + 1
            and diffs[0][1] == diffs[1][2]
            and diffs[1][1] == diffs[0][2]
        ):
            index = diffs[0][0]
            offset = len(typo) - index - 2
            text = typo[index:index + 2] + "\0" + corrected[index:index + 2]
            if offset <= 1:
                add_pattern("transpose", text, offset, count, True)
            else:
                add_pattern("transpose", text, index, count)

suggestions = {}
for (kind, text, position, relative_to_end), count in sorted(patterns.items(), key=lambda item: (-item[1], item[0][0], item[0][3], item[0][2], item[0][1])):
    if count < 2:
        continue
    corrected = ""
    apply_position = position
    if relative_to_end:
        if kind == "missing":
            if position > len(preedit):
                continue
            apply_position = len(preedit) - position
        else:
            wrong = text.split("\0", 1)[0] if kind in {"replace", "transpose"} else text
            width = len(wrong)
            if position + width > len(preedit):
                continue
            apply_position = len(preedit) - position - width
    if kind == "missing":
        if apply_position > len(preedit):
            continue
        if apply_position < len(preedit) and preedit[apply_position] == text:
            continue
        corrected = preedit[:apply_position] + text + preedit[apply_position:]
    elif kind == "extra":
        if apply_position >= len(preedit) or preedit[apply_position] != text:
            continue
        corrected = preedit[:apply_position] + preedit[apply_position + 1:]
    elif kind in {"replace", "transpose"}:
        wrong, right = text.split("\0", 1)
        if preedit[apply_position:apply_position + len(wrong)] != wrong:
            continue
        corrected = preedit[:apply_position] + right + preedit[apply_position + len(wrong):]
    if plausible(preedit, corrected):
        suggestions[corrected] = max(suggestions.get(corrected, 0), count)

ranked = sorted(suggestions.items(), key=lambda item: (-item[1], item[0]))
if ranked and (len(ranked) == 1 or ranked[0][1] > ranked[1][1]):
    print(f"correction\t{preedit}\t{ranked[0][0]}")
PY
}

run_heuristic() {
    {
        infer_correction_from_events
        if [[ -n "$preedit" ]]; then
            emit_segment_chain_hints
            emit_preference_hints
            emit_history_preference_hints
            emit_history_correction_hints
            emit_pending_segment_confirmation_learning
            emit_selected_candidate_learning
            emit_correction_pattern_hints
            emit_known_correction_hints
            for candidate in "${candidates[@]}"; do
                case "$candidate" in
                    api|bash|build|cargo|cargobuild|chatgpt|cmake|cmakebuild|codex|cursor|dbus|docker|fcitx|flatpak|git|github|gnome|gpt4|gtk|hyprland|ipv4|ipv6|javascript|json|linux|niri|node|npm|ollama|openai|python|qwen2|qwen3|react|rust|systemd|tipe|typescript|vscode|vue|waybar|wayland)
                        printf 'candidate\t%s\n' "$candidate"
                        ;;
                esac
            done
        fi
    } | awk '!seen[$0]++'
}

json_request_body() {
    TIPE_ADAPTER_PREEDIT="$preedit" \
    TIPE_ADAPTER_APPLICATION="$application" \
    TIPE_ADAPTER_SURROUNDING_BEFORE="$surrounding_before" \
    TIPE_ADAPTER_SURROUNDING_AFTER="$surrounding_after" \
    TIPE_ADAPTER_CANDIDATES="$(printf '%s\n' "${candidates[@]}")" \
    TIPE_ADAPTER_CANDIDATE_METADATA="$(printf '%s\n' "${candidate_metadata[@]}")" \
    TIPE_ADAPTER_INPUT_STATE="$input_state" \
    TIPE_ADAPTER_RUNTIME_STATE="$runtime_state" \
    TIPE_ADAPTER_SUPERVISION_STATE="$supervision_state" \
    TIPE_ADAPTER_SELECTED_CANDIDATE="$selected_candidate" \
    TIPE_ADAPTER_VISIBLE_CANDIDATES="$(printf '%s\n' "${visible_candidates[@]}")" \
    TIPE_ADAPTER_NUMBERED_CANDIDATES="$(printf '%s\n' "${numbered_candidates[@]}")" \
    TIPE_ADAPTER_EVENTS="$(printf '%s\n' "${events[@]}")" \
    TIPE_ADAPTER_EVENT_COUNTS="$(printf '%s\n' "${event_counts[@]}")" \
    TIPE_ADAPTER_CORRECTION_EVENTS="$(printf '%s\n' "${correction_events[@]}")" \
    TIPE_ADAPTER_CORRECTION_EVENT_COUNTS="$(printf '%s\n' "${correction_event_counts[@]}")" \
    TIPE_ADAPTER_CONTEXT="$(printf '%s\n' "${context[@]}")" \
    TIPE_ADAPTER_SEGMENT_CHAINS="$(printf '%s\n' "${segment_chains[@]}")" \
    TIPE_ADAPTER_PENDING_SEGMENTS="$(printf '%s\n' "${pending_segments[@]}")" \
    TIPE_ADAPTER_PREFERENCES="$(printf '%s\n' "${preferences[@]}")" \
    TIPE_ADAPTER_CORRECTIONS="$(printf '%s\n' "${corrections[@]}")" \
    TIPE_ADAPTER_MODEL="$model" \
    TIPE_ADAPTER_TEMPERATURE="$temperature" \
    TIPE_ADAPTER_MAX_TOKENS="$max_tokens" \
    TIPE_ADAPTER_SEND_RECENT_INPUT="$send_recent_input" \
    TIPE_ADAPTER_SEND_SURROUNDING="$send_surrounding" \
    python3 -c '
import collections
import json
import os
from pathlib import Path

def unescape_tipe_field(text):
    result = []
    escaped = False
    for ch in text:
        if escaped:
            if ch == "t":
                result.append("\t")
            elif ch == "r":
                result.append("\r")
            elif ch == "n":
                result.append("\n")
            elif ch == "\\":
                result.append("\\")
            else:
                result.append(ch)
            escaped = False
            continue
        if ch == "\\":
            escaped = True
            continue
        result.append(ch)
    if escaped:
        result.append("\\")
    return "".join(result)

preedit = os.environ.get("TIPE_ADAPTER_PREEDIT", "")
application = unescape_tipe_field(os.environ.get("TIPE_ADAPTER_APPLICATION", ""))
surrounding_before = unescape_tipe_field(os.environ.get("TIPE_ADAPTER_SURROUNDING_BEFORE", ""))
surrounding_after = unescape_tipe_field(os.environ.get("TIPE_ADAPTER_SURROUNDING_AFTER", ""))
candidates = [line for line in os.environ.get("TIPE_ADAPTER_CANDIDATES", "").splitlines() if line]
raw_candidate_metadata = [
    line for line in os.environ.get("TIPE_ADAPTER_CANDIDATE_METADATA", "").splitlines() if line
]
raw_input_state = os.environ.get("TIPE_ADAPTER_INPUT_STATE", "")
raw_runtime_state = os.environ.get("TIPE_ADAPTER_RUNTIME_STATE", "")
raw_supervision_state = os.environ.get("TIPE_ADAPTER_SUPERVISION_STATE", "")
raw_selected_candidate = os.environ.get("TIPE_ADAPTER_SELECTED_CANDIDATE", "")
raw_visible_candidates = [
    line for line in os.environ.get("TIPE_ADAPTER_VISIBLE_CANDIDATES", "").splitlines() if line
]
raw_numbered_candidates = [
    line for line in os.environ.get("TIPE_ADAPTER_NUMBERED_CANDIDATES", "").splitlines() if line
]
events = [unescape_tipe_field(line) for line in os.environ.get("TIPE_ADAPTER_EVENTS", "").splitlines() if line]
raw_event_counts = [
    unescape_tipe_field(line)
    for line in os.environ.get("TIPE_ADAPTER_EVENT_COUNTS", "").splitlines()
    if line
]
correction_events = [
    unescape_tipe_field(line)
    for line in os.environ.get("TIPE_ADAPTER_CORRECTION_EVENTS", "").splitlines()
    if line
]
raw_correction_event_counts = [
    unescape_tipe_field(line)
    for line in os.environ.get("TIPE_ADAPTER_CORRECTION_EVENT_COUNTS", "").splitlines()
    if line
]
context = [unescape_tipe_field(line) for line in os.environ.get("TIPE_ADAPTER_CONTEXT", "").splitlines() if line]
raw_segment_chains = [line for line in os.environ.get("TIPE_ADAPTER_SEGMENT_CHAINS", "").splitlines() if line]
raw_pending_segments = [line for line in os.environ.get("TIPE_ADAPTER_PENDING_SEGMENTS", "").splitlines() if line]
preferences = [line for line in os.environ.get("TIPE_ADAPTER_PREFERENCES", "").splitlines() if line]
corrections = [line for line in os.environ.get("TIPE_ADAPTER_CORRECTIONS", "").splitlines() if line]
send_recent_input = os.environ.get("TIPE_ADAPTER_SEND_RECENT_INPUT", "0") == "1"
send_surrounding = os.environ.get("TIPE_ADAPTER_SEND_SURROUNDING", "0") == "1"

if not send_recent_input:
    events = []
    raw_event_counts = []
    correction_events = []
    raw_correction_event_counts = []
    context = []
    raw_segment_chains = []
    raw_pending_segments = []
    preferences = []
    corrections = []
if not send_surrounding:
    application = ""
    surrounding_before = ""
    surrounding_after = ""
model = os.environ.get("TIPE_ADAPTER_MODEL", "qwen2.5:0.5b")
try:
    temperature = float(os.environ.get("TIPE_ADAPTER_TEMPERATURE", "0"))
except ValueError:
    temperature = 0
try:
    max_tokens = int(os.environ.get("TIPE_ADAPTER_MAX_TOKENS", "128"))
except ValueError:
    max_tokens = 128
max_tokens = min(max(max_tokens, 1), 4096)

input_state = {}
state_fields = raw_input_state.split("\t") if raw_input_state else []
for index in range(0, len(state_fields) - 1, 2):
    key = state_fields[index]
    value = state_fields[index + 1]
    if key in {"preedit_cursor", "candidate_cursor"}:
        try:
            input_state[key] = int(value)
        except ValueError:
            continue
    elif key == "expanded":
        input_state[key] = value == "1"

runtime_state = {}
runtime_fields = raw_runtime_state.split("\t") if raw_runtime_state else []
for index in range(0, len(runtime_fields) - 1, 2):
    key = runtime_fields[index]
    value = runtime_fields[index + 1]
    if key == "continuous":
        runtime_state[key] = value == "1"
    elif key == "input_mode" and value in {"chinese", "english"}:
        runtime_state[key] = value

supervision_state = {}
supervision_fields = raw_supervision_state.split("\t") if raw_supervision_state else []
for index in range(0, len(supervision_fields) - 1, 2):
    key = supervision_fields[index]
    value = supervision_fields[index + 1]
    if key in {"active_preedit"}:
        supervision_state[key] = value == "1"
    elif key in {"recent_events", "correction_events"}:
        try:
            supervision_state[key] = int(value)
        except ValueError:
            continue
    else:
        supervision_state[key] = value

candidate_metadata = []
for item in raw_candidate_metadata:
    fields = item.split("\t")
    if len(fields) < 7:
        continue
    try:
        candidate_index = int(fields[0])
        consumed_prefix = int(fields[2]) if fields[1] == "consumed_prefix" else 0
        source = unescape_tipe_field(fields[4]) if fields[3] == "source" else ""
        score = int(fields[6]) if fields[5] == "score" else 0
    except ValueError:
        continue
    candidate_metadata.append({
        "index": candidate_index,
        "text": candidates[candidate_index] if 0 <= candidate_index < len(candidates) else "",
        "consumed_prefix": consumed_prefix,
        "source": source,
        "score": score,
    })

selected_candidate = None
if raw_selected_candidate:
    selected_fields = raw_selected_candidate.split("\t", 1)
    if len(selected_fields) == 2:
        try:
            selected_candidate = {
                "index": int(selected_fields[0]),
                "text": selected_fields[1],
            }
        except ValueError:
            selected_candidate = None

visible_candidates = []
for item in raw_visible_candidates:
    index_text = item.split(":", 1)
    if len(index_text) != 2:
        continue
    try:
        visible_candidates.append({"index": int(index_text[0]), "text": index_text[1]})
    except ValueError:
        continue

numbered_candidates = []
for item in raw_numbered_candidates:
    shortcut_index_text = item.split(":", 2)
    if len(shortcut_index_text) != 3:
        continue
    try:
        numbered_candidates.append({
            "shortcut": shortcut_index_text[0],
            "index": int(shortcut_index_text[1]),
            "text": shortcut_index_text[2],
        })
    except ValueError:
        continue

segment_chains = []
for item in raw_segment_chains:
    fields = item.split("\t")
    if len(fields) != 6:
        continue
    segment_chains.append({
        "original_preedit": unescape_tipe_field(fields[0]),
        "consumed_preedit": unescape_tipe_field(fields[1]),
        "committed_text": unescape_tipe_field(fields[2]),
        "remaining_preedit": unescape_tipe_field(fields[3]),
        "corrected_full_preedit": unescape_tipe_field(fields[4]),
        "combined_candidate": unescape_tipe_field(fields[5]),
    })

pending_segments = []
for item in raw_pending_segments:
    fields = item.split("\t")
    if len(fields) != 4:
        continue
    pending_segments.append({
        "original_preedit": unescape_tipe_field(fields[0]),
        "consumed_preedit": unescape_tipe_field(fields[1]),
        "committed_text": unescape_tipe_field(fields[2]),
        "remaining_preedit": unescape_tipe_field(fields[3]),
    })

if not preedit:
    preferences = []
    corrections = []
    segment_chains = []
    pending_segments = []

def event_kind(event):
    return event.split(":", 1)[0]

def event_text(event):
    parts = event.split(":", 1)
    return parts[1] if len(parts) == 2 else ""

def event_counts(items):
    counts = {}
    for item in items:
        kind = event_kind(item)
        counts[kind] = counts.get(kind, 0) + 1
    return counts

def declared_event_counts(items):
    counts = {}
    for item in items:
        kind, separator, count = item.partition(":")
        if not separator or not kind:
            continue
        try:
            parsed_count = int(count)
        except ValueError:
            continue
        if parsed_count < 0:
            continue
        counts[kind] = parsed_count
    return counts

def preedit_leading_context(items, has_preedit):
    if not has_preedit:
        leading = list(items[-48:])
    else:
        leading = []
        for item in items[-48:]:
            kind = event_kind(item)
            if kind in {"letter", "digit", "symbol", "rerank-requested"}:
                break
            leading.append(item)
    return {
        "active": bool(leading),
        "events": leading,
        "event_counts": event_counts(leading),
        "events_before_preedit": len(leading),
        "meaning": (
            "all supervised pass-through keys because no preedit is active"
            if not has_preedit
            else "supervised pass-through keys observed immediately before the current preedit"
        ),
    }

def plausible_correction(typo, corrected):
    if len(typo) < 3 or len(corrected) < 3 or typo == corrected:
        return False
    if len(corrected) == len(typo) + 1:
        return any(corrected[:skipped] + corrected[skipped + 1:] == typo for skipped in range(len(corrected)))
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

def valid_segment_chain(chain):
    original = chain.get("original_preedit", "")
    consumed = chain.get("consumed_preedit", "")
    committed = chain.get("committed_text", "")
    remaining = chain.get("remaining_preedit", "")
    corrected = chain.get("corrected_full_preedit", "")
    combined = chain.get("combined_candidate", "")
    return (
        combined.startswith(committed)
        and (consumed + remaining == original or consumed + remaining == corrected)
        and (corrected == original or plausible_correction(original, corrected))
    )

segment_chains = [chain for chain in segment_chains if valid_segment_chain(chain)]

def possible_corrections_from_events(items, corrected_preedit):
    items = list(items[-192:])
    if items and event_kind(items[-1]) in {"candidate-selected", "raw-committed", "escape"}:
        items.pop()
    current = ""
    cursor = 0
    erased_original = ""
    last_fully_erased = None
    last_edited_original = None
    in_preedit_edit_original = None
    erasing = False

    def remember_middle_edit():
        nonlocal in_preedit_edit_original
        if current and in_preedit_edit_original is None:
            in_preedit_edit_original = current

    for item in items:
        kind = event_kind(item)
        text = event_text(item)
        if kind in {"letter", "digit", "symbol"}:
            if erasing and erased_original:
                last_edited_original = erased_original
            erasing = False
            cursor = max(0, min(cursor, len(current)))
            if cursor < len(current):
                remember_middle_edit()
            current = current[:cursor] + text + current[cursor:]
            cursor += len(text)
        elif kind == "backspace":
            cursor = max(0, min(cursor, len(current)))
            if not current or cursor == 0:
                erasing = False
                erased_original = ""
                continue
            if cursor < len(current):
                remember_middle_edit()
            if not erasing:
                erasing = True
                erased_original = current
            current = current[:cursor - 1] + current[cursor:]
            cursor -= 1
            if not current and erased_original:
                last_fully_erased = erased_original
        elif kind == "delete":
            cursor = max(0, min(cursor, len(current)))
            if cursor >= len(current):
                continue
            remember_middle_edit()
            if not erasing:
                erasing = True
                erased_original = current
            current = current[:cursor] + current[cursor + 1:]
            if not current and erased_original:
                last_fully_erased = erased_original
        elif kind == "cursor-move":
            if text in {"Left", "KP_Left"}:
                cursor = max(0, cursor - 1)
            elif text in {"Right", "KP_Right"}:
                cursor = min(len(current), cursor + 1)
        elif kind in {"candidate-selected", "raw-committed", "escape"}:
            current = ""
            cursor = 0
            erased_original = ""
            last_fully_erased = None
            last_edited_original = None
            in_preedit_edit_original = None
            erasing = False

    if current != corrected_preedit:
        return []
    result = []
    seen = set()
    for source, typo in (
        ("full-delete-retype", last_fully_erased),
        ("partial-rewrite", last_edited_original),
        ("middle-edit", in_preedit_edit_original),
    ):
        if typo and typo not in seen and plausible_correction(typo, corrected_preedit):
            result.append({"source": source, "typo": typo, "corrected_preedit": corrected_preedit})
            seen.add(typo)
    return result

def edit_summary_from_events(items):
    current = ""
    cursor = 0
    erased_original = ""
    last_fully_erased = ""
    last_edited_original = ""
    middle_edit_original = ""
    typed_tail = ""
    erasing = False

    def remember_middle_edit():
        nonlocal middle_edit_original
        if current and not middle_edit_original:
            middle_edit_original = current

    for item in items[-192:]:
        kind = event_kind(item)
        text = event_text(item)
        if kind in {"letter", "digit", "symbol"}:
            if erasing and erased_original:
                last_edited_original = erased_original
            erasing = False
            cursor = max(0, min(cursor, len(current)))
            if cursor < len(current):
                remember_middle_edit()
            current = current[:cursor] + text + current[cursor:]
            cursor += len(text)
            typed_tail = (typed_tail + text)[-32:]
        elif kind == "backspace":
            cursor = max(0, min(cursor, len(current)))
            typed_tail = ""
            if not current or cursor == 0:
                erasing = False
                erased_original = ""
                continue
            if cursor < len(current):
                remember_middle_edit()
            if not erasing:
                erasing = True
                erased_original = current
            current = current[:cursor - 1] + current[cursor:]
            cursor -= 1
            if not current and erased_original:
                last_fully_erased = erased_original
        elif kind == "delete":
            cursor = max(0, min(cursor, len(current)))
            typed_tail = ""
            if cursor >= len(current):
                continue
            remember_middle_edit()
            if not erasing:
                erasing = True
                erased_original = current
            current = current[:cursor] + current[cursor + 1:]
            if not current and erased_original:
                last_fully_erased = erased_original
        elif kind == "cursor-move":
            typed_tail = ""
            if text in {"Left", "KP_Left"}:
                cursor = max(0, cursor - 1)
            elif text in {"Right", "KP_Right"}:
                cursor = min(len(current), cursor + 1)
        elif kind in {"candidate-selected", "raw-committed", "escape"}:
            current = ""
            cursor = 0
            erased_original = ""
            last_fully_erased = ""
            last_edited_original = ""
            middle_edit_original = ""
            typed_tail = ""
            erasing = False

    return {
        "current": current,
        "cursor": cursor,
        "typed_tail": typed_tail,
        "last_fully_erased": last_fully_erased,
        "last_edited_original": last_edited_original,
        "middle_edit_original": middle_edit_original,
    }

def correction_patterns_from_rows(rows):
    patterns = {}
    def add_pattern(kind, text, position, count, relative_to_end=False):
        key = (kind, text, position, relative_to_end)
        patterns[key] = patterns.get(key, 0) + count

    for row in rows:
        fields = row.split("\t")
        if len(fields) < 3:
            continue
        typo, corrected, count_text = fields[:3]
        try:
            count = int(count_text)
        except ValueError:
            continue
        if count <= 0 or not typo or not corrected or typo == corrected:
            continue
        if len(corrected) == len(typo) + 1:
            for index, ch in enumerate(corrected):
                if corrected[:index] + corrected[index + 1:] == typo:
                    offset = len(typo) - index
                    if offset <= 2:
                        add_pattern("missing", ch, offset, count, True)
                    else:
                        add_pattern("missing", ch, index, count)
                    break
        elif len(typo) == len(corrected) + 1:
            for index, ch in enumerate(typo):
                if typo[:index] + typo[index + 1:] == corrected:
                    offset = len(typo) - index - 1
                    if offset <= 1:
                        add_pattern("extra", ch, offset, count, True)
                    else:
                        add_pattern("extra", ch, index, count)
                    break
        elif len(typo) == len(corrected):
            diffs = [(index, wrong, right) for index, (wrong, right) in enumerate(zip(typo, corrected)) if wrong != right]
            if len(diffs) == 1:
                index, wrong, right = diffs[0]
                offset = len(typo) - index - 1
                if offset <= 1:
                    add_pattern("replace", f"{wrong}->{right}", offset, count, True)
                else:
                    add_pattern("replace", f"{wrong}->{right}", index, count)
            elif (
                len(diffs) == 2
                and diffs[1][0] == diffs[0][0] + 1
                and diffs[0][1] == diffs[1][2]
                and diffs[1][1] == diffs[0][2]
            ):
                index = diffs[0][0]
                offset = len(typo) - index - 2
                text = f"{typo[index:index + 2]}->{corrected[index:index + 2]}"
                if offset <= 1:
                    add_pattern("transpose", text, offset, count, True)
                else:
                    add_pattern("transpose", text, index, count)
    ranked = sorted(patterns.items(), key=lambda item: (-item[1], item[0][0], item[0][3], item[0][2], item[0][1]))
    return [
        {"kind": kind, "text": text, "position": position, "relative_to_end": relative_to_end, "count": count}
        for (kind, text, position, relative_to_end), count in ranked[:5]
    ]

KNOWN_ENGLISH_TOKENS = {
    "typescript", "flatpak", "github", "docker", "cursor", "openai", "python", "vscode",
    "wayland", "cargo", "cmake", "codex", "fcitx", "linux", "react", "bash",
    "javascript", "cargobuild", "cmakebuild", "hyprland", "chatgpt", "ollama", "waybar", "systemd",
    "gnome", "dbus", "build", "json", "node", "niri", "npm", "rust",
    "vue", "api", "gtk", "gpt4", "qwen2", "qwen3", "ipv4", "ipv6",
    "git", "gpt", "tipe",
}

def lower_ascii(text):
    return "".join(ch.lower() if "A" <= ch <= "Z" else ch for ch in text)

def is_code_or_terminal_application(application):
    lowered = lower_ascii(application)
    return any(
        needle in lowered
        for needle in (
            "alacritty", "code", "codium", "cursor", "emacs", "ghostty", "jetbrains",
            "kitty", "konsole", "neovide", "terminal", "vscode", "wezterm", "xterm",
        )
    )

def looks_like_english_identifier(preedit):
    if len(preedit) < 4 or not preedit.isascii():
        return False
    lowered = lower_ascii(preedit)
    if lowered in KNOWN_ENGLISH_TOKENS:
        return True
    if not preedit.isalpha():
        return False
    for index, ch in enumerate(lowered):
        next_ch = lowered[index + 1] if index + 1 < len(lowered) else ""
        prev_ch = lowered[index - 1] if index > 0 else ""
        if ch == "v" and prev_ch not in ("l", "n"):
            return True
        if ch == "x" and next_ch not in ("i", "u"):
            return True
        if ch == "q" and next_ch not in ("i", "u"):
            return True
    if any(
        marker in lowered
        for marker in (
            "ck", "cl", "cr", "ct", "dr", "ea", "ee", "fl", "ft", "gr", "ld", "ll",
            "lt", "mp", "nd", "nt", "oo", "ph", "pl", "pr", "pt", "rb", "rd", "rk",
            "rn", "rs", "rt", "sk", "sl", "sm", "sn", "sp", "ss", "st", "sv", "sw",
            "th", "tr", "ts", "tw", "xt",
        )
    ):
        return True
    return any(ch in lowered for ch in "aeiouy") and lowered[-1] in "bdfjklmpqtvxz"

def looks_like_learned_raw_identifier(preedit):
    if looks_like_english_identifier(preedit):
        return True
    if len(preedit) < 2 or not preedit.isascii() or not preedit.isalpha():
        return False
    lowered = lower_ascii(preedit)
    if lowered in {"er", "lv", "nv", "lve", "nve"}:
        return False
    if lowered.endswith("g") and not lowered.endswith("ng"):
        return True
    return lowered[-1] in "bcdfhjklmpqrstvwxyz"

def looks_like_code_surrounding(before, after):
    nearby = before[-96:] + after[:32]
    if not nearby:
        return False
    lowered = lower_ascii(nearby)
    if any(
        marker in lowered
        for marker in (
            "://", "::", "->", "=>", "./", "../", "/home/", "/usr/", " --", " -",
            "git ", "npm ", "cargo ", "cmake ", "python ", "docker ", "const ",
            "let ", "var ", "fn ", "def ", "class ", "import ", "export ", "return ",
            "std::", "#include",
        )
    ):
        return True
    identifier_bytes = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./:-")
    return (bool(before) and before[-1] in identifier_bytes) or (bool(after) and after[0] in identifier_bytes)

def raw_preference_count(preedit, preferences):
    strongest = 0
    if not preedit:
        return strongest
    for row in preferences:
        fields = row.split("\t")
        if len(fields) < 3:
            continue
        learned_preedit, learned_candidate, count_text = fields[:3]
        if learned_preedit != preedit or learned_candidate != preedit:
            continue
        try:
            count = int(count_text)
        except ValueError:
            continue
        strongest = max(strongest, count)
    return strongest

def raw_english_hint(preedit, application, surrounding_before, surrounding_after, preferences):
    _ = (application, surrounding_before, surrounding_after)
    learned_count = raw_preference_count(preedit, preferences)
    if learned_count >= 3:
        return {"active": True, "source": "learned-raw-preference", "count": learned_count}
    if preedit and lower_ascii(preedit) in KNOWN_ENGLISH_TOKENS:
        return {"active": True, "source": "known-english-token", "count": 0}
    return {"active": False, "source": "none", "count": learned_count}

def prefix_only_candidates(preedit, candidates, candidate_metadata):
    prefix_only = set()
    metadata_candidates = set()
    full_consumed = set()
    for item in candidate_metadata:
        try:
            index = item.get("index")
            consumed_prefix = item.get("consumed_prefix", 0)
        except AttributeError:
            continue
        if not isinstance(index, int) or not isinstance(consumed_prefix, int) or not 0 <= index < len(candidates):
            continue
        candidate = candidates[index]
        metadata_candidates.add(candidate)
        if 0 < consumed_prefix < len(preedit):
            prefix_only.add(candidate)
        else:
            full_consumed.add(candidate)
    prefix_only.difference_update(full_consumed)
    if preedit.isascii() and preedit.isalpha() and len(preedit) >= 8:
        for candidate in candidates:
            if candidate in metadata_candidates:
                continue
            if candidate and not any(ch.isascii() and ch.isalnum() for ch in candidate) and len(candidate) * 3 < len(preedit):
                prefix_only.add(candidate)
    for candidate in candidates:
        if (
            candidate
            and candidate not in metadata_candidates
            and any(other != candidate and other.startswith(candidate) for other in candidates)
        ):
            prefix_only.add(candidate)
    return prefix_only

def known_correction_pairs(corrections):
    result = set()
    for row in corrections:
        fields = row.split("\t")
        if len(fields) >= 2:
            result.add((fields[0], fields[1]))
    return result

def current_learning_signals(preedit, candidates, candidate_metadata, selected_candidate, segment_chains, pending_segments, corrections):
    signals = []
    candidate_set = set(candidates)
    prefix_only = prefix_only_candidates(preedit, candidates, candidate_metadata)
    known_corrections = known_correction_pairs(corrections)
    selected_text = ""
    selected_index = 0
    if selected_candidate:
        selected_text = selected_candidate.get("text", "")
        selected_index = selected_candidate.get("index", 0)
        if selected_text and selected_text in candidate_set:
            if selected_index == 0 or (candidates and selected_text == candidates[0]):
                signals.append({
                    "kind": "selected_candidate",
                    "status": "already_top",
                    "preedit": preedit,
                    "candidate": selected_text,
                    "index": selected_index,
                })
            elif selected_text in prefix_only:
                signals.append({
                    "kind": "selected_candidate",
                    "status": "prefix_only_no_preference",
                    "preedit": preedit,
                    "candidate": selected_text,
                    "index": selected_index,
                    "meaning": "selected candidate commits only a prefix; learn a segment_chain after the suffix is confirmed, not a full-preedit preference",
                })
            else:
                signals.append({
                    "kind": "selected_candidate",
                    "status": "would_learn_preference",
                    "preedit": preedit,
                    "candidate": selected_text,
                    "index": selected_index,
                    "suggested_protocol": f"preference\t{preedit}\t{selected_text}\t2",
                })
    for chain in segment_chains[-16:]:
        if chain.get("original_preedit") != preedit:
            continue
        combined = chain.get("combined_candidate", "")
        if combined not in candidate_set:
            continue
        original = chain.get("original_preedit", "")
        corrected = chain.get("corrected_full_preedit", "")
        consumed = chain.get("consumed_preedit", "")
        committed = chain.get("committed_text", "")
        remaining = chain.get("remaining_preedit", "")
        status = "correction_chain" if corrected and corrected != original else "continuation"
        signal = {
            "kind": "segment_chain",
            "status": status,
            "original_preedit": original,
            "consumed_preedit": consumed,
            "committed_text": committed,
            "remaining_preedit": remaining,
            "corrected_full_preedit": corrected,
            "combined_candidate": combined,
            "evidence_protocol": (
                "segment_chain\t"
                f"{original}\t{consumed}\t{committed}\t{remaining}\t{corrected}\t{combined}\t1"
            ),
        }
        if status == "correction_chain" and (original, corrected) not in known_corrections:
            signal["suggested_correction_protocol"] = f"correction\t{original}\t{corrected}"
        signals.append(signal)
    for segment in pending_segments[-8:]:
        if segment.get("remaining_preedit", "") != preedit:
            continue
        if selected_text and selected_text in candidate_set and selected_index > 0:
            original = segment.get("original_preedit", "")
            consumed = segment.get("consumed_preedit", "")
            committed = segment.get("committed_text", "")
            remaining = segment.get("remaining_preedit", "")
            corrected = consumed + preedit
            combined = committed + selected_text
            chain = {
                "original_preedit": original,
                "consumed_preedit": consumed,
                "committed_text": committed,
                "remaining_preedit": remaining,
                "corrected_full_preedit": corrected,
                "combined_candidate": combined,
            }
            if valid_segment_chain(chain):
                signal = {
                    "kind": "pending_segment",
                    "status": "confirmed_suffix",
                    "original_preedit": original,
                    "consumed_preedit": consumed,
                    "committed_text": committed,
                    "remaining_preedit": remaining,
                    "suffix_candidate": selected_text,
                    "selected_index": selected_index,
                    "corrected_full_preedit": corrected,
                    "combined_candidate": combined,
                    "suggested_protocol": (
                        "segment_chain\t"
                        f"{original}\t{consumed}\t{committed}\t{remaining}\t{corrected}\t{combined}\t1"
                    ),
                }
                if corrected != original:
                    signal["suggested_correction_protocol"] = f"correction\t{original}\t{corrected}"
                signals.append(signal)
                continue
        signals.append({
            "kind": "pending_segment",
            "status": "awaiting_suffix_confirmation",
            "original_preedit": segment.get("original_preedit", ""),
            "consumed_preedit": segment.get("consumed_preedit", ""),
            "committed_text": segment.get("committed_text", ""),
            "remaining_preedit": segment.get("remaining_preedit", ""),
            "meaning": "prefix was selected from a longer preedit; wait for the suffix candidate before outputting a segment_chain row",
        })
    return signals

def supervised_learning_signals(preedit, candidates, candidate_metadata, selected_candidate, segment_chains, pending_segments, possible_corrections, corrections, history_summary):
    signals = list(current_learning_signals(preedit, candidates, candidate_metadata, selected_candidate, segment_chains, pending_segments, corrections))
    known_corrections = known_correction_pairs(corrections)
    candidate_set = set(candidates)
    prefix_only = prefix_only_candidates(preedit, candidates, candidate_metadata)
    seen = {
        (
            item.get("kind", ""),
            item.get("preedit", item.get("original_preedit", "")),
            item.get("candidate", item.get("combined_candidate", "")),
            item.get("corrected_preedit", item.get("corrected_full_preedit", "")),
        )
        for item in signals
    }

    for row in history_summary.get("top_preedit_selected_pairs", [])[:8]:
        history_preedit = row.get("preedit", "")
        candidate = row.get("candidate", "")
        count = row.get("count", 0)
        if history_preedit != preedit or not candidate:
            continue
        try:
            count = int(count)
        except (TypeError, ValueError):
            count = 0
        if count < 2:
            continue
        if candidate in prefix_only:
            continue
        if candidate not in candidate_set and not (candidate == preedit and looks_like_learned_raw_identifier(preedit)):
            continue
        key = ("history_preference", preedit, candidate, "")
        if key in seen:
            continue
        weight = min(max(count, 2), 10)
        signals.append({
            "kind": "history_preference",
            "status": "repeated_supervised_history",
            "preedit": preedit,
            "candidate": candidate,
            "count": count,
            "suggested_protocol": f"preference\t{preedit}\t{candidate}\t{weight}",
        })
        seen.add(key)

    for correction in possible_corrections[-8:]:
        typo = correction.get("typo", "")
        corrected = correction.get("corrected_preedit", "")
        key = ("possible_correction", typo, "", corrected)
        if not typo or not corrected or key in seen:
            continue
        signals.append({
            "kind": "possible_correction",
            "status": "supervised_evidence",
            "source": correction.get("source", ""),
            "typo": typo,
            "corrected_preedit": corrected,
            "suggested_protocol": f"correction\t{typo}\t{corrected}",
        })
        seen.add(key)

    for chain in segment_chains[-16:]:
        original = chain.get("original_preedit", "")
        consumed = chain.get("consumed_preedit", "")
        committed = chain.get("committed_text", "")
        remaining = chain.get("remaining_preedit", "")
        corrected = chain.get("corrected_full_preedit", "")
        combined = chain.get("combined_candidate", "")
        if not original or not combined:
            continue
        key = ("segment_chain", original, combined, corrected)
        if key in seen:
            continue
        status = "current_request" if original == preedit and combined in set(candidates) else "recent_supervised"
        signal = {
            "kind": "segment_chain",
            "status": status,
            "original_preedit": original,
            "consumed_preedit": consumed,
            "committed_text": committed,
            "remaining_preedit": remaining,
            "corrected_full_preedit": corrected,
            "combined_candidate": combined,
            "evidence_protocol": (
                "segment_chain\t"
                f"{original}\t{consumed}\t{committed}\t{remaining}\t{corrected}\t{combined}\t1"
            ),
        }
        if corrected and corrected != original and (original, corrected) not in known_corrections:
            signal["suggested_correction_protocol"] = f"correction\t{original}\t{corrected}"
        signals.append(signal)
        seen.add(key)

    for row in corrections[:16]:
        fields = row.split("\t")
        if len(fields) < 3:
            continue
        typo, corrected, count_text = fields[:3]
        if not typo or not corrected or not plausible_correction(typo, corrected):
            continue
        if typo != preedit and corrected != preedit:
            continue
        key = ("known_correction", typo, "", corrected)
        if key in seen:
            continue
        try:
            count = int(count_text)
        except ValueError:
            count = 0
        signal = {
            "kind": "known_correction",
            "status": "current_typo" if typo == preedit else "current_corrected_preedit",
            "typo": typo,
            "corrected_preedit": corrected,
            "count": count,
            "evidence_protocol": f"correction\t{typo}\t{corrected}",
        }
        signals.append(signal)
        seen.add(key)
    for row in history_summary.get("top_corrections", [])[:8]:
        typo = row.get("typo", "")
        corrected = row.get("corrected_preedit", "")
        if not typo or not corrected or (typo != preedit and corrected != preedit) or not plausible_correction(typo, corrected):
            continue
        key = ("history_correction", typo, "", corrected)
        if key in seen:
            continue
        signal = {
            "kind": "history_correction",
            "status": "current_typo" if typo == preedit else "current_corrected_preedit",
            "typo": typo,
            "corrected_preedit": corrected,
            "count": row.get("count", 0),
            "suggested_protocol": f"correction\t{typo}\t{corrected}",
        }
        signals.append(signal)
        seen.add(key)
    return signals[:24]

def learning_status(supervision_mode, preedit, candidates, selected_candidate, signals, history_summary):
    kind_counts = collections.Counter(item.get("kind", "") for item in signals if item.get("kind"))
    status_counts = collections.Counter(item.get("status", "") for item in signals if item.get("status"))
    suggested_protocols = []
    for item in signals:
        protocol = item.get("suggested_protocol", "")
        if protocol and protocol not in suggested_protocols:
            suggested_protocols.append(protocol)
        correction_protocol = item.get("suggested_correction_protocol", "")
        if correction_protocol and correction_protocol not in suggested_protocols:
            suggested_protocols.append(correction_protocol)
    awaiting_suffix = [
        {
            "original_preedit": item.get("original_preedit", ""),
            "consumed_preedit": item.get("consumed_preedit", ""),
            "committed_text": item.get("committed_text", ""),
            "remaining_preedit": item.get("remaining_preedit", ""),
        }
        for item in signals
        if item.get("kind") == "pending_segment" and item.get("status") == "awaiting_suffix_confirmation"
    ]
    selected_text = selected_candidate.get("text", "") if selected_candidate else ""
    if supervision_mode == "pass-through-only":
        primary = "keyboard-context-only"
        next_step = "wait for an active preedit before emitting candidates or learning rows"
    elif suggested_protocols:
        primary = "ready-to-learn"
        next_step = "prefer suggested_protocol rows that match the current active preedit and visible UI state"
    elif awaiting_suffix:
        primary = "awaiting-suffix-confirmation"
        next_step = "wait until the remaining preedit has an explicitly selected suffix candidate"
    elif selected_text and candidates and selected_text == candidates[0]:
        primary = "selected-candidate-already-top"
        next_step = "no persistent preference is needed for the current selected candidate"
    elif preedit and candidates:
        primary = "rank-only"
        next_step = "rerank only if evidence is stronger than the current candidate order"
    else:
        primary = "no-action"
        next_step = "do not emit output"
    return {
        "mode": supervision_mode,
        "primary": primary,
        "preedit": preedit,
        "candidate_count": len(candidates),
        "selected_candidate": selected_candidate,
        "signal_counts": dict(kind_counts),
        "status_counts": dict(status_counts),
        "suggested_protocols": suggested_protocols[:8],
        "awaiting_suffix": awaiting_suffix[:4],
        "history_available": bool(history_summary.get("available")),
        "history_records": int(history_summary.get("records", 0) or 0),
        "next_step": next_step,
    }

def realtime_correction_decisions(preedit, patterns):
    guard = ""
    if len(preedit) > 16:
        guard = "long-preedit"
    elif preedit in KNOWN_ENGLISH_TOKENS:
        guard = "known-raw-english"
    elif looks_like_english_identifier(preedit):
        guard = "english-identifier"

    decisions = []
    for pattern in patterns:
        kind = pattern["kind"]
        text = pattern["text"]
        position = pattern["position"]
        relative_to_end = pattern.get("relative_to_end", False)
        count = pattern["count"]
        status = "guarded" if guard else "skipped"
        reason = guard or "not-applicable"
        corrected = ""
        apply_position = position
        if relative_to_end:
            if kind == "missing":
                if position > len(preedit):
                    apply_position = None
                else:
                    apply_position = len(preedit) - position
            else:
                wrong = text.partition("->")[0] if kind in {"replace", "transpose"} else text
                width = len(wrong)
                if position + width > len(preedit):
                    apply_position = None
                else:
                    apply_position = len(preedit) - position - width
        if count < 2:
            reason = "weak-pattern"
        elif guard:
            pass
        elif kind == "missing":
            if apply_position is None:
                reason = "out-of-range"
            elif apply_position > len(preedit):
                reason = "out-of-range"
            elif apply_position < len(preedit) and preedit[apply_position:apply_position + len(text)] == text:
                reason = "already-present"
            else:
                corrected = preedit[:apply_position] + text + preedit[apply_position:]
        elif kind == "extra":
            if apply_position is None:
                reason = "out-of-range"
            elif apply_position >= len(preedit) or preedit[apply_position:apply_position + len(text)] != text:
                reason = "not-present"
            else:
                corrected = preedit[:apply_position] + preedit[apply_position + len(text):]
        elif kind in {"replace", "transpose"}:
            wrong, separator, right = text.partition("->")
            if not separator:
                reason = "invalid-pattern"
            elif apply_position is None:
                reason = "out-of-range"
            elif preedit[apply_position:apply_position + len(wrong)] != wrong:
                reason = "not-present"
            else:
                corrected = preedit[:apply_position] + right + preedit[apply_position + len(wrong):]
        if corrected:
            if plausible_correction(preedit, corrected):
                status = "applied"
                reason = "ok"
            else:
                corrected = ""
                status = "skipped"
                reason = "implausible"
        decisions.append({
            "kind": kind,
            "text": text,
            "position": position,
            "relative_to_end": relative_to_end,
            "count": count,
            "status": status,
            "reason": reason,
            "corrected_preedit": corrected,
        })
    return decisions

def default_history_path():
    cache_home = os.environ.get("XDG_CACHE_HOME")
    if cache_home:
        return Path(cache_home) / "tipe" / "supervision-history.tsv"
    home = os.environ.get("HOME")
    if home:
        return Path(home) / ".cache" / "tipe" / "supervision-history.tsv"
    return Path("/tmp/tipe/supervision-history.tsv")

def recent_history_summary():
    configured = os.environ.get("TIPE_SUPERVISION_HISTORY")
    history_path = Path(configured) if configured else default_history_path()
    try:
        lines = history_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return {
            "available": False,
            "path": str(history_path),
            "records": 0,
            "active_preedit_records": 0,
            "pass_through_records": 0,
            "top_preedits": [],
            "top_selected_candidates": [],
            "top_preedit_selected_pairs": [],
            "top_applications": [],
            "event_counts": {},
            "active_event_counts": {},
            "pass_through_event_counts": {},
            "correction_event_counts": {},
            "top_corrections": [],
            "segment_chain_count": 0,
            "pending_segment_count": 0,
        }

    records = []
    header = ""
    body = []
    for line in lines:
        if line.startswith("---\t"):
            if body:
                records.append((header, body))
            header = line
            body = []
        else:
            body.append(line)
    if body:
        records.append((header, body))
    records = records[-24:]

    parsed = []
    for header, body in records:
        fields = {}
        header_fields = header.split("\t")
        for index in range(1, len(header_fields) - 1, 2):
            fields[f"header_{header_fields[index]}"] = header_fields[index + 1]
        for line in body:
            parts = line.split("\t")
            if not parts:
                continue
            kind = parts[0]
            if kind == "preedit" and len(parts) >= 2:
                fields["preedit"] = parts[1]
            elif kind == "application" and len(parts) >= 2:
                fields["application"] = unescape_tipe_field(parts[1])
            elif kind == "candidates":
                fields["candidates"] = parts[1:]
            elif kind == "candidate_metadata" and len(parts) >= 4:
                fields.setdefault("candidate_metadata", []).append(parts[1:])
            elif kind == "selected_candidate" and len(parts) >= 3:
                fields["selected_candidate_index"] = parts[1]
                fields["selected_candidate"] = parts[2]
            elif kind == "event_counts":
                fields.setdefault("event_counts", []).extend(parts[1:])
            elif kind == "events":
                fields["events"] = parts[1:]
            elif kind == "correction_event_counts":
                fields.setdefault("correction_event_counts", []).extend(parts[1:])
            elif kind == "correction_events":
                fields["correction_events"] = parts[1:]
            elif kind == "supervision_state":
                for part_index in range(1, len(parts) - 1, 2):
                    if parts[part_index] == "mode":
                        fields["supervision_mode"] = parts[part_index + 1]
            elif kind == "segment_chain":
                fields["segment_chain_count"] = str(int(fields.get("segment_chain_count", "0")) + 1)
            elif kind == "pending_segment":
                fields["pending_segment_count"] = str(int(fields.get("pending_segment_count", "0")) + 1)
        parsed.append(fields)

    def count_pairs(items):
        counts = collections.Counter()
        for item in items:
            name, separator, value = item.partition(":")
            if not name or not separator:
                continue
            try:
                amount = int(value)
            except ValueError:
                amount = 1
            counts[name] += amount
        return dict(counts)

    def trail_overlap(previous, current):
        limit = min(len(previous), len(current))
        for size in range(limit, 0, -1):
            if previous[-size:] == current[:size]:
                return size
        return 0

    def incremental_correction_event_counts(items):
        counts = collections.Counter()
        recent_trails = []
        for item in items:
            trail = item.get("correction_events", [])
            if not trail:
                counts.update(count_pairs(item.get("correction_event_counts", [])))
                continue
            overlap = max((trail_overlap(previous, trail) for previous in recent_trails), default=0)
            for event in trail[overlap:]:
                kind = event_kind(event)
                if kind:
                    counts[kind] += 1
            recent_trails.append(trail)
            recent_trails = recent_trails[-8:]
        return dict(counts)

    def confirmed_correction_record(item):
        events = item.get("correction_events", [])
        if not events or event_kind(events[-1]) not in {"candidate-selected", "raw-committed"}:
            return False
        return item.get("header_terminal", "1") != "0"

    def ranked(counter, key_name, limit):
        last_seen = {}
        for index, item in enumerate(parsed):
            if key_name in {"preedit", "selected_candidate"} and record_mode(item) != "active-preedit":
                continue
            value = item.get(key_name, "")
            if value:
                last_seen[value] = index
        return [
            {"text": text, "count": count}
            for text, count in sorted(counter.items(), key=lambda item: (-item[1], -last_seen.get(item[0], -1), item[0]))[:limit]
        ]

    def ranked_preedit_selected(counter, limit):
        last_seen = {}
        for index, item in enumerate(parsed):
            if not learnable_selected(item):
                continue
            preedit = item.get("preedit", "")
            candidate = item.get("selected_candidate", "")
            if preedit and candidate:
                last_seen[(preedit, candidate)] = index
        return [
            {"preedit": preedit, "candidate": candidate, "count": count}
            for (preedit, candidate), count in sorted(
                counter.items(),
                key=lambda item: (-item[1], -last_seen.get(item[0], -1), item[0][0], item[0][1]),
            )[:limit]
        ]

    def prefix_only_candidates_for_history(item):
        preedit = item.get("preedit", "")
        candidates = item.get("candidates", [])
        if not preedit or not candidates:
            return set()
        result = set()
        metadata_candidates = set()
        full_consumed = set()
        for metadata in item.get("candidate_metadata", []):
            if len(metadata) < 3 or metadata[1] != "consumed_prefix":
                continue
            try:
                candidate_index = int(metadata[0])
                consumed = int(metadata[2])
            except ValueError:
                continue
            if candidate_index < 0 or candidate_index >= len(candidates):
                continue
            candidate = candidates[candidate_index]
            metadata_candidates.add(candidate)
            if 0 < consumed < len(preedit):
                result.add(candidate)
            else:
                full_consumed.add(candidate)
        result.difference_update(full_consumed)
        if preedit.isalpha() and len(preedit) >= 8:
            for candidate in candidates:
                if (
                    candidate not in metadata_candidates
                    and not any(ch.isalnum() for ch in candidate)
                    and len(candidate) * 3 < len(preedit)
                ):
                    result.add(candidate)
        for candidate in candidates:
            if candidate in metadata_candidates:
                continue
            for other in candidates:
                if candidate != other and other.startswith(candidate):
                    result.add(candidate)
                    break
        return result

    def learnable_selected(item):
        if record_mode(item) != "active-preedit":
            return False
        preedit = item.get("preedit", "")
        candidate = item.get("selected_candidate", "")
        if not preedit or not candidate:
            return False
        confirmed = False
        for event in item.get("events", []):
            kind, separator, text = event.partition(":")
            if not separator:
                continue
            if kind == "candidate-selected" and text == candidate:
                confirmed = True
                break
            if kind == "raw-committed" and candidate == preedit and text == preedit:
                confirmed = True
                break
        if not confirmed:
            return False
        index_text = item.get("selected_candidate_index", "")
        try:
            selected_index = int(index_text)
        except ValueError:
            selected_index = 0
        if candidate in prefix_only_candidates_for_history(item):
            return False
        return selected_index > 0 or candidate == preedit

    def record_mode(item):
        mode = item.get("supervision_mode", "")
        if mode in {"active-preedit", "pass-through-only"}:
            return mode
        return "active-preedit" if item.get("preedit", "") else "pass-through-only"

    def ranked_corrections(counter, limit):
        last_seen = {}
        for index, item in enumerate(parsed):
            record_preedit = item.get("preedit", "")
            correction_events = item.get("correction_events", [])
            if not record_preedit or not confirmed_correction_record(item):
                continue
            for correction in possible_corrections_from_events(correction_events, record_preedit):
                last_seen[(correction["typo"], correction["corrected_preedit"])] = index
        return [
            {"typo": typo, "corrected_preedit": corrected, "count": count}
            for (typo, corrected), count in sorted(
                counter.items(),
                key=lambda item: (-item[1], -last_seen.get(item[0], -1), item[0][0], item[0][1]),
            )[:limit]
        ]

    preedits = collections.Counter(
        item.get("preedit", "") for item in parsed
        if record_mode(item) == "active-preedit" and item.get("preedit")
    )
    selected = collections.Counter(
        item.get("selected_candidate", "") for item in parsed if learnable_selected(item)
    )
    preedit_selected = collections.Counter(
        (item.get("preedit", ""), item.get("selected_candidate", ""))
        for item in parsed
        if learnable_selected(item)
    )
    applications = collections.Counter(item.get("application", "") for item in parsed if item.get("application"))
    event_counts = count_pairs(value for item in parsed for value in item.get("event_counts", []))
    active_event_counts = count_pairs(
        value
        for item in parsed
        if record_mode(item) == "active-preedit"
        for value in item.get("event_counts", [])
    )
    pass_through_event_counts = count_pairs(
        value
        for item in parsed
        if record_mode(item) == "pass-through-only"
        for value in item.get("event_counts", [])
    )
    correction_counts = incremental_correction_event_counts(parsed)
    history_corrections = collections.Counter()
    for item in parsed:
        record_preedit = item.get("preedit", "")
        correction_events = item.get("correction_events", [])
        if not record_preedit or not confirmed_correction_record(item):
            continue
        for correction in possible_corrections_from_events(correction_events, record_preedit):
            history_corrections[(correction["typo"], correction["corrected_preedit"])] += 1
    active_records = sum(1 for item in parsed if record_mode(item) == "active-preedit")
    pass_through_records = sum(1 for item in parsed if record_mode(item) == "pass-through-only")

    return {
        "available": bool(parsed),
        "path": str(history_path),
        "records": len(parsed),
        "active_preedit_records": active_records,
        "pass_through_records": pass_through_records,
        "top_preedits": ranked(preedits, "preedit", 5),
        "top_selected_candidates": ranked(selected, "selected_candidate", 5),
        "top_preedit_selected_pairs": ranked_preedit_selected(preedit_selected, 8),
        "top_corrections": ranked_corrections(history_corrections, 8),
        "top_applications": ranked(applications, "application", 3),
        "event_counts": event_counts,
        "active_event_counts": active_event_counts,
        "pass_through_event_counts": pass_through_event_counts,
        "correction_event_counts": correction_counts,
        "segment_chain_count": sum(int(item.get("segment_chain_count", "0")) for item in parsed),
        "pending_segment_count": sum(int(item.get("pending_segment_count", "0")) for item in parsed),
    }

correction_patterns = correction_patterns_from_rows(corrections)
declared_recent_event_counts = declared_event_counts(raw_event_counts)
declared_correction_event_counts = declared_event_counts(raw_correction_event_counts)
supervision_mode = "active-preedit" if preedit else "pass-through-only"
declared_supervision_mode = supervision_state.get("mode", "")
if declared_supervision_mode in {"active-preedit", "pass-through-only"}:
    supervision_mode = declared_supervision_mode
possible_corrections = possible_corrections_from_events(correction_events, preedit)
history_summary = recent_history_summary() if send_recent_input else {"available": False, "records": 0}
learning_signals = current_learning_signals(preedit, candidates, candidate_metadata, selected_candidate, segment_chains, pending_segments, corrections)
supervised_signals = supervised_learning_signals(
    preedit, candidates, candidate_metadata, selected_candidate, segment_chains, pending_segments,
    possible_corrections, corrections, history_summary
)
behavior_summary = {
    "recent_event_counts": declared_recent_event_counts or event_counts(events),
    "correction_event_counts": declared_correction_event_counts or event_counts(correction_events),
    "preedit_leading_context": preedit_leading_context(events, bool(preedit)),
    "possible_corrections": possible_corrections,
    "edit_summary": edit_summary_from_events(correction_events),
    "correction_patterns": correction_patterns,
    "realtime_correction_decisions": realtime_correction_decisions(preedit, correction_patterns),
    "raw_english_hint": raw_english_hint(preedit, application, surrounding_before, surrounding_after, preferences),
    "learning_signals": learning_signals,
    "supervised_learning_signals": supervised_signals,
}

prompt = {
    "protocol": "tipe.cloud-rerank.v1",
    "invocation": "explicit-one-shot",
    "data_sharing": {
        "recent_input": send_recent_input,
        "surrounding_text_and_application": send_surrounding,
    },
    "task": "Rerank TiPE input method candidates and optionally suggest typo corrections or learning rows from supervised key behavior.",
    "learning_objectives": [
        {
            "name": "ranking_preference",
            "evidence": "selected_candidate is a non-leading full-preedit candidate, repeated known_preferences, or explicit user choice in the current UI state; never learn a full-preedit preference from a prefix-only candidate",
            "output": "preference<TAB>PREEDIT<TAB>CANDIDATE<TAB>COUNT",
        },
        {
            "name": "typo_correction",
            "evidence": "correction_events, known_corrections, behavior_summary.possible_corrections, or repeated behavior_summary.correction_patterns show omitted, extra, replaced, transposed, deleted, or middle-edited keys",
            "output": "correction<TAB>TYPO<TAB>CORRECTED_PREEDIT",
        },
        {
            "name": "segment_chain",
            "evidence": "recent_segment_chains show the user selected a prefix candidate, kept composing, then selected a combined phrase; pending_segments show an unfinished prefix selection and must wait for suffix confirmation",
            "output": "segment_chain<TAB>ORIGINAL<TAB>CONSUMED<TAB>COMMITTED<TAB>REMAINING<TAB>CORRECTED_FULL<TAB>COMBINED<TAB>COUNT",
        },
        {
            "name": "raw_english",
            "evidence": "explicit raw English commits or selected raw candidate behavior; never application name alone",
            "output": "candidate<TAB>RAW_PREEDIT only when raw text should outrank Chinese candidates",
        },
    ],
    "rules": [
        "Return plain lines only.",
        "Use candidate<TAB>TEXT only for TEXT already in candidates, except raw English preedit when it is clearly an English identifier.",
        "Use correction<TAB>TYPO<TAB>CORRECTED_PREEDIT only for clear keyboard omission, extra-key, replacement, adjacent-transposition, delete-retype, or in-preedit edit patterns visible in correction_events and behavior_summary.",
        "Use preference<TAB>PREEDIT<TAB>CANDIDATE<TAB>COUNT when the selected candidate or repeated behavior proves a stable ordering preference.",
        "Use segment_chain<TAB>ORIGINAL<TAB>CONSUMED<TAB>COMMITTED<TAB>REMAINING<TAB>CORRECTED_FULL<TAB>COMBINED<TAB>COUNT when multi-step prefix selections prove that a long preedit should compose into one combined candidate.",
        "Use candidate_metadata to distinguish full-preedit candidates from prefix candidates; 0 < consumed_prefix < preedit length means selecting that candidate commits only that leading pinyin and keeps the remaining preedit, while consumed_prefix equal to preedit length consumes the current preedit fully.",
        "Do not output preference rows for prefix-only candidates; wait for segment_chain evidence after the suffix is confirmed.",
        "Use recent_events as the bounded key stream, behavior_summary.preedit_leading_context as the pass-through keys before the current preedit, correction_events as the longer edit/correction trail, selected_candidate as the latest explicit user choice, visible_candidates and numbered_candidates as the exact UI state, pending_segments as unfinished prefix selections, and recent_segment_chains as prior confirmed multi-step selection evidence.",
        "Use recent_history_summary as bounded cross-request behavior context; top_preedit_selected_pairs is stronger ranking evidence than global top_selected_candidates because it preserves which preedit led to which explicit choice.",
        "When recent_history_summary suggests a habit, generalize the behavior pattern only if it is compatible with current preedit, candidate_metadata, correction_events, or supervised_learning_signals; do not turn global frequency into unrelated current output.",
        "Use behavior_summary.supervised_learning_signals as the compact list of learning rows already supported by TiPE; prefer these signals over inventing new protocol shapes.",
        "Do not echo rows that already appear in known_preferences, known_corrections, or recent_segment_chains; they are context for ranking and explanation, not work to output again.",
        "When supervision_mode is pass-through-only, treat the request as read-only keyboard behavior context; it may contain a bounded raw token in English input mode, but it is not an active candidate-generation request.",
        "Generalize from the supervised behavior fields and known learning rows; do not hardcode example words, pinyin strings, applications, or screenshots.",
        "Do not infer raw English preference from application name or code-looking context alone; learn it only from explicit repeated raw commits or selected candidate evidence.",
        "Prefer conservative learning: one clear high-confidence row is better than many guesses.",
        "Do not explain.",
    ],
    "preedit": preedit,
    "supervision_mode": supervision_mode,
    "application": application,
    "surrounding_context": {
        "before_cursor": surrounding_before,
        "after_cursor": surrounding_after,
    },
    "candidates": candidates,
    "candidate_metadata": candidate_metadata,
    "input_state": input_state,
    "runtime_state": runtime_state,
    "supervision_state": supervision_state,
    "selected_candidate": selected_candidate,
    "visible_candidates": visible_candidates,
    "numbered_candidates": numbered_candidates,
    "behavior_summary": behavior_summary,
    "learning_status": learning_status(supervision_mode, preedit, candidates, selected_candidate, supervised_signals, history_summary),
    "recent_events": events[-48:],
    "correction_events": correction_events[-128:],
    "recent_context": context[-16:],
    "pending_segments": pending_segments[-8:],
    "recent_segment_chains": segment_chains[-16:],
    "known_preferences": preferences[:16],
    "known_corrections": corrections[:16],
    "recent_history_summary": history_summary,
}

print(json.dumps({
    "model": model,
    "temperature": temperature,
    "max_tokens": max_tokens,
    "messages": [
        {"role": "system", "content": "You are a conservative input method reranker."},
        {"role": "user", "content": json.dumps(prompt, ensure_ascii=False)},
    ],
}, ensure_ascii=False))
'
}

parse_chat_response() {
    python3 -c '
import json
import sys

try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)

content = ""
choices = data.get("choices") or []
if choices:
    message = choices[0].get("message") or {}
    content = message.get("content") or choices[0].get("text") or ""
print(content)
'
}

chat_url() {
    local path="$chat_path"
    [[ "$path" == /* ]] || path="/$path"
    printf '%s%s\n' "${base_url%/}" "$path"
}

run_openai_compatible() {
    if [[ "$dry_run" == "1" ]]; then
        command -v python3 >/dev/null 2>&1 || exit 0
        printf 'request\t%s\n' "$(chat_url)"
        printf 'request-json\t'
        json_request_body
        return
    fi

    command -v curl >/dev/null 2>&1 || exit 0
    command -v python3 >/dev/null 2>&1 || exit 0

    headers=(-H 'Content-Type: application/json')
    if [[ -n "$api_key" ]]; then
        if [[ "$api_key" == *$'\n'* || "$api_key" == *$'\r'* ]]; then
            echo "TIPE_MODEL_API_KEY must not contain newlines" >&2
            return 2
        fi
        http_header_file=$(mktemp "${TMPDIR:-/tmp}/tipe-model-header.XXXXXX")
        trap 'rm -f -- "$http_header_file"' EXIT
        trap 'rm -f -- "$http_header_file"; exit 129' HUP
        trap 'rm -f -- "$http_header_file"; exit 130' INT
        trap 'rm -f -- "$http_header_file"; exit 143' TERM
        chmod 0600 "$http_header_file"
        printf 'Authorization: Bearer %s\n' "$api_key" >"$http_header_file"
        headers+=(-H "@$http_header_file")
    fi

    local status=0
    json_request_body | curl -fsS -m "$timeout_seconds" "${headers[@]}" \
        -d @- "$(chat_url)" | parse_chat_response | emit_safe_protocol_lines || status=$?
    if [[ -n "$http_header_file" ]]; then
        rm -f -- "$http_header_file"
        http_header_file=""
        trap - EXIT HUP INT TERM
    fi
    return "$status"
}

llama_prompt() {
    json_request_body | python3 -c '
import json
import sys

payload = json.load(sys.stdin)
for message in payload.get("messages", []):
    role = str(message.get("role", "user")).capitalize()
    content = message.get("content", "")
    print(f"{role}: {content}")
print("Assistant:")
'
}

cleanup_llama_tmp() {
    if [[ -n "$llama_tmp_dir" && -d "$llama_tmp_dir" ]]; then
        rm -rf -- "$llama_tmp_dir"
    fi
    llama_tmp_dir=""
}

run_llama_cpp() {
    if [[ "$dry_run" == "1" ]]; then
        command -v python3 >/dev/null 2>&1 || exit 0
        printf 'request\tllama-cpp:%s\n' "$model"
        printf 'request-json\t'
        json_request_body
        printf 'llama-command\t%s\n' "$llama_command"
        return
    fi

    command -v python3 >/dev/null 2>&1 || {
        echo "python3 is required for llama.cpp prompt generation" >&2
        return 1
    }
    [[ "$llama_threads" =~ ^[1-9][0-9]*$ && "$llama_context" =~ ^[1-9][0-9]*$ ]] || {
        echo "TIPE_LLAMA_CPP_THREADS and TIPE_LLAMA_CPP_CONTEXT must be positive integers" >&2
        return 2
    }
    [[ -x "$llama_command" ]] || {
        echo "llama.cpp command is not executable: $llama_command" >&2
        return 1
    }
    [[ -r "$model" ]] || {
        echo "llama.cpp GGUF model is not readable: $model" >&2
        return 1
    }

    local prompt_path output_path error_path status
    llama_tmp_dir=$(mktemp -d)
    chmod 0700 "$llama_tmp_dir"
    trap cleanup_llama_tmp EXIT
    trap 'cleanup_llama_tmp; exit 129' HUP
    trap 'cleanup_llama_tmp; exit 130' INT
    trap 'cleanup_llama_tmp; exit 143' TERM
    prompt_path="$llama_tmp_dir/prompt.txt"
    output_path="$llama_tmp_dir/output.txt"
    error_path="$llama_tmp_dir/error.txt"
    llama_prompt >"$prompt_path"
    chmod 0600 "$prompt_path"
    status=0
    "$llama_command" -m "$model" -f "$prompt_path" -n "$max_tokens" --temp "$temperature" \
        -c "$llama_context" -t "$llama_threads" -ngl 0 --no-display-prompt --simple-io \
        >"$output_path" 2>"$error_path" || status=$?
    if [[ "$status" == "0" ]]; then
        emit_safe_protocol_lines <"$output_path"
    else
        tail -c 4096 "$error_path" >&2 || true
    fi
    cleanup_llama_tmp
    trap - EXIT HUP INT TERM
    return "$status"
}

case "$backend" in
    heuristic)
        run_heuristic
        ;;
    ollama|openai|openai-compatible)
        run_openai_compatible
        ;;
    llama-cpp)
        run_llama_cpp
        ;;
    *)
        echo "unknown TIPE_MODEL_BACKEND: $backend" >&2
        exit 2
        ;;
esac
