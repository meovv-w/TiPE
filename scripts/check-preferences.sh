#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 [--explain] [--summary] [--preedit TEXT] [--query-only] [--top N] [PATH]

Validate a TiPE candidate preference TSV file.
Use --explain to print valid candidate preference, correction, and segment-chain rows.
Use --summary to print aggregate learning counts, correction-pattern habits, and the strongest learned rows.
Use --preedit TEXT to print learned rows that can affect or explain one input string.
Use --query-only with --preedit for trusted UI refreshes that validate only matching rows.
For --preedit, query-effect rows describe the direct next ranking/correction effect;
query-inactive-evidence rows are valid history that has not reached its activation threshold.
EOF
}

default_preferences_path() {
    if [[ -n "${XDG_DATA_HOME:-}" ]]; then
        printf '%s\n' "$XDG_DATA_HOME/tipe/candidate-preferences.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.local/share/tipe/candidate-preferences.tsv"
    else
        return 1
    fi
}

safe_text_error() {
    local field_name="$1"
    local value="$2"
    if [[ -z "$value" ]]; then
        printf '%s field is empty\n' "$field_name"
        return 0
    fi
    if [[ "$value" == *$'\t'* || "$value" == *$'\r'* || "$value" == *$'\n'* ]]; then
        printf '%s must not contain TAB or newlines\n' "$field_name"
        return 0
    fi
}

count_error() {
    local value="$1"
    local max_count=1000000
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        printf 'count should be a positive integer: %s\n' "$value"
        return 0
    fi
    if (( ${#value} > ${#max_count} )) ||
        { (( ${#value} == ${#max_count} )) && [[ "$value" > "$max_count" ]]; }; then
        printf 'count exceeds TiPE learning limit %s: %s\n' "$max_count" "$value"
    fi
}

learned_edit_error() {
    local kind="$1"
    local typed="$2"
    local replacement="$3"
    local expected_typed expected_replacement
    case "$kind" in
        missing) expected_typed=0; expected_replacement=1 ;;
        extra) expected_typed=1; expected_replacement=0 ;;
        replace) expected_typed=1; expected_replacement=1 ;;
        transpose) expected_typed=2; expected_replacement=2 ;;
        *) printf 'unknown learned edit kind: %s\n' "$kind"; return 0 ;;
    esac
    if [[ -n "$typed" && "$typed" == *[!A-Za-z0-9]* ]] ||
        [[ -n "$replacement" && "$replacement" == *[!A-Za-z0-9]* ]]; then
        echo "learned edit text must use ASCII letters or digits"
        return 0
    fi
    if (( ${#typed} != expected_typed || ${#replacement} != expected_replacement )) ||
        [[ "$typed" == "$replacement" ]]; then
        printf 'invalid %s edit shape\n' "$kind"
    fi
}

correction_pattern_activation_count() {
    case "$1" in
        missing|transpose) echo 2 ;;
        extra) echo 3 ;;
        replace) echo 4 ;;
        *) echo 1000001 ;;
    esac
}

key_habit_activation_count() {
    case "$1" in
        transpose) echo 3 ;;
        missing) echo 5 ;;
        extra|replace) echo 6 ;;
        *) echo 1000001 ;;
    esac
}

plausible_correction_error() {
    local typo="$1"
    local corrected="$2"
    if ! command -v python3 >/dev/null 2>&1; then
        echo "python3 is required to validate correction plausibility"
        return 0
    fi
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

if plausible(typo, corrected):
    sys.exit(0)
print(f"correction is not plausible: {typo} -> {corrected}")
PY
}

segment_chain_shape_error() {
    local original="$1"
    local consumed="$2"
    local committed="$3"
    local remaining="$4"
    local corrected_full="$5"
    local combined="$6"
    if [[ "$combined" != "$committed"* ]]; then
        printf 'segment-chain combined candidate does not start with committed text: %s -> %s\n' "$committed" "$combined"
        return 0
    fi
    if [[ "$consumed$remaining" != "$original" && "$consumed$remaining" != "$corrected_full" ]]; then
        printf 'segment-chain consumed+remaining is not original or corrected preedit: %s + %s\n' "$consumed" "$remaining"
        return 0
    fi
    return 1
}

