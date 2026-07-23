#!/usr/bin/env bash
set -euo pipefail

output_path=""
config_path=""
force=0
dry_run=0
configure=0

usage() {
    cat <<'EOF'
Usage:
  tipe-model-wrapper-new --path PATH [--force] [--configure] [--config PATH] [--dry-run]

Creates an executable custom TiPE model wrapper template. The generated wrapper
reads TiPE's TSV model request from stdin and emits safe protocol rows on stdout.
With --configure, it also writes TiPE's model-env custom configuration for the
generated wrapper. It does not restart fcitx5 or switch input methods.
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
        --path)
            require_value "$1" "${2:-}"
            output_path="$2"
            shift
            ;;
        --force)
            force=1
            ;;
        --configure)
            configure=1
            ;;
        --config)
            require_value "$1" "${2:-}"
            config_path="$2"
            shift
            ;;
        --dry-run)
            dry_run=1
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

if [[ -z "$output_path" ]]; then
    echo "--path is required" >&2
    exit 2
fi
if [[ "$output_path" == *$'\t'* || "$output_path" == *$'\r'* || "$output_path" == *$'\n'* ]]; then
    echo "--path must not contain control characters" >&2
    exit 2
fi
if [[ -n "$config_path" ]] &&
    [[ "$config_path" == *$'\t'* || "$config_path" == *$'\r'* || "$config_path" == *$'\n'* ]]; then
    echo "--config must not contain control characters" >&2
    exit 2
fi
if [[ -e "$output_path" && "$force" != "1" ]]; then
    echo "refusing to overwrite existing file: $output_path" >&2
    exit 1
fi
if [[ "$configure" == "1" && -z "${HOME:-}" ]]; then
    echo "HOME is not set; TiPE model configuration paths are unavailable" >&2
    exit 1
fi

template() {
    cat <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

# Custom TiPE model wrapper template.
# stdin:  TiPE model request TSV.
# stdout: protocol rows such as:
#         candidate<TAB>TEXT
#         correction<TAB>TYPO<TAB>CORRECTED_PREEDIT
#         preference<TAB>PREEDIT<TAB>CANDIDATE<TAB>COUNT
#         segment_chain<TAB>ORIGINAL<TAB>CONSUMED<TAB>COMMITTED<TAB>REMAINING<TAB>CORRECTED<TAB>COMBINED<TAB>COUNT
#
# Keep expensive model calls behind TiPE's click-triggered rerank path. TiPE
# validates returned candidates, corrections, preferences, and segment chains
# before accepting them, but this wrapper should still be conservative.

preedit=""
supervision_mode="pass-through-only"
pass_through_only=1
application=""
surrounding_before=""
surrounding_after=""
candidates=()
candidate_metadata=()
input_state=""
runtime_state=""
supervision_state=""
supervision_state_declared=0
continuous_mode=0
input_mode="chinese"
selected_candidate=""
visible_candidates=()
numbered_candidates=()
events=()
event_counts=()
preedit_leading_events=()
preedit_leading_event_counts=()
preedit_leading_active=0
preedit_leading_events_before_preedit=0
correction_events=()
correction_event_counts=()
context=()
segment_chains=()
pending_segments=()
preferences=()
corrections=()
edit_summary_current=""
edit_summary_cursor=0
edit_summary_typed_tail=""
edit_summary_last_erased=""
edit_summary_last_edited=""
edit_summary_middle_edit=""
correction_patterns=()
realtime_correction_decisions=()
recent_history_available=0
recent_history_path=""
recent_history_records=0
recent_history_active_records=0
recent_history_pass_through_records=0
recent_history_segment_chains=0
recent_history_pending_segments=0
recent_history_preedits=()
recent_history_selected_candidates=()
recent_history_preedit_selected_pairs=()
recent_history_applications=()
recent_history_event_counts=()
recent_history_active_event_counts=()
recent_history_pass_through_event_counts=()
recent_history_correction_event_counts=()
recent_history_corrections=()
learning_status_primary="no-action"
learning_status_next_step="do not emit output"
learning_status_suggested_protocols=()
learning_status_evidence_protocols=()
learning_status_awaiting_suffix=()
learning_status_signal_counts=()

while IFS=$'\t' read -r kind rest || [[ -n "${kind:-}" || -n "${rest:-}" ]]; do
    case "$kind" in
        preedit)
            preedit="${rest:-}"
            ;;
        application)
            application="${rest:-}"
            ;;
        surrounding_before)
            surrounding_before="${rest:-}"
            ;;
        surrounding_after)
            surrounding_after="${rest:-}"
            ;;
        candidates)
            if [[ -n "${rest:-}" ]]; then
                IFS=$'\t' read -r -a candidates <<< "$rest"
            fi
            ;;
        candidate_metadata)
            candidate_metadata+=("${rest:-}")
            ;;
        state)
            input_state="${rest:-}"
            ;;
        runtime_state)
            runtime_state="${rest:-}"
            IFS=$'\t' read -r -a runtime_fields <<< "$runtime_state"
            for ((runtime_index = 0; runtime_index + 1 < ${#runtime_fields[@]}; runtime_index += 2)); do
                case "${runtime_fields[$runtime_index]}" in
                    continuous)
                        continuous_mode="${runtime_fields[$((runtime_index + 1))]}"
                        ;;
                    input_mode)
                        input_mode="${runtime_fields[$((runtime_index + 1))]}"
                        ;;
                esac
            done
            ;;
        supervision_state)
            supervision_state="${rest:-}"
            supervision_state_declared=1
            IFS=$'\t' read -r -a supervision_fields <<< "$supervision_state"
            for ((supervision_index = 0; supervision_index + 1 < ${#supervision_fields[@]}; supervision_index += 2)); do
                if [[ "${supervision_fields[$supervision_index]}" == "mode" &&
                      ( "${supervision_fields[$((supervision_index + 1))]}" == "active-preedit" ||
                        "${supervision_fields[$((supervision_index + 1))]}" == "pass-through-only" ) ]]; then
                    supervision_mode="${supervision_fields[$((supervision_index + 1))]}"
                fi
            done
            ;;
        selected_candidate)
            selected_candidate="${rest:-}"
            ;;
        visible_candidates)
            if [[ -n "${rest:-}" ]]; then
                IFS=$'\t' read -r -a visible_candidates <<< "$rest"
            fi
            ;;
        numbered_candidates)
            if [[ -n "${rest:-}" ]]; then
                IFS=$'\t' read -r -a numbered_candidates <<< "$rest"
            fi
            ;;
        events)
            if [[ -n "${rest:-}" ]]; then
                IFS=$'\t' read -r -a events <<< "$rest"
            fi
            ;;
        event_counts)
            if [[ -n "${rest:-}" ]]; then
                IFS=$'\t' read -r -a event_counts <<< "$rest"
            fi
            ;;
        correction_events)
            if [[ -n "${rest:-}" ]]; then
                IFS=$'\t' read -r -a correction_events <<< "$rest"
            fi
            ;;
        correction_event_counts)
            if [[ -n "${rest:-}" ]]; then
                IFS=$'\t' read -r -a correction_event_counts <<< "$rest"
            fi
            ;;
        context)
            if [[ -n "${rest:-}" ]]; then
                IFS=$'\t' read -r -a context <<< "$rest"
            fi
            ;;
        segment_chain)
            segment_chains+=("${rest:-}")
            ;;
        pending_segment)
            pending_segments+=("${rest:-}")
            ;;
        preference)
            preferences+=("${rest:-}")
            ;;
        correction)
            corrections+=("${rest:-}")
            ;;
    esac