correction_pattern_key() {
    local typo="$1"
    local corrected="$2"
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - "$typo" "$corrected" <<'PY'
import sys

typo = sys.argv[1]
corrected = sys.argv[2]

if len(corrected) == len(typo) + 1:
    for index, ch in enumerate(corrected):
        if corrected[:index] + corrected[index + 1:] == typo:
            offset = len(typo) - index
            position = f"end-{offset}" if offset <= 2 else str(index)
            print(f"missing\t{ch}\t{position}")
            sys.exit(0)
elif len(typo) == len(corrected) + 1:
    for index, ch in enumerate(typo):
        if typo[:index] + typo[index + 1:] == corrected:
            offset = len(typo) - index - 1
            position = f"end-{offset}" if offset <= 1 else str(index)
            print(f"extra\t{ch}\t{position}")
            sys.exit(0)
elif len(typo) == len(corrected):
    diffs = [(index, wrong, right) for index, (wrong, right) in enumerate(zip(typo, corrected)) if wrong != right]
    if len(diffs) == 1:
        index, wrong, right = diffs[0]
        offset = len(typo) - index - 1
        position = f"end-{offset}" if offset <= 1 else str(index)
        print(f"replace\t{wrong}->{right}\t{position}")
        sys.exit(0)
    if (
        len(diffs) == 2
        and diffs[1][0] == diffs[0][0] + 1
        and diffs[0][1] == diffs[1][2]
        and diffs[1][1] == diffs[0][2]
    ):
        index = diffs[0][0]
        offset = len(typo) - index - 2
        position = f"end-{offset}" if offset <= 1 else str(index)
        print(f"transpose\t{typo[index:index + 2]}->{corrected[index:index + 2]}\t{position}")
        sys.exit(0)
sys.exit(1)
PY
}