done

if [[ -n "$preedit" && ( "$supervision_state_declared" != "1" || "$supervision_mode" != "pass-through-only" ) ]]; then
    supervision_mode="active-preedit"
    pass_through_only=0
elif [[ "$supervision_mode" == "active-preedit" ]]; then
    pass_through_only=0
fi

compute_behavior_summary() {
    command -v python3 >/dev/null 2>&1 || return 0
    local summary line kind rest
    summary=$(
        TIPE_WRAPPER_EVENTS="$(printf '%s\n' "${events[@]}")" \
        TIPE_WRAPPER_CORRECTION_EVENTS="$(printf '%s\n' "${correction_events[@]}")" \
        TIPE_WRAPPER_CORRECTIONS="$(printf '%s\n' "${corrections[@]}")" \
        TIPE_WRAPPER_PREEDIT="$preedit" \
        python3 - <<'PY'
import os


def event_kind(event):
    return event.split(":", 1)[0]


def event_text(event):
    parts = event.split(":", 1)
    return parts[1] if len(parts) == 2 else ""


def event_counts(items):
    counts = {}
    order = []
    for item in items:
        kind = event_kind(item)
        if kind not in counts:
            counts[kind] = 0
            order.append(kind)
        counts[kind] += 1
    return [(kind, counts[kind]) for kind in order]


def preedit_leading_context(items, has_preedit):
    if not has_preedit:
        return list(items[-48:])
    leading = []
    for item in items[-48:]:
        kind = event_kind(item)
        if kind in {"letter", "digit", "symbol", "rerank-requested"}:
            break
        leading.append(item)
    return leading


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
    return current, cursor, typed_tail, last_fully_erased, last_edited_original, middle_edit_original


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
    return sorted(patterns.items(), key=lambda item: (-item[1], item[0][0], item[0][3], item[0][2], item[0][1]))[:5]

KNOWN_ENGLISH_TOKENS = {
    "typescript", "flatpak", "github", "docker", "cursor", "openai", "python", "vscode",
    "wayland", "cargo", "cmake", "codex", "fcitx", "linux", "react", "bash",
    "javascript", "cargobuild", "cmakebuild", "hyprland", "chatgpt", "ollama", "waybar", "systemd",
    "gnome", "dbus", "build", "json", "node", "niri", "npm", "rust",
    "vue", "api", "gtk", "gpt4", "qwen2", "qwen3", "ipv4", "ipv6",
    "git", "gpt", "tipe",
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
    if any(marker in lowered for marker in (
        "ck", "cl", "cr", "ct", "dr", "ea", "ee", "fl", "ft", "gr", "ld", "ll",
        "lt", "mp", "nd", "nt", "oo", "ph", "pl", "pr", "pt", "rb", "rd", "rk",
        "rn", "rs", "rt", "sk", "sl", "sm", "sn", "sp", "ss", "st", "sv", "sw",
        "th", "tr", "ts", "tw", "xt",
    )):
        return True
    return any(ch in lowered for ch in "aeiouy") and lowered[-1] in "bdfjklmpqtvxz"


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


def realtime_correction_decisions(preedit, patterns):
    guard = ""
    if len(preedit) > 16:
        guard = "long-preedit"
    elif preedit in KNOWN_ENGLISH_TOKENS:
        guard = "known-raw-english"
    elif looks_like_english_identifier(preedit):
        guard = "english-identifier"

    decisions = []
    for (kind, text, position, relative_to_end), count in patterns:
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
        position_text = f"end-{position}" if relative_to_end else str(position)
        decisions.append((status, reason, kind, text, position_text, count, corrected))
    return decisions


preedit = os.environ.get("TIPE_WRAPPER_PREEDIT", "")
events = [line for line in os.environ.get("TIPE_WRAPPER_EVENTS", "").splitlines() if line]
correction_events = [line for line in os.environ.get("TIPE_WRAPPER_CORRECTION_EVENTS", "").splitlines() if line]
corrections = [line for line in os.environ.get("TIPE_WRAPPER_CORRECTIONS", "").splitlines() if line]
current, cursor, typed_tail, last_erased, last_edited, middle_edit = edit_summary_from_events(correction_events)
patterns = correction_patterns_from_rows(corrections)
leading_context = preedit_leading_context(events, bool(preedit))
print(f"preedit_leading_summary\t{1 if leading_context else 0}\t{len(leading_context)}")
for item in leading_context:
    print(f"preedit_leading_event\t{item}")
for kind, count in event_counts(leading_context):
    print(f"preedit_leading_event_count\t{kind}\t{count}")
print(f"edit_summary\t{current}\t{cursor}\t{typed_tail}\t{last_erased}\t{last_edited}\t{middle_edit}")
for (kind, text, position, relative_to_end), count in patterns:
    position_text = f"end-{position}" if relative_to_end else str(position)
    print(f"correction_pattern\t{kind}\t{text}\t{position_text}\t{count}")
for status, reason, kind, text, position, count, corrected in realtime_correction_decisions(preedit, patterns):
    print(f"realtime_correction\t{status}\t{reason}\t{kind}\t{text}\t{position}\t{count}\t{corrected}")
PY
    )
    while IFS= read -r line; do
        kind="${line%%$'\t'*}"
        if [[ "$line" == *$'\t'* ]]; then
            rest="${line#*$'\t'}"
        else
            rest=""
        fi
        case "$kind" in
            preedit_leading_summary)
                rest="${rest//$'\t'/$'\001'}"
                IFS=$'\001' read -r preedit_leading_active preedit_leading_events_before_preedit <<< "${rest:-}"
                ;;
            preedit_leading_event)
                preedit_leading_events+=("${rest:-}")
                ;;
            preedit_leading_event_count)
                preedit_leading_event_counts+=("${rest:-}")
                ;;
            edit_summary)
                rest="${rest//$'\t'/$'\001'}"
                IFS=$'\001' read -r edit_summary_current edit_summary_cursor edit_summary_typed_tail \
                    edit_summary_last_erased edit_summary_last_edited edit_summary_middle_edit <<< "${rest:-}"
                ;;
            correction_pattern)
                correction_patterns+=("${rest:-}")
                ;;
            realtime_correction)
                realtime_correction_decisions+=("${rest:-}")
                ;;
        esac
    done <<< "$summary"
}

compute_behavior_summary

compute_recent_history_summary() {
    command -v python3 >/dev/null 2>&1 || return 0
    local summary line kind rest
    summary=$(
        python3 - <<'PY'
import collections
import os
from pathlib import Path


def plausible(typo, corrected):
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


def event_kind(event):
    return event.split(":", 1)[0]


def event_text(event):
    parts = event.split(":", 1)
    return parts[1] if len(parts) == 2 else ""


def possible_corrections(items, corrected_preedit):
    items = list(items[-192:])
    if items and event_kind(items[-1]) in {"candidate-selected", "raw-committed", "escape"}:
        items.pop()
    current = ""
    cursor = 0
    erased_original = ""
    last_fully_erased = None
    last_edited_original = None
    middle_edit_original = None
    erasing = False

    def remember_middle_edit():
        nonlocal middle_edit_original
        if current and middle_edit_original is None:
            middle_edit_original = current

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
            middle_edit_original = None
            erasing = False

    if current != corrected_preedit:
        return []
    result = []
    seen = set()
    for typo in (last_fully_erased, last_edited_original, middle_edit_original):
        if typo and typo not in seen and plausible(typo, corrected_preedit):
            result.append((typo, corrected_preedit))
            seen.add(typo)
    return result


def history_path():
    if os.environ.get("TIPE_SUPERVISION_HISTORY"):
        return Path(os.environ["TIPE_SUPERVISION_HISTORY"])
    if os.environ.get("XDG_CACHE_HOME"):
        return Path(os.environ["XDG_CACHE_HOME"]) / "tipe" / "supervision-history.tsv"
    if os.environ.get("HOME"):
        return Path(os.environ["HOME"]) / ".cache" / "tipe" / "supervision-history.tsv"
    return Path("/tmp/tipe/supervision-history.tsv")


path = history_path()
try:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
except OSError:
    print(f"summary\t0\t{path}\t0\t0\t0\t0\t0")
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
            fields["application"] = parts[1]
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
    return counts


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
    return counts


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
    return sorted(counter.items(), key=lambda item: (-item[1], -last_seen.get(item[0], -1), item[0]))[:limit]


def prefix_only_candidates(item):
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
    try:
        selected_index = int(item.get("selected_candidate_index", "0"))
    except ValueError:
        selected_index = 0
    if candidate in prefix_only_candidates(item):
        return False
    return selected_index > 0 or candidate == preedit

def record_mode(item):
    mode = item.get("supervision_mode", "")
    if mode in {"active-preedit", "pass-through-only"}:
        return mode
    return "active-preedit" if item.get("preedit", "") else "pass-through-only"


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
events = count_pairs(value for item in parsed for value in item.get("event_counts", []))
active_events = count_pairs(
    value
    for item in parsed
    if record_mode(item) == "active-preedit"
    for value in item.get("event_counts", [])
)
pass_through_events = count_pairs(
    value
    for item in parsed
    if record_mode(item) == "pass-through-only"
    for value in item.get("event_counts", [])
)
correction_events = incremental_correction_event_counts(parsed)
history_corrections = collections.Counter()
last_correction_seen = {}
for index, item in enumerate(parsed):
    record_preedit = item.get("preedit", "")
    record_events = item.get("correction_events", [])
    if not record_preedit or not confirmed_correction_record(item):
        continue
    for pair in possible_corrections(record_events, record_preedit):
        history_corrections[pair] += 1
        last_correction_seen[pair] = index
active = sum(1 for item in parsed if record_mode(item) == "active-preedit")
pass_through = sum(1 for item in parsed if record_mode(item) == "pass-through-only")
segment_chains = sum(int(item.get("segment_chain_count", "0")) for item in parsed)
pending_segments = sum(int(item.get("pending_segment_count", "0")) for item in parsed)

print(f"summary\t{1 if parsed else 0}\t{path}\t{len(parsed)}\t{active}\t{pass_through}\t{segment_chains}\t{pending_segments}")
for text, count in ranked(preedits, "preedit", 5):
    print(f"preedit\t{text}\t{count}")
for text, count in ranked(selected, "selected_candidate", 5):
    print(f"selected_candidate\t{text}\t{count}")
last_pair_seen = {}
for index, item in enumerate(parsed):
    preedit = item.get("preedit", "")
    candidate = item.get("selected_candidate", "")
    if learnable_selected(item):
        last_pair_seen[(preedit, candidate)] = index
for (preedit, candidate), count in sorted(
    preedit_selected.items(),
    key=lambda item: (-item[1], -last_pair_seen.get(item[0], -1), item[0][0], item[0][1]),
)[:8]:
    print(f"preedit_selected\t{preedit}\t{candidate}\t{count}")
for text, count in ranked(applications, "application", 3):
    print(f"application\t{text}\t{count}")
for name, count in events.most_common(8):
    print(f"event_count\t{name}\t{count}")
for name, count in active_events.most_common(8):
    print(f"active_event_count\t{name}\t{count}")
for name, count in pass_through_events.most_common(8):
    print(f"pass_through_event_count\t{name}\t{count}")
for name, count in correction_events.most_common(8):
    print(f"correction_event_count\t{name}\t{count}")
for (typo, corrected), count in sorted(
    history_corrections.items(),
    key=lambda item: (-item[1], -last_correction_seen.get(item[0], -1), item[0][0], item[0][1]),
)[:8]:
    print(f"correction\t{typo}\t{corrected}\t{count}")
PY
    )
    while IFS= read -r line; do
        kind="${line%%$'\t'*}"
        if [[ "$line" == *$'\t'* ]]; then
            rest="${line#*$'\t'}"
        else
            rest=""
        fi
        case "$kind" in
            summary)
                rest="${rest//$'\t'/$'\001'}"
                IFS=$'\001' read -r recent_history_available recent_history_path recent_history_records \
                    recent_history_active_records recent_history_pass_through_records \
                    recent_history_segment_chains recent_history_pending_segments <<< "${rest:-}"
                ;;
            preedit)
                recent_history_preedits+=("${rest:-}")
                ;;
            selected_candidate)
                recent_history_selected_candidates+=("${rest:-}")
                ;;
            preedit_selected)
                recent_history_preedit_selected_pairs+=("${rest:-}")
                ;;
            application)
                recent_history_applications+=("${rest:-}")
                ;;
            event_count)
                recent_history_event_counts+=("${rest:-}")
                ;;
            active_event_count)
                recent_history_active_event_counts+=("${rest:-}")
                ;;
            pass_through_event_count)
                recent_history_pass_through_event_counts+=("${rest:-}")
                ;;
            correction_event_count)
                recent_history_correction_event_counts+=("${rest:-}")
                ;;
            correction)
                recent_history_corrections+=("${rest:-}")
                ;;
        esac
    done <<< "$summary"
}