looks_like_english_identifier() {
    local value="$1"
    local lowered
    lowered="${value,,}"
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

validate_preferences() {
    local preferences_path="$1"
    local explain="$2"
    local summary="$3"
    local query_preedit="$4"
    local top_limit="$5"
    local query_only="$6"

    if [[ ! -e "$preferences_path" ]]; then
        echo "TiPE preferences not found: $preferences_path"
        return 0
    fi

    local line line_number kind preedit candidate count typo corrected message pattern_key pattern_count correction_pattern_total
    local typed replacement position relative runtime_status activation_required
    local original consumed committed remaining corrected_full combined
    local preference_status activation_count preference_requirement
    local -a fields
    local preference_rows=0 legacy_rows=0 raw_token_rows=0 correction_rows=0 segment_chain_rows=0
    local runtime_pattern_rows=0 runtime_habit_rows=0 runtime_pattern_total=0 runtime_habit_total=0
    local runtime_pattern_active=0 runtime_habit_active=0
    local active_preference_rows=0 inactive_preference_rows=0 active_legacy_rows=0 inactive_legacy_rows=0
    local preference_total=0 legacy_total=0 raw_token_total=0 correction_total=0 segment_chain_total=0
    local query_preference_rows=0 query_legacy_rows=0 query_raw_token_rows=0 query_correction_rows=0 query_segment_chain_rows=0
    local query_active_preferences=0 query_inactive_preferences=0
    local -a top_preferences=() top_legacy=() top_inactive_preferences=() top_inactive_legacy=()
    local -a top_raw_tokens=() top_corrections=() top_segment_chains=() top_correction_patterns=()
    local -a top_runtime_patterns=() top_runtime_habits=()
    local -A correction_pattern_counts=()
    line_number=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        line_number=$((line_number + 1))
        line="${line%$'\r'}"
        [[ -z "$line" || "${line:0:1}" == "#" ]] && continue

        readarray -td $'\t' fields < <(printf '%s\t' "$line")
        if [[ "${fields[0]:-}" == "__correction_pattern__" ]]; then
            if [[ ${#fields[@]} -ne 7 ]]; then
                echo "$preferences_path:$line_number: runtime correction-pattern row requires 7 TAB-separated fields" >&2
                return 1
            fi
            kind="${fields[1]}"
            typed="${fields[2]}"
            replacement="${fields[3]}"
            position="${fields[4]}"
            relative="${fields[5]}"
            count="${fields[6]}"
            if message=$(learned_edit_error "$kind" "$typed" "$replacement"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if [[ ! "$position" =~ ^(0|[1-9][0-9]*)$ ]] || (( position > 63 )); then
                echo "$preferences_path:$line_number: runtime correction-pattern position must be 0..63" >&2
                return 1
            fi
            if [[ "$relative" != 0 && "$relative" != 1 ]]; then
                echo "$preferences_path:$line_number: runtime correction-pattern relative flag must be 0 or 1" >&2
                return 1
            fi
            if message=$(count_error "$count"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            activation_required=$(correction_pattern_activation_count "$kind")
            runtime_status=$([[ "$count" -ge "$activation_required" ]] && echo active || echo inactive-evidence)
            [[ "$explain" != 1 ]] || printf 'runtime-correction-pattern\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\trequires\t%s\n' \
                "$line_number" "$kind" "$typed" "$replacement" "$position" "$relative" "$count" \
                "$runtime_status" "$activation_required"
            if [[ "$summary" == 1 ]]; then
                runtime_pattern_rows=$((runtime_pattern_rows + 1))
                runtime_pattern_total=$((runtime_pattern_total + count))
                [[ "$runtime_status" != active ]] || runtime_pattern_active=$((runtime_pattern_active + 1))
                top_runtime_patterns+=("$count"$'\t'"$kind"$'\t'"$typed"$'\t'"$replacement"$'\t'"$position"$'\t'"$relative"$'\t'"$runtime_status"$'\t'"$activation_required")
            fi
            if [[ -n "$query_preedit" ]]; then
                printf 'query-runtime-correction-pattern\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\trequires\t%s\n' \
                    "$line_number" "$kind" "$typed" "$replacement" "$position" "$relative" "$count" \
                    "$runtime_status" "$activation_required"
            fi
            continue
        fi
        if [[ "${fields[0]:-}" == "__key_habit__" ]]; then
            if [[ ${#fields[@]} -ne 5 ]]; then
                echo "$preferences_path:$line_number: runtime key-habit row requires 5 TAB-separated fields" >&2
                return 1
            fi
            kind="${fields[1]}"
            typed="${fields[2]}"
            replacement="${fields[3]}"
            count="${fields[4]}"
            if message=$(learned_edit_error "$kind" "$typed" "$replacement"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if message=$(count_error "$count"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            activation_required=$(key_habit_activation_count "$kind")
            runtime_status=$([[ "$count" -ge "$activation_required" ]] && echo active || echo inactive-evidence)
            [[ "$explain" != 1 ]] || printf 'runtime-key-habit\t%s\t%s\t%s\t%s\t%s\t%s\trequires\t%s\n' \
                "$line_number" "$kind" "$typed" "$replacement" "$count" "$runtime_status" "$activation_required"
            if [[ "$summary" == 1 ]]; then
                runtime_habit_rows=$((runtime_habit_rows + 1))
                runtime_habit_total=$((runtime_habit_total + count))
                [[ "$runtime_status" != active ]] || runtime_habit_active=$((runtime_habit_active + 1))
                top_runtime_habits+=("$count"$'\t'"$kind"$'\t'"$typed"$'\t'"$replacement"$'\t'"$runtime_status"$'\t'"$activation_required")
            fi
            if [[ -n "$query_preedit" ]]; then
                printf 'query-runtime-key-habit\t%s\t%s\t%s\t%s\t%s\t%s\trequires\t%s\n' \
                    "$line_number" "$kind" "$typed" "$replacement" "$count" "$runtime_status" "$activation_required"
            fi
            continue
        fi
        if [[ "${fields[0]:-}" == "__raw_token__" ]]; then
            if [[ ${#fields[@]} -ne 3 ]]; then
                echo "$preferences_path:$line_number: supervised raw-token row requires 3 TAB-separated fields" >&2
                return 1
            fi
            preedit="${fields[1]}"
            count="${fields[2]}"
            if [[ "$query_only" == 1 && "$preedit" != "$query_preedit" ]]; then
                continue
            fi
            if [[ ! "$preedit" =~ ^[a-z]{2,}$ ]]; then
                echo "$preferences_path:$line_number: supervised raw token must be lowercase ASCII letters" >&2
                return 1
            fi
            if message=$(count_error "$count"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            preference_status="inactive-evidence"
            if (( count >= 3 )); then
                preference_status="active"
            fi
            if [[ "$explain" == 1 ]]; then
                printf 'supervised-raw-token\t%s\t%s\t%s\t%s\n' \
                    "$line_number" "$preedit" "$count" "$preference_status"
            fi
            if [[ "$summary" == 1 ]]; then
                raw_token_rows=$((raw_token_rows + 1))
                raw_token_total=$((raw_token_total + count))
                top_raw_tokens+=("$count"$'\t'"$preedit")
            fi
            if [[ -n "$query_preedit" && "$preedit" == "$query_preedit" ]]; then
                query_raw_token_rows=$((query_raw_token_rows + 1))
                printf 'query-supervised-raw-token\t%s\t%s\t%s\t%s\n' \
                    "$line_number" "$preedit" "$count" "$preference_status"
                if [[ "$preference_status" == "active" ]]; then
                    printf 'query-effect\tsupervised-raw-token\t%s\t%s\t%s\n' \
                        "$line_number" "$preedit" "$count"
                else
                    printf 'query-inactive-evidence\tsupervised-raw-token\t%s\t%s\t%s\trequires\t3\n' \
                        "$line_number" "$preedit" "$count"
                fi
            fi
            continue
        fi
        if [[ "${fields[0]:-}" == "__correction__" ]]; then
            if [[ ${#fields[@]} -ne 4 ]]; then
                echo "$preferences_path:$line_number: correction row requires 4 TAB-separated fields" >&2
                return 1
            fi
            kind="${fields[0]}"
            typo="${fields[1]}"
            corrected="${fields[2]}"
            count="${fields[3]}"
            if [[ "$query_only" == 1 && "$typo" != "$query_preedit" && "$corrected" != "$query_preedit" ]]; then
                continue
            fi
            if message=$(safe_text_error "typo" "$typo"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if message=$(safe_text_error "corrected" "$corrected"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if message=$(count_error "$count"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if message=$(plausible_correction_error "$typo" "$corrected"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if [[ "$explain" == 1 ]]; then
                printf 'correction\t%s\t%s\t%s\t%s\n' "$line_number" "$typo" "$corrected" "$count"
            fi
            if [[ "$summary" == 1 ]]; then
                correction_rows=$((correction_rows + 1))
                correction_total=$((correction_total + count))
                top_corrections+=("$count"$'\t'"$typo"$'\t'"$corrected")
                if pattern_key=$(correction_pattern_key "$typo" "$corrected"); then
                    correction_pattern_counts["$pattern_key"]=$(( ${correction_pattern_counts["$pattern_key"]:-0} + count ))
                fi
            fi
            if [[ -n "$query_preedit" && ( "$typo" == "$query_preedit" || "$corrected" == "$query_preedit" ) ]]; then
                query_correction_rows=$((query_correction_rows + 1))
                if [[ "$typo" == "$query_preedit" ]]; then
                    printf 'query-correction\t%s\ttypo\t%s\t%s\t%s\n' "$line_number" "$typo" "$corrected" "$count"
                    printf 'query-effect\tcorrection-borrow\t%s\t%s\t%s\t%s\n' "$line_number" "$typo" "$corrected" "$count"
                else
                    printf 'query-correction\t%s\tcorrected\t%s\t%s\t%s\n' "$line_number" "$typo" "$corrected" "$count"
                    printf 'query-effect\tcorrection-target\t%s\t%s\t%s\t%s\n' "$line_number" "$typo" "$corrected" "$count"
                fi
            fi
            continue
        fi

        if [[ "${fields[0]:-}" == "__segment_chain__" ]]; then
            if [[ ${#fields[@]} -ne 8 ]]; then
                echo "$preferences_path:$line_number: segment-chain row requires 8 TAB-separated fields" >&2
                return 1
            fi
            original="${fields[1]}"
            consumed="${fields[2]}"
            committed="${fields[3]}"
            remaining="${fields[4]}"
            corrected_full="${fields[5]}"
            combined="${fields[6]}"
            count="${fields[7]}"
            if [[ "$query_only" == 1 && "$original" != "$query_preedit" &&
                "$remaining" != "$query_preedit" && "$corrected_full" != "$query_preedit" ]]; then
                continue
            fi
            for field_pair in \
                "original preedit:$original" \
                "consumed preedit:$consumed" \
                "committed text:$committed" \
                "remaining preedit:$remaining" \
                "corrected full preedit:$corrected_full" \
                "combined candidate:$combined"; do
                field_name="${field_pair%%:*}"
                field_value="${field_pair#*:}"
                if message=$(safe_text_error "$field_name" "$field_value"); [[ -n "$message" ]]; then
                    echo "$preferences_path:$line_number: $message" >&2
                    return 1
                fi
            done
            if message=$(count_error "$count"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if [[ "$corrected_full" != "$original" ]]; then
                if message=$(plausible_correction_error "$original" "$corrected_full"); [[ -n "$message" ]]; then
                    echo "$preferences_path:$line_number: segment-chain $message" >&2
                    return 1
                fi
            fi
            if message=$(segment_chain_shape_error "$original" "$consumed" "$committed" "$remaining" "$corrected_full" "$combined"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if [[ "$explain" == 1 ]]; then
                printf 'segment-chain\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$line_number" "$original" "$consumed" \
                    "$committed" "$remaining" "$corrected_full" "$combined" "$count"
            fi
            if [[ "$summary" == 1 ]]; then
                segment_chain_rows=$((segment_chain_rows + 1))
                segment_chain_total=$((segment_chain_total + count))
                top_segment_chains+=("$count"$'\t'"$original"$'\t'"$combined")
            fi
            if [[ -n "$query_preedit" &&
                ( "$original" == "$query_preedit" || "$remaining" == "$query_preedit" || "$corrected_full" == "$query_preedit" ) ]]; then
                query_segment_chain_rows=$((query_segment_chain_rows + 1))
                printf 'query-segment-chain\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$line_number" "$original" "$consumed" \
                    "$committed" "$remaining" "$corrected_full" "$combined" "$count"
                if [[ "$remaining" == "$query_preedit" ]]; then
                    printf 'query-effect\tsegment-chain-suffix\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$line_number" \
                        "$original" "$consumed" "$committed" "$remaining" "$corrected_full" "$combined" "$count"
                elif [[ "$original" == "$query_preedit" ]]; then
                    printf 'query-effect\tsegment-chain-full\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$line_number" \
                        "$original" "$consumed" "$committed" "$remaining" "$corrected_full" "$combined" "$count"
                else
                    printf 'query-effect\tsegment-chain-corrected-full\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$line_number" \
                        "$original" "$consumed" "$committed" "$remaining" "$corrected_full" "$combined" "$count"
                fi
            fi
            continue
        fi

        if [[ ${#fields[@]} -eq 2 ]]; then
            preedit="${fields[0]}"
            count="${fields[1]}"
            if [[ "$query_only" == 1 && "$preedit" != "$query_preedit" ]]; then
                continue
            fi
            if message=$(safe_text_error "legacy key" "$preedit"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if message=$(count_error "$count"); [[ -n "$message" ]]; then
                echo "$preferences_path:$line_number: $message" >&2
                return 1
            fi
            if (( count >= 3 )); then
                preference_status="active"
                active_legacy_rows=$((active_legacy_rows + 1))
            else
                preference_status="inactive-evidence"
                inactive_legacy_rows=$((inactive_legacy_rows + 1))
            fi
            if [[ "$explain" == 1 ]]; then
                printf 'legacy-preference\t%s\t%s\t%s\t%s\n' "$line_number" "$preedit" "$count" "$preference_status"
            fi
            if [[ "$summary" == 1 ]]; then
                legacy_rows=$((legacy_rows + 1))
                legacy_total=$((legacy_total + count))
                if [[ "$preference_status" == "active" ]]; then
                    top_legacy+=("$count"$'\t'"$preedit")
                else
                    top_inactive_legacy+=("$count"$'\t'"$preedit")
                fi
            fi
            if [[ -n "$query_preedit" && "$preedit" == "$query_preedit" ]]; then
                query_legacy_rows=$((query_legacy_rows + 1))
                printf 'query-legacy-preference\t%s\t%s\t%s\t%s\n' "$line_number" "$preedit" "$count" "$preference_status"
                if [[ "$preference_status" == "active" ]]; then
                    query_active_preferences=$((query_active_preferences + 1))
                    printf 'query-effect\tlegacy-raw-preference\t%s\t%s\t%s\n' "$line_number" "$preedit" "$count"
                else
                    query_inactive_preferences=$((query_inactive_preferences + 1))
                    printf 'query-inactive-evidence\tlegacy-raw-preference\t%s\t%s\t%s\trequires\t3\n' \
                        "$line_number" "$preedit" "$count"
                fi
            fi
            continue
        fi

        if [[ ${#fields[@]} -ne 3 ]]; then
            echo "$preferences_path:$line_number: preference row requires 2 or 3 TAB-separated fields" >&2
            return 1
        fi
        preedit="${fields[0]}"
        candidate="${fields[1]}"
        count="${fields[2]}"
        if [[ "$query_only" == 1 && "$preedit" != "$query_preedit" ]]; then
            continue
        fi
        if message=$(safe_text_error "preedit" "$preedit"); [[ -n "$message" ]]; then
            echo "$preferences_path:$line_number: $message" >&2
            return 1
        fi
        if message=$(safe_text_error "candidate" "$candidate"); [[ -n "$message" ]]; then
            echo "$preferences_path:$line_number: $message" >&2
            return 1
        fi
        if message=$(count_error "$count"); [[ -n "$message" ]]; then
            echo "$preferences_path:$line_number: $message" >&2
            return 1
        fi
        if [[ "$candidate" == "$preedit" ]]; then
            activation_count=3
        else
            activation_count=2
        fi
        preference_requirement="$activation_count"
        if [[ "$candidate" == "$preedit" ]] && ! looks_like_english_identifier "$preedit"; then
            preference_status="inactive-evidence"
            preference_requirement="english-identifier"
            inactive_preference_rows=$((inactive_preference_rows + 1))
        elif (( count >= activation_count )); then
            preference_status="active"
            active_preference_rows=$((active_preference_rows + 1))
        else
            preference_status="inactive-evidence"
            inactive_preference_rows=$((inactive_preference_rows + 1))
        fi
        if [[ "$explain" == 1 ]]; then
            printf 'preference\t%s\t%s\t%s\t%s\t%s\n' \
                "$line_number" "$preedit" "$candidate" "$count" "$preference_status"
        fi
        if [[ "$summary" == 1 ]]; then
            preference_rows=$((preference_rows + 1))
            preference_total=$((preference_total + count))
            if [[ "$preference_status" == "active" ]]; then
                top_preferences+=("$count"$'\t'"$preedit"$'\t'"$candidate")
            else
                top_inactive_preferences+=("$count"$'\t'"$preedit"$'\t'"$candidate")
            fi
        fi
        if [[ -n "$query_preedit" && "$preedit" == "$query_preedit" ]]; then
            query_preference_rows=$((query_preference_rows + 1))
            printf 'query-preference\t%s\t%s\t%s\t%s\t%s\n' \
                "$line_number" "$preedit" "$candidate" "$count" "$preference_status"
            if [[ "$preference_status" == "active" ]]; then
                query_active_preferences=$((query_active_preferences + 1))
                if [[ "$candidate" == "$preedit" ]]; then
                    printf 'query-effect\traw-preference\t%s\t%s\t%s\t%s\n' "$line_number" "$preedit" "$candidate" "$count"
                else
                    printf 'query-effect\trank-preference\t%s\t%s\t%s\t%s\n' "$line_number" "$preedit" "$candidate" "$count"
                fi
            else
                query_inactive_preferences=$((query_inactive_preferences + 1))
                if [[ "$candidate" == "$preedit" ]]; then
                    printf 'query-inactive-evidence\traw-preference\t%s\t%s\t%s\t%s\trequires\t%s\n' \
                        "$line_number" "$preedit" "$candidate" "$count" "$preference_requirement"
                else
                    printf 'query-inactive-evidence\trank-preference\t%s\t%s\t%s\t%s\trequires\t%s\n' \
                        "$line_number" "$preedit" "$candidate" "$count" "$activation_count"
                fi
            fi
        fi
    done <"$preferences_path"

    if [[ "$summary" == 1 ]]; then
        correction_pattern_total=0
        for pattern_key in "${!correction_pattern_counts[@]}"; do
            pattern_count="${correction_pattern_counts["$pattern_key"]}"
            correction_pattern_total=$((correction_pattern_total + pattern_count))
            top_correction_patterns+=("$pattern_count"$'\t'"$pattern_key")
        done
        printf 'summary\trows\t%s\n' "$((preference_rows + legacy_rows + raw_token_rows + correction_rows + segment_chain_rows + runtime_pattern_rows + runtime_habit_rows))"
        printf 'summary\tpreferences\t%s\t%s\n' "$preference_rows" "$preference_total"
        printf 'summary\tlegacy-preferences\t%s\t%s\n' "$legacy_rows" "$legacy_total"
        printf 'summary\tsupervised-raw-tokens\t%s\t%s\n' "$raw_token_rows" "$raw_token_total"
        printf 'summary\tpreference-evidence\tactive\t%s\tinactive\t%s\n' \
            "$((active_preference_rows + active_legacy_rows))" \
            "$((inactive_preference_rows + inactive_legacy_rows))"
        printf 'summary\tcorrections\t%s\t%s\n' "$correction_rows" "$correction_total"
        printf 'summary\tcorrection-patterns\t%s\t%s\n' "${#correction_pattern_counts[@]}" "$correction_pattern_total"
        printf 'summary\truntime-correction-patterns\t%s\t%s\tactive\t%s\n' \
            "$runtime_pattern_rows" "$runtime_pattern_total" "$runtime_pattern_active"
        printf 'summary\truntime-key-habits\t%s\t%s\tactive\t%s\n' \
            "$runtime_habit_rows" "$runtime_habit_total" "$runtime_habit_active"
        printf 'summary\tsegment-chains\t%s\t%s\n' "$segment_chain_rows" "$segment_chain_total"
        print_top_rows "top-preference" "$top_limit" "${top_preferences[@]}"
        print_top_rows "top-legacy-preference" "$top_limit" "${top_legacy[@]}"
        print_top_rows "top-inactive-preference" "$top_limit" "${top_inactive_preferences[@]}"
        print_top_rows "top-inactive-legacy-preference" "$top_limit" "${top_inactive_legacy[@]}"
        print_top_rows "top-supervised-raw-token" "$top_limit" "${top_raw_tokens[@]}"
        print_top_rows "top-correction" "$top_limit" "${top_corrections[@]}"
        print_top_rows "top-correction-pattern" "$top_limit" "${top_correction_patterns[@]}"
        print_top_rows "top-runtime-correction-pattern" "$top_limit" "${top_runtime_patterns[@]}"
        print_top_rows "top-runtime-key-habit" "$top_limit" "${top_runtime_habits[@]}"
        print_top_rows "top-segment-chain" "$top_limit" "${top_segment_chains[@]}"
    fi

    if [[ -n "$query_preedit" ]]; then
        printf 'query-summary\t%s\tpreferences\t%s\tlegacy-preferences\t%s\tcorrections\t%s\tsegment-chains\t%s\tactive-preferences\t%s\tinactive-preferences\t%s\tsupervised-raw-tokens\t%s\n' \
            "$query_preedit" "$query_preference_rows" "$query_legacy_rows" "$query_correction_rows" \
            "$query_segment_chain_rows" "$query_active_preferences" "$query_inactive_preferences" \
            "$query_raw_token_rows"
    fi

    echo "TiPE preferences ok: $preferences_path"
}

explain=0
summary=0
query_preedit=""
query_only=0
top_limit=5
preferences_path=""

print_top_rows() {
    local label="$1"
    local limit="$2"
    shift 2
    if [[ $# -eq 0 || "$limit" -le 0 ]]; then
        return 0
    fi
    local rank=0
    local row
    while IFS= read -r row; do
        [[ -z "$row" ]] && continue
        rank=$((rank + 1))
        if [[ "$rank" -le "$limit" ]]; then
            printf '%s\t%s\t%s\n' "$label" "$rank" "$row"
        fi
    done < <(printf '%s\n' "$@" | sort -t $'\t' -k1,1nr -k2,2 -k3,3)
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --explain)
            explain=1
            shift
            ;;
        --summary)
            summary=1
            shift
            ;;
        --preedit)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "--preedit requires a value" >&2
                exit 2
            fi
            query_preedit="$2"
            shift 2
            ;;
        --query-only)
            query_only=1
            shift
            ;;
        --top)
            if [[ $# -lt 2 || ! "$2" =~ ^[0-9]+$ ]]; then
                echo "--top requires a non-negative integer" >&2
                exit 2
            fi
            top_limit="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
        *)
            if [[ -n "$preferences_path" ]]; then
                echo "usage: $0 [--explain] [--summary] [--top N] [PATH]" >&2
                exit 2
            fi
            preferences_path="$1"
            shift
            ;;
    esac
done

if [[ -n "$query_preedit" ]]; then
    if message=$(safe_text_error "query preedit" "$query_preedit"); [[ -n "$message" ]]; then
        echo "$message" >&2
        exit 2
    fi
fi
if [[ "$query_only" == 1 && -z "$query_preedit" ]]; then
    echo "--query-only requires --preedit" >&2
    exit 2
fi
if [[ "$query_only" == 1 && ( "$summary" == 1 || "$explain" == 1 ) ]]; then
    echo "--query-only cannot be combined with --summary or --explain" >&2
    exit 2
fi

if [[ -z "$preferences_path" ]]; then
    if ! preferences_path=$(default_preferences_path); then
        echo "HOME is not set and no preferences path was provided" >&2
        exit 1
    fi
fi

validate_preferences "$preferences_path" "$explain" "$summary" "$query_preedit" "$top_limit" "$query_only"