compute_recent_history_summary

emit_candidate_if_present() {
    local wanted="$1"
    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ "$candidate" == "$wanted" ]]; then
            printf 'candidate\t%s\n' "$wanted"
            return 0
        fi
    done
    return 1
}

candidate_is_present() {
    local wanted="$1"
    local candidate
    for candidate in "${candidates[@]}"; do
        [[ "$candidate" == "$wanted" ]] && return 0
    done
    return 1
}

known_preference_present() {
    local wanted_preedit="$1"
    local wanted_candidate="$2"
    local item item_preedit item_candidate _count
    for item in "${preferences[@]}"; do
        IFS=$'\t' read -r item_preedit item_candidate _count <<< "$item"
        [[ "$item_preedit" == "$wanted_preedit" && "$item_candidate" == "$wanted_candidate" ]] && return 0
    done
    return 1
}

known_correction_present() {
    local wanted_typo="$1"
    local wanted_corrected="$2"
    local item typo corrected _count
    for item in "${corrections[@]}"; do
        IFS=$'\t' read -r typo corrected _count <<< "$item"
        [[ "$typo" == "$wanted_typo" && "$corrected" == "$wanted_corrected" ]] && return 0
    done
    return 1
}

known_segment_chain_present() {
    local wanted_original="$1"
    local wanted_consumed="$2"
    local wanted_committed="$3"
    local wanted_remaining="$4"
    local wanted_corrected="$5"
    local wanted_combined="$6"
    local item original consumed committed remaining corrected combined _extra
    for item in "${segment_chains[@]}"; do
        IFS=$'\t' read -r original consumed committed remaining corrected combined _extra <<< "$item"
        if [[ "$original" == "$wanted_original" && "$consumed" == "$wanted_consumed" &&
            "$committed" == "$wanted_committed" && "$remaining" == "$wanted_remaining" &&
            "$corrected" == "$wanted_corrected" && "$combined" == "$wanted_combined" ]]; then
            return 0
        fi
    done
    return 1
}

is_prefix_only_candidate() {
    local wanted="$1"
    local item index key_consumed consumed _key_source _source _key_score _score candidate
    for item in "${candidate_metadata[@]}"; do
        IFS=$'\t' read -r index key_consumed consumed _key_source _source _key_score _score <<< "$item"
        if [[ "$key_consumed" == "consumed_prefix" && "$consumed" =~ ^[0-9]+$ &&
              "$index" =~ ^[0-9]+$ && "${candidates[$index]:-}" == "$wanted" ]] &&
            (( consumed == 0 || consumed >= ${#preedit} )); then
            return 1
        fi
    done
    for item in "${candidate_metadata[@]}"; do
        IFS=$'\t' read -r index key_consumed consumed _key_source _source _key_score _score <<< "$item"
        if [[ "$key_consumed" == "consumed_prefix" && "$consumed" =~ ^[1-9][0-9]*$ &&
              "$consumed" -lt "${#preedit}" && "$index" =~ ^[0-9]+$ &&
              "${candidates[$index]:-}" == "$wanted" ]]; then
            return 0
        fi
    done
    if [[ "$preedit" =~ ^[A-Za-z]+$ && "${#preedit}" -ge 8 &&
          "$wanted" != *[A-Za-z0-9]* && $(( ${#wanted} * 3 )) -lt "${#preedit}" ]]; then
        return 0
    fi
    for candidate in "${candidates[@]}"; do
        if [[ "$candidate" != "$wanted" && "$candidate" == "$wanted"* ]]; then
            return 0
        fi
    done
    return 1
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

emit_matching_segment_chain_candidate() {
    local chain original consumed committed remaining corrected combined extra last_context suffix_candidate
    last_context=""
    if ((${#context[@]} > 0)); then
        last_context="${context[$((${#context[@]} - 1))]}"
    fi
    for chain in "${segment_chains[@]}"; do
        IFS=$'\t' read -r original consumed committed remaining corrected combined extra <<< "$chain"
        [[ -n "${combined:-}" ]] || continue
        if [[ "${original:-}" == "$preedit" ]]; then
            emit_candidate_if_present "$combined" || true
        elif [[ -n "$last_context" && "$last_context" == "$committed" && "$remaining" == "$preedit" &&
            "$combined" == "$committed"* ]]; then
            suffix_candidate="${combined#"$committed"}"
            [[ -n "$suffix_candidate" ]] && emit_candidate_if_present "$suffix_candidate" || true
        fi
    done
}

emit_selected_candidate_preference() {
    local selected_index selected_text extra
    [[ -n "$preedit" && -n "$selected_candidate" ]] || return 0
    IFS=$'\t' read -r selected_index selected_text extra <<< "$selected_candidate"
    [[ "$selected_index" =~ ^[0-9]+$ && "$selected_index" != "0" && -n "${selected_text:-}" ]] || return 0
    candidate_is_present "$selected_text" || return 0
    is_prefix_only_candidate "$selected_text" && return 0
    printf 'candidate\t%s\n' "$selected_text"
    known_preference_present "$preedit" "$selected_text" && return 0
    printf 'preference\t%s\t%s\t2\n' "$preedit" "$selected_text"
}

emit_pending_segment_confirmation_chain() {
    local selected_index selected_text extra segment original consumed committed remaining corrected combined
    local history_pair history_preedit history_candidate history_count history_weight
    [[ -n "$preedit" && -n "$selected_candidate" ]] || return 0
    IFS=$'\t' read -r selected_index selected_text extra <<< "$selected_candidate"
    [[ "$selected_index" =~ ^[1-9][0-9]*$ && -n "${selected_text:-}" ]] || return 0
    candidate_is_present "$selected_text" || return 0
    for segment in "${pending_segments[@]}"; do
        IFS=$'\t' read -r original consumed committed remaining extra <<< "$segment"
        [[ -n "${original:-}" && -n "${consumed:-}" && -n "${committed:-}" && "${remaining:-}" == "$preedit" ]] ||
            continue
        corrected="$consumed$preedit"
        combined="$committed$selected_text"
        valid_segment_chain_shape "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" ||
            continue
        known_segment_chain_present "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" &&
            continue
        printf 'segment_chain\t%s\t%s\t%s\t%s\t%s\t%s\t1\n' \
            "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined"
    done
}

compute_learning_status() {
    learning_status_primary="no-action"
    learning_status_next_step="do not emit output"
    learning_status_suggested_protocols=()
    learning_status_evidence_protocols=()
    learning_status_awaiting_suffix=()
    learning_status_signal_counts=()

    if [[ "$pass_through_only" == "1" ]]; then
        learning_status_primary="keyboard-context-only"
        learning_status_next_step="wait for an active preedit before emitting candidates or learning rows"
        return 0
    fi

    local selected_index selected_text extra segment original consumed committed remaining corrected combined
    local history_pair history_preedit history_candidate history_count history_weight history_typo history_corrected
    local selected_present=0 selected_explicit=0 selected_top=0 selected_prefix_only=0
    if [[ -n "$selected_candidate" ]]; then
        IFS=$'\t' read -r selected_index selected_text extra <<< "$selected_candidate"
        if [[ -n "${selected_text:-}" ]] && candidate_is_present "$selected_text"; then
            selected_present=1
            if [[ "${selected_index:-}" =~ ^[1-9][0-9]*$ ]]; then
                selected_explicit=1
            fi
            if [[ "${selected_index:-}" == "0" || "${candidates[0]:-}" == "$selected_text" ]]; then
                selected_top=1
            fi
            if is_prefix_only_candidate "$selected_text"; then
                selected_prefix_only=1
            fi
        fi
    fi

    if [[ "$selected_present" == "1" && "$selected_top" != "1" && "$selected_prefix_only" != "1" ]]; then
        if ! known_preference_present "$preedit" "$selected_text"; then
            learning_status_suggested_protocols+=("preference"$'\t'"$preedit"$'\t'"$selected_text"$'\t'"2")
            learning_status_signal_counts+=("selected_candidate:1")
        fi
    fi

    for history_pair in "${recent_history_preedit_selected_pairs[@]}"; do
        IFS=$'\t' read -r history_preedit history_candidate history_count extra <<< "$history_pair"
        [[ "$history_preedit" == "$preedit" && -n "${history_candidate:-}" &&
            "${history_count:-}" =~ ^[0-9]+$ && "$history_count" -ge 2 ]] || continue
        candidate_is_present "$history_candidate" || continue
        is_prefix_only_candidate "$history_candidate" && continue
        known_preference_present "$preedit" "$history_candidate" && continue
        history_weight="$history_count"
        if (( history_weight > 10 )); then
            history_weight=10
        fi
        learning_status_suggested_protocols+=("preference"$'\t'"$preedit"$'\t'"$history_candidate"$'\t'"$history_weight")
        learning_status_signal_counts+=("history_preference:1")
    done

    for history_pair in "${recent_history_corrections[@]}"; do
        IFS=$'\t' read -r history_typo history_corrected history_count extra <<< "$history_pair"
        [[ -n "${history_typo:-}" && -n "${history_corrected:-}" &&
            "${history_count:-}" =~ ^[0-9]+$ && "$history_count" -ge 2 ]] || continue
        [[ "$history_typo" == "$preedit" || "$history_corrected" == "$preedit" ]] || continue
        known_correction_present "$history_typo" "$history_corrected" && continue
        learning_status_suggested_protocols+=("correction"$'\t'"$history_typo"$'\t'"$history_corrected")
        learning_status_signal_counts+=("history_correction:1")
    done

    for segment in "${pending_segments[@]}"; do
        IFS=$'\t' read -r original consumed committed remaining extra <<< "$segment"
        [[ -n "${original:-}" && -n "${consumed:-}" && -n "${committed:-}" && "${remaining:-}" == "$preedit" ]] ||
            continue
        if [[ "$selected_present" == "1" && "$selected_explicit" == "1" ]]; then
            corrected="$consumed$preedit"
            combined="$committed$selected_text"
            if valid_segment_chain_shape "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" &&
                ! known_segment_chain_present "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined"; then
                learning_status_suggested_protocols+=("segment_chain"$'\t'"$original"$'\t'"$consumed"$'\t'"$committed"$'\t'"$remaining"$'\t'"$corrected"$'\t'"$combined"$'\t'"1")
                learning_status_signal_counts+=("pending_segment:1")
            fi
        else
            learning_status_awaiting_suffix+=("$original"$'\t'"$consumed"$'\t'"$committed"$'\t'"$remaining")
            learning_status_signal_counts+=("pending_segment:1")
        fi
    done

    for segment in "${corrections[@]}"; do
        IFS=$'\t' read -r original corrected extra <<< "$segment"
        if [[ -n "${original:-}" && -n "${corrected:-}" && ( "$original" == "$preedit" || "$corrected" == "$preedit" ) ]]; then
            learning_status_evidence_protocols+=("correction"$'\t'"$original"$'\t'"$corrected")
            learning_status_signal_counts+=("known_correction:1")
        fi
    done

    if ((${#learning_status_suggested_protocols[@]} > 0)); then
        learning_status_primary="ready-to-learn"
        learning_status_next_step="prefer suggested protocol rows that match the current active preedit and visible UI state"
    elif ((${#learning_status_awaiting_suffix[@]} > 0)); then
        learning_status_primary="awaiting-suffix-confirmation"
        learning_status_next_step="wait until the remaining preedit has an explicitly selected suffix candidate"
    elif [[ "$selected_present" == "1" && "$selected_prefix_only" == "1" ]]; then
        learning_status_primary="awaiting-suffix-confirmation"
        learning_status_next_step="wait until the prefix-only selection is followed by a confirmed suffix candidate"
    elif [[ "$selected_present" == "1" && "$selected_top" == "1" ]]; then
        learning_status_primary="selected-candidate-already-top"
        learning_status_next_step="no persistent preference is needed for the current selected candidate"
    elif [[ -n "$preedit" && ${#candidates[@]} -gt 0 ]]; then
        learning_status_primary="rank-only"
        learning_status_next_step="rerank only if evidence is stronger than the current candidate order"
    fi
}

emit_debug_summary() {
    local item
    compute_learning_status
    printf 'wrapper-debug\tpreedit\t%s\n' "$preedit"
    printf 'wrapper-debug\tsupervision-mode\t%s\n' "$supervision_mode"
    printf 'wrapper-debug\tinput-mode\t%s\n' "$input_mode"
    printf 'wrapper-debug\tsupervision-state\t%s\n' "$supervision_state"
    printf 'wrapper-debug\tlearning-status\tprimary\t%s\tnext-step\t%s\n' \
        "$learning_status_primary" "$learning_status_next_step"
    for item in "${learning_status_suggested_protocols[@]}"; do
        printf 'wrapper-debug\tlearning-status-suggested-protocol\t%s\n' "$item"
    done
    for item in "${learning_status_evidence_protocols[@]}"; do
        printf 'wrapper-debug\tlearning-status-evidence-protocol\t%s\n' "$item"
    done
    for item in "${learning_status_awaiting_suffix[@]}"; do
        printf 'wrapper-debug\tlearning-status-awaiting-suffix\t%s\n' "$item"
    done
    for item in "${learning_status_signal_counts[@]}"; do
        printf 'wrapper-debug\tlearning-status-signal-count\t%s\n' "$item"
    done
    printf 'wrapper-debug\tcandidates\t%s\n' "${#candidates[@]}"
    for item in "${candidate_metadata[@]}"; do
        printf 'wrapper-debug\tcandidate-metadata\t%s\n' "$item"
    done
    for item in "${pending_segments[@]}"; do
        printf 'wrapper-debug\tpending-segment\t%s\n' "$item"
    done
    printf 'wrapper-debug\tpreedit-leading-context\tactive\t%s\tevents\t%s\n' \
        "$preedit_leading_active" "$preedit_leading_events_before_preedit"
    for item in "${preedit_leading_events[@]}"; do
        printf 'wrapper-debug\tpreedit-leading-event\t%s\n' "$item"
    done
    for item in "${preedit_leading_event_counts[@]}"; do
        printf 'wrapper-debug\tpreedit-leading-event-count\t%s\n' "$item"
    done
    printf 'wrapper-debug\tedit-summary\tcurrent\t%s\tcursor\t%s\ttyped-tail\t%s\tlast-erased\t%s\tlast-edited\t%s\tmiddle-edit\t%s\n' \
        "$edit_summary_current" "$edit_summary_cursor" "$edit_summary_typed_tail" \
        "$edit_summary_last_erased" "$edit_summary_last_edited" "$edit_summary_middle_edit"
    for item in "${correction_patterns[@]}"; do
        printf 'wrapper-debug\tcorrection-pattern\t%s\n' "$item"
    done
    for item in "${realtime_correction_decisions[@]}"; do
        printf 'wrapper-debug\trealtime-correction\t%s\n' "$item"
    done
    printf 'wrapper-debug\trecent-history\tsummary\tavailable\t%s\trecords\t%s\tactive\t%s\tpass-through\t%s\tsegment-chains\t%s\tpending-segments\t%s\tpath\t%s\n' \
        "$recent_history_available" "$recent_history_records" "$recent_history_active_records" \
        "$recent_history_pass_through_records" "$recent_history_segment_chains" \
        "$recent_history_pending_segments" "$recent_history_path"
    for item in "${recent_history_preedits[@]}"; do
        printf 'wrapper-debug\trecent-history-preedit\t%s\n' "$item"
    done
    for item in "${recent_history_selected_candidates[@]}"; do
        printf 'wrapper-debug\trecent-history-selected-candidate\t%s\n' "$item"
    done
    for item in "${recent_history_preedit_selected_pairs[@]}"; do
        printf 'wrapper-debug\trecent-history-preedit-selected\t%s\n' "$item"
    done
    for item in "${recent_history_applications[@]}"; do
        printf 'wrapper-debug\trecent-history-application\t%s\n' "$item"
    done
    for item in "${recent_history_event_counts[@]}"; do
        printf 'wrapper-debug\trecent-history-event-count\t%s\n' "$item"
    done
    for item in "${recent_history_active_event_counts[@]}"; do
        printf 'wrapper-debug\trecent-history-active-event-count\t%s\n' "$item"
    done
    for item in "${recent_history_pass_through_event_counts[@]}"; do
        printf 'wrapper-debug\trecent-history-pass-through-event-count\t%s\n' "$item"
    done
    for item in "${recent_history_correction_event_counts[@]}"; do
        printf 'wrapper-debug\trecent-history-correction-event-count\t%s\n' "$item"
    done
    for item in "${recent_history_corrections[@]}"; do
        printf 'wrapper-debug\trecent-history-correction\t%s\n' "$item"
    done
}

# Generic starter behavior: replay confirmed segment-chain evidence when it
# already points at a current candidate, promote and persist a non-leading
# selected full-preedit candidate, and turn a pending prefix selection plus an
# explicitly selected non-leading suffix candidate into a segment_chain learning row. Replace
# or extend this block with a local model, cloud API, or custom rules that
# consume the supervised fields above instead of hardcoding example pinyin
# strings. recent_history_* variables expose a bounded cross-request behavior
# summary for click-triggered local/cloud models. When supervision_mode=
# pass-through-only, TiPE is showing read-only keyboard behavior context. In
# English input mode it may include a bounded raw token, but it is not an active
# candidate-generation request. learning_status_* variables
# are computed before this point so custom logic can branch on whether the
# request is ready-to-learn, awaiting-suffix-confirmation, rank-only, or
# keyboard-context-only.
compute_learning_status
if [[ "${TIPE_WRAPPER_DEBUG_SUMMARY:-0}" == "1" ]]; then
    emit_debug_summary
    exit 0
fi

emit_matching_segment_chain_candidate
emit_selected_candidate_preference
emit_pending_segment_confirmation_chain
EOF
}

if [[ "$dry_run" == "1" ]]; then
    template
    if [[ "$configure" == "1" ]]; then
        printf '# Would configure TiPE custom model wrapper: %s\n' "$output_path"
        if [[ -n "$config_path" ]]; then
            printf '# Would write model config: %s\n' "$config_path"
        fi
    fi
    exit 0
fi

mkdir -p "$(dirname -- "$output_path")"
temporary_path="${output_path}.tmp.$$"
template >"$temporary_path"
chmod 0755 "$temporary_path"
mv -f "$temporary_path" "$output_path"
echo "created	$output_path"

if [[ "$configure" == "1" ]]; then
    script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
    model_config="$script_dir/model-config.sh"
    if [[ ! -x "$model_config" ]]; then
        echo "cannot find executable model-config helper: $model_config" >&2
        exit 1
    fi
    if [[ -n "$config_path" ]]; then
        TIPE_MODEL_CONFIG="$config_path" "$model_config" --write custom --command "$output_path"
    else
        "$model_config" --write custom --command "$output_path"
    fi
fi
