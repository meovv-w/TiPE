#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tipe-model-explain [--panel] [REQUEST_TSV]

Reads a TiPE model request TSV from a file or stdin and prints a compact,
human-readable TSV summary. This is read-only and does not call any model.
Use --panel to also print stable panel rows for a learning/debug UI.
EOF
}

panel_output=0
input=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --panel)
            panel_output=1
            shift
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
            if [[ -n "$input" ]]; then
                usage >&2
                exit 2
            fi
            input="$1"
            shift
            ;;
    esac
done

input="${input:-/dev/stdin}"
if [[ "$input" == "-" ]]; then
    input="/dev/stdin"
fi
if [[ ! -r "$input" ]]; then
    echo "cannot read model request: $input" >&2
    exit 1
fi

preedit_value=""
application_value=""
surrounding_before_value=""
surrounding_after_value=""
events_fields=()
correction_event_fields=()
event_count_fields=()
correction_event_count_fields=()
candidate_count=0
visible_count=0
numbered_count=0
context_count=0
context_fields=()
segment_chain_rows=0
segment_chain_fields=()
pending_segment_rows=0
pending_segment_fields=()
preference_rows=0
correction_rows=0
preference_total=0
correction_total=0
top_preferences=()
top_corrections=()
correction_pattern_rows=()
candidates=()
candidate_metadata=()
visible_candidates=()
numbered_candidates=()
selected_candidate_index=""
selected_candidate_text=""
runtime_continuous="0"
runtime_input_mode=""
supervision_state=""
supervision_mode=""
supervision_recent_events=""
supervision_correction_events=""

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

print_top_rows() {
    local label="$1"
    shift
    if [[ $# -eq 0 ]]; then
        return 0
    fi
    local rank=0
    local row
    while IFS= read -r row; do
        [[ -z "$row" ]] && continue
        rank=$((rank + 1))
        printf '%s\t%s\t%s\n' "$label" "$rank" "$row"
        if [[ "$rank" -ge 5 ]]; then
            break
        fi
    done < <(printf '%s\n' "$@" | sort -t $'\t' -k1,1nr -k2,2 -k3,3)
}

print_panel_top_rows() {
    local section="$1"
    shift
    if [[ $# -eq 0 ]]; then
        return 0
    fi
    local rank=0
    local row
    while IFS= read -r row; do
        [[ -z "$row" ]] && continue
        rank=$((rank + 1))
        printf 'panel\t%s\t%s\t%s\n' "$section" "$rank" "$row"
        if [[ "$rank" -ge 5 ]]; then
            break
        fi
    done < <(printf '%s\n' "$@" | sort -t $'\t' -k1,1nr -k2,2 -k3,3)
}

is_prefix_only_candidate() {
    local candidate="$1"
    local item index key_consumed consumed _key_source _source _key_score _score other
    for item in "${candidate_metadata[@]}"; do
        IFS=$'\t' read -r index key_consumed consumed _key_source _source _key_score _score <<< "$item"
        if [[ "$key_consumed" == "consumed_prefix" && "$consumed" =~ ^[1-9][0-9]*$ &&
              "$consumed" -ge "${#preedit_value}" &&
              "$index" =~ ^[0-9]+$ && "${candidates[$index]:-}" == "$candidate" ]]; then
            return 1
        fi
    done
    for item in "${candidate_metadata[@]}"; do
        IFS=$'\t' read -r index key_consumed consumed _key_source _source _key_score _score <<< "$item"
        if [[ "$key_consumed" == "consumed_prefix" && "$consumed" =~ ^[1-9][0-9]*$ &&
              "$consumed" -lt "${#preedit_value}" &&
              "$index" =~ ^[0-9]+$ && "${candidates[$index]:-}" == "$candidate" ]]; then
            return 0
        fi
    done
    if [[ "$preedit_value" =~ ^[A-Za-z]+$ && "${#preedit_value}" -ge 8 &&
          "$candidate" != *[A-Za-z0-9]* && $(( ${#candidate} * 3 )) -lt "${#preedit_value}" ]]; then
        return 0
    fi
    for other in "${candidates[@]}"; do
        if [[ "$other" != "$candidate" && "$other" == "$candidate"* ]]; then
            return 0
        fi
    done
    return 1
}

candidate_is_present() {
    local candidate="$1"
    local item
    for item in "${candidates[@]}"; do
        [[ "$item" == "$candidate" ]] && return 0
    done
    return 1
}

line_number=0
while IFS= read -r line || [[ -n "$line" ]]; do
    ((++line_number))
    line="${line%$'\r'}"
    [[ -z "$line" ]] && continue
    IFS=$'\t' read -r kind rest <<< "$line"
    case "$kind" in
        protocol)
            printf 'protocol\t%s\n' "${rest:-}"
            ;;
        preedit)
            preedit_value="${rest:-}"
            printf 'preedit\t%s\n' "${rest:-}"
            ;;
        application)
            application_value="${rest:-}"
            printf 'application\t%s\n' "${rest:-}"
            ;;
        surrounding_before)
            surrounding_before_value="${rest:-}"
            printf '%s\t%s\n' "$kind" "${rest:-}"
            ;;
        surrounding_after)
            surrounding_after_value="${rest:-}"
            printf '%s\t%s\n' "$kind" "${rest:-}"
            ;;
        candidates)
            if [[ -z "${rest:-}" ]]; then
                printf 'candidate_count\t0\n'
                candidates=()
                continue
            fi
            IFS=$'\t' read -r -a candidates <<< "$rest"
            candidate_count="${#candidates[@]}"
            printf 'candidate_count\t%s\n' "${#candidates[@]}"
            for index in "${!candidates[@]}"; do
                printf 'candidate\t%s\t%s\n' "$index" "${candidates[$index]}"
            done
            ;;
        candidate_metadata)
            candidate_metadata+=("${rest:-}")
            IFS=$'\t' read -r index key_consumed consumed key_source source key_score score <<< "${rest:-}"
            printf 'candidate_metadata\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "${index:-}" "${key_consumed:-}" "${consumed:-}" "${key_source:-}" "${source:-}" \
                "${key_score:-}" "${score:-}"
            ;;
        state)
            printf 'state\t%s\n' "${rest:-}"
            ;;
        runtime_state)
            printf 'runtime_state\t%s\n' "${rest:-}"
            IFS=$'\t' read -r -a runtime_fields <<< "${rest:-}"
            for ((runtime_index = 0; runtime_index + 1 < ${#runtime_fields[@]}; runtime_index += 2)); do
                case "${runtime_fields[$runtime_index]}" in
                    continuous)
                        runtime_continuous="${runtime_fields[$((runtime_index + 1))]}"
                        ;;
                    input_mode)
                        runtime_input_mode="${runtime_fields[$((runtime_index + 1))]}"
                        ;;
                esac
            done
            ;;
        supervision_state)
            supervision_state="${rest:-}"
            printf 'supervision_state\t%s\n' "${rest:-}"
            IFS=$'\t' read -r -a supervision_fields <<< "${rest:-}"
            for ((supervision_index = 0; supervision_index + 1 < ${#supervision_fields[@]}; supervision_index += 2)); do
                case "${supervision_fields[$supervision_index]}" in
                    mode)
                        supervision_mode="${supervision_fields[$((supervision_index + 1))]}"
                        ;;
                    recent_events)
                        supervision_recent_events="${supervision_fields[$((supervision_index + 1))]}"
                        ;;
                    correction_events)
                        supervision_correction_events="${supervision_fields[$((supervision_index + 1))]}"
                        ;;
                esac
            done
            ;;
        selected_candidate)
            IFS=$'\t' read -r index text <<< "${rest:-}"
            selected_candidate_index="${index:-}"
            selected_candidate_text="${text:-}"
            printf 'selected_candidate\t%s\t%s\n' "${index:-}" "${text:-}"
            ;;
        visible_candidates)
            if [[ -z "${rest:-}" ]]; then
                printf 'visible_count\t0\n'
                visible_candidates=()
                continue
            fi
            IFS=$'\t' read -r -a visible <<< "$rest"
            visible_candidates=("${visible[@]}")
            visible_count="${#visible[@]}"
            printf 'visible_count\t%s\n' "${#visible[@]}"
            for item in "${visible[@]}"; do
                IFS=: read -r index text <<< "$item"
                printf 'visible_candidate\t%s\t%s\n' "${index:-}" "${text:-}"
            done
            ;;
        numbered_candidates)
            if [[ -z "${rest:-}" ]]; then
                printf 'numbered_count\t0\n'
                numbered_candidates=()
                continue
            fi
            IFS=$'\t' read -r -a numbered <<< "$rest"
            numbered_candidates=("${numbered[@]}")
            numbered_count="${#numbered[@]}"
            printf 'numbered_count\t%s\n' "${#numbered[@]}"
            for item in "${numbered[@]}"; do
                IFS=: read -r shortcut index text <<< "$item"
                printf 'numbered_candidate\t%s\t%s\t%s\n' "${shortcut:-}" "${index:-}" "${text:-}"
            done
            ;;
        events|correction_events|context)
            if [[ -z "${rest:-}" ]]; then
                printf '%s_count\t0\n' "$kind"
                if [[ "$kind" == "events" ]]; then
                    events_fields=()
                elif [[ "$kind" == "correction_events" ]]; then
                    correction_event_fields=()
                fi
                continue
            fi
            IFS=$'\t' read -r -a fields <<< "$rest"
            if [[ "$kind" == "events" ]]; then
                events_fields=("${fields[@]}")
            elif [[ "$kind" == "correction_events" ]]; then
                correction_event_fields=("${fields[@]}")
            elif [[ "$kind" == "context" ]]; then
                context_fields=("${fields[@]}")
                context_count="${#fields[@]}"
            fi
            printf '%s_count\t%s\n' "$kind" "${#fields[@]}"
            declare -A kind_counts=()
            for index in "${!fields[@]}"; do
                printf '%s_item\t%s\t%s\n' "$kind" "$index" "${fields[$index]}"
                if [[ "$kind" == "events" || "$kind" == "correction_events" ]]; then
                    event_kind="${fields[$index]%%:*}"
                    [[ -n "$event_kind" ]] && kind_counts["$event_kind"]=$(( ${kind_counts["$event_kind"]:-0} + 1 ))
                fi
            done
            if [[ "$kind" == "events" || "$kind" == "correction_events" ]]; then
                for event_kind in "${!kind_counts[@]}"; do
                    printf '%s_kind_count\t%s\t%s\n' "$kind" "$event_kind" "${kind_counts[$event_kind]}"
                done
            fi
            ;;
        event_counts|correction_event_counts)
            if [[ -z "${rest:-}" ]]; then
                printf '%s_count\t0\n' "$kind"
                if [[ "$kind" == "event_counts" ]]; then
                    event_count_fields=()
                else
                    correction_event_count_fields=()
                fi
                continue
            fi
            IFS=$'\t' read -r -a fields <<< "$rest"
            if [[ "$kind" == "event_counts" ]]; then
                event_count_fields=("${fields[@]}")
            else
                correction_event_count_fields=("${fields[@]}")
            fi
            printf '%s_count\t%s\n' "$kind" "${#fields[@]}"
            for item in "${fields[@]}"; do
                IFS=: read -r event_kind event_count <<< "$item"
                printf '%s_kind_count\t%s\t%s\n' "$kind" "${event_kind:-}" "${event_count:-0}"
            done
            ;;
        segment_chain)
            IFS=$'\t' read -r original consumed committed remaining corrected combined <<< "${rest:-}"
            if ! valid_segment_chain_shape "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}" \
                "${corrected:-}" "${combined:-}"; then
                printf 'invalid_segment_chain\t%s\t%s\n' "$line_number" "${rest:-}"
                continue
            fi
            segment_chain_rows=$((segment_chain_rows + 1))
            segment_chain_fields+=("${rest:-}")
            printf 'segment_chain\t%s\t%s\t%s\t%s\t%s\t%s\n' "${original:-}" "${consumed:-}" "${committed:-}" \
                "${remaining:-}" "${corrected:-}" "${combined:-}"
            ;;
        pending_segment)
            IFS=$'\t' read -r original consumed committed remaining <<< "${rest:-}"
            pending_segment_rows=$((pending_segment_rows + 1))
            pending_segment_fields+=("${rest:-}")
            printf 'pending_segment\t%s\t%s\t%s\t%s\n' "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}"
            ;;
        preference)
            IFS=$'\t' read -r learned_preedit learned_candidate learned_count _ <<< "${rest:-}"
            printf 'preference\t%s\n' "${rest:-}"
            if [[ "${learned_count:-}" =~ ^[1-9][0-9]*$ ]]; then
                preference_rows=$((preference_rows + 1))
                preference_total=$((preference_total + learned_count))
                top_preferences+=("$learned_count"$'\t'"${learned_preedit:-}"$'\t'"${learned_candidate:-}")
            fi
            ;;
        correction)
            IFS=$'\t' read -r learned_typo learned_corrected learned_count _ <<< "${rest:-}"
            printf 'correction\t%s\n' "${rest:-}"
            if [[ "${learned_count:-}" =~ ^[1-9][0-9]*$ ]]; then
                correction_rows=$((correction_rows + 1))
                correction_total=$((correction_total + learned_count))
                top_corrections+=("$learned_count"$'\t'"${learned_typo:-}"$'\t'"${learned_corrected:-}")
                correction_pattern_rows+=("${learned_typo:-}"$'\t'"${learned_corrected:-}"$'\t'"$learned_count")
            fi
            ;;
        *)
            printf 'unknown\t%s\t%s\t%s\n' "$line_number" "$kind" "${rest:-}"
            ;;
    esac
done < "$input"

printf 'learning_preference_count\t%s\n' "$preference_rows"
printf 'learning_preference_total\t%s\n' "$preference_total"
printf 'learning_correction_count\t%s\n' "$correction_rows"
printf 'learning_correction_total\t%s\n' "$correction_total"
print_top_rows "learning_top_preference" "${top_preferences[@]}"
print_top_rows "learning_top_correction" "${top_corrections[@]}"
if [[ -n "$selected_candidate_text" ]]; then
    selected_candidate_status="would-learn-preference"
    if [[ "$selected_candidate_index" == "0" || ( "${#candidates[@]}" -gt 0 && "$selected_candidate_text" == "${candidates[0]}" ) ]]; then
        selected_candidate_status="already-top"
    elif is_prefix_only_candidate "$selected_candidate_text"; then
        selected_candidate_status="prefix-only-no-preference"
    fi
    printf 'learning_selected_candidate_signal\t%s\t%s\t%s\t%s\n' \
        "$preedit_value" "$selected_candidate_index" "$selected_candidate_text" "$selected_candidate_status"
fi
rank=0
for chain in "${segment_chain_fields[@]}"; do
    rank=$((rank + 1))
    IFS=$'\t' read -r original consumed committed remaining corrected combined _ <<< "$chain"
    chain_status="continuation"
    if [[ -n "${corrected:-}" && -n "${original:-}" && "${corrected:-}" != "${original:-}" ]]; then
        chain_status="correction-chain"
    fi
    if [[ "${original:-}" != "$preedit_value" && "${remaining:-}" == "$preedit_value" &&
        "${#context_fields[@]}" -gt 0 && "${context_fields[$((${#context_fields[@]} - 1))]}" == "${committed:-}" ]]; then
        if [[ "$chain_status" == "correction-chain" ]]; then
            chain_status="suffix-correction-chain"
        else
            chain_status="suffix-continuation"
        fi
    fi
    printf 'learning_segment_chain_signal\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$rank" "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}" "${corrected:-}" \
        "${combined:-}" "$chain_status"
    if [[ "$rank" -ge 5 ]]; then
        break
    fi
done

rank=0
for segment in "${pending_segment_fields[@]}"; do
    rank=$((rank + 1))
    IFS=$'\t' read -r original consumed committed remaining _ <<< "$segment"
    if [[ -n "$selected_candidate_text" && "$selected_candidate_index" =~ ^[1-9][0-9]*$ &&
          "${remaining:-}" == "$preedit_value" ]]; then
        corrected="${consumed:-}${preedit_value}"
        combined="${committed:-}${selected_candidate_text}"
        if valid_segment_chain_shape "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}" \
            "$corrected" "$combined"; then
            printf 'learning_pending_segment_signal\t%s\t%s\t%s\t%s\t%s\tconfirmed-suffix\t%s\t%s\t%s\n' \
                "$rank" "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}" \
                "$selected_candidate_text" "$corrected" "$combined"
        else
            printf 'learning_pending_segment_signal\t%s\t%s\t%s\t%s\t%s\tawaiting-suffix-confirmation\n' \
                "$rank" "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}"
        fi
    else
        printf 'learning_pending_segment_signal\t%s\t%s\t%s\t%s\t%s\tawaiting-suffix-confirmation\n' \
            "$rank" "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}"
    fi
    if [[ "$rank" -ge 5 ]]; then
        break
    fi
done

if [[ "$panel_output" == 1 ]]; then
    printf 'panel\tstate\tpreedit\t%s\n' "$preedit_value"
    printf 'panel\tstate\tapplication\t%s\n' "$application_value"
    printf 'panel\tstate\tsurrounding-before\t%s\n' "$surrounding_before_value"
    printf 'panel\tstate\tsurrounding-after\t%s\n' "$surrounding_after_value"
    printf 'panel\tcandidates\ttotal\t%s\tvisible\t%s\tnumbered\t%s\n' "$candidate_count" "$visible_count" "$numbered_count"
    if [[ "${#candidates[@]}" -gt 0 ]]; then
        printf 'panel\tcandidates\tfirst\t0\t%s\n' "${candidates[0]}"
    fi
    if [[ -n "$selected_candidate_index" || -n "$selected_candidate_text" ]]; then
        printf 'panel\tcandidates\tselected\t%s\t%s\n' "$selected_candidate_index" "$selected_candidate_text"
    fi
    rank=0
    for item in "${candidate_metadata[@]}"; do
        rank=$((rank + 1))
        IFS=$'\t' read -r index key_consumed consumed key_source source key_score score <<< "$item"
        printf 'panel\tcandidates\tmetadata\t%s\t%s\t%s\t%s\n' "${index:-}" "${consumed:-0}" "${source:-}" "${score:-0}"
        if [[ "$rank" -ge 9 ]]; then
            break
        fi
    done
    rank=0
    for item in "${visible_candidates[@]}"; do
        rank=$((rank + 1))
        IFS=: read -r index text <<< "$item"
        printf 'panel\tcandidates\tvisible\t%s\t%s\t%s\n' "$rank" "${index:-}" "${text:-}"
        if [[ "$rank" -ge 9 ]]; then
            break
        fi
    done
    rank=0
    for item in "${numbered_candidates[@]}"; do
        rank=$((rank + 1))
        IFS=: read -r shortcut index text <<< "$item"
        printf 'panel\tcandidates\tnumbered\t%s\t%s\t%s\t%s\n' "$rank" "${shortcut:-}" "${index:-}" "${text:-}"
        if [[ "$rank" -ge 9 ]]; then
            break
        fi
    done
    printf 'panel\tsupervision\trecent-events\t%s\tcorrection-events\t%s\tcontext\t%s\tsegment-chains\t%s\tpending-segments\t%s\n' \
        "${#events_fields[@]}" "${#correction_event_fields[@]}" "$context_count" "$segment_chain_rows" "$pending_segment_rows"
    printf 'panel\tsupervision\tmodel-input\tpreedit\t%s\tcandidates\t%s\tvisible\t%s\tnumbered\t%s\tcontext\t%s\tsegment-chains\t%s\tpending-segments\t%s\n' \
        "$preedit_value" "$candidate_count" "$visible_count" "$numbered_count" "$context_count" "$segment_chain_rows" "$pending_segment_rows"
    if [[ "$supervision_mode" == "active-preedit" || "$supervision_mode" == "pass-through-only" ]]; then
        printf 'panel\tsupervision\tmode\t%s\n' "$supervision_mode"
    elif [[ -n "$preedit_value" ]]; then
        printf 'panel\tsupervision\tmode\tactive-preedit\n'
    else
        printf 'panel\tsupervision\tmode\tpass-through-only\n'
    fi
    if [[ -n "$supervision_state" ]]; then
        printf 'panel\tsupervision\tstate\tmode\t%s\trecent-events\t%s\tcorrection-events\t%s\n' \
            "${supervision_mode:-unknown}" "${supervision_recent_events:-0}" "${supervision_correction_events:-0}"
    fi
    printf 'panel\tsupervision\truntime-state\tcontinuous\t%s\n' "$runtime_continuous"
    if [[ -n "$runtime_input_mode" ]]; then
        printf 'panel\tsupervision\tinput-mode\t%s\n' "$runtime_input_mode"
    fi
    printf 'panel\tsupervision\tevent-trail\trecent\t%s\tlimit\t64\tpurpose\tui-and-short-action-order\n' \
        "${#events_fields[@]}"
    printf 'panel\tsupervision\tcorrection-trail\trecent\t%s\tlimit\t256\tpurpose\tdelete-retype-and-middle-edit-learning\n' \
        "${#correction_event_fields[@]}"
    if [[ "${#event_count_fields[@]}" -gt 0 ]]; then
        for item in "${event_count_fields[@]}"; do
            IFS=: read -r event_kind event_count <<< "$item"
            printf 'panel\tsupervision\tevent-count\t%s\t%s\n' "${event_kind:-}" "${event_count:-0}"
        done
    fi
    if [[ "${#correction_event_count_fields[@]}" -gt 0 ]]; then
        for item in "${correction_event_count_fields[@]}"; do
            IFS=: read -r event_kind event_count <<< "$item"
            printf 'panel\tsupervision\tcorrection-event-count\t%s\t%s\n' "${event_kind:-}" "${event_count:-0}"
        done
    fi
    rank=0
    for segment in "${pending_segment_fields[@]}"; do
        rank=$((rank + 1))
        IFS=$'\t' read -r original consumed committed remaining _ <<< "$segment"
        if [[ -n "$selected_candidate_text" && "$selected_candidate_index" =~ ^[1-9][0-9]*$ &&
              "${remaining:-}" == "$preedit_value" ]]; then
            corrected="${consumed:-}${preedit_value}"
            combined="${committed:-}${selected_candidate_text}"
            if valid_segment_chain_shape "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}" \
                "$corrected" "$combined"; then
                printf 'panel\tlearning\tpending-segment-signal\t%s\t%s\t%s\t%s\t%s\tconfirmed-suffix\t%s\t%s\t%s\n' \
                    "$rank" "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}" \
                    "$selected_candidate_text" "$corrected" "$combined"
            else
                printf 'panel\tlearning\tpending-segment-signal\t%s\t%s\t%s\t%s\t%s\tawaiting-suffix-confirmation\n' \
                    "$rank" "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}"
            fi
        else
            printf 'panel\tlearning\tpending-segment-signal\t%s\t%s\t%s\t%s\t%s\tawaiting-suffix-confirmation\n' \
                "$rank" "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}"
        fi
        if [[ "$rank" -ge 5 ]]; then
            break
        fi
    done
    if [[ "${#events_fields[@]}" -gt 0 ]]; then
        start=0
        if [[ "${#events_fields[@]}" -gt 8 ]]; then
            start=$((${#events_fields[@]} - 8))
        fi
        rank=0
        for ((index = start; index < ${#events_fields[@]}; ++index)); do
            rank=$((rank + 1))
            IFS=: read -r event_kind event_text <<< "${events_fields[$index]}"
            printf 'panel\tsupervision\tevent-item\t%s\t%s\t%s\n' "$rank" "${event_kind:-}" "${event_text:-}"
        done
    fi
    if [[ "${#correction_event_fields[@]}" -gt 0 ]]; then
        start=0
        if [[ "${#correction_event_fields[@]}" -gt 12 ]]; then
            start=$((${#correction_event_fields[@]} - 12))
        fi
        rank=0
        for ((index = start; index < ${#correction_event_fields[@]}; ++index)); do
            rank=$((rank + 1))
            IFS=: read -r event_kind event_text <<< "${correction_event_fields[$index]}"
            printf 'panel\tsupervision\tcorrection-event-item\t%s\t%s\t%s\n' "$rank" "${event_kind:-}" "${event_text:-}"
        done
    fi
    rank=0
    for chain in "${segment_chain_fields[@]}"; do
        rank=$((rank + 1))
        printf 'panel\tsegment-chain\t%s\t%s\n' "$rank" "$chain"
        if [[ "$rank" -ge 5 ]]; then
            break
        fi
    done
    printf 'panel\tlearning\tpreferences\t%s\ttotal\t%s\tcorrections\t%s\ttotal\t%s\n' \
        "$preference_rows" "$preference_total" "$correction_rows" "$correction_total"
    learning_status_primary="no-action"
    learning_status_next_step="do-not-emit-output"
    learning_status_suggested_protocols=()
    learning_status_awaiting_suffix=()
    learning_status_signal_counts=()
    selected_candidate_prefix_only=0
    if [[ -z "$preedit_value" ]]; then
        learning_status_primary="keyboard-context-only"
        learning_status_next_step="wait-for-active-preedit"
    else
        if [[ -n "$selected_candidate_text" && "$selected_candidate_index" != "0" &&
              "${#candidates[@]}" -gt 0 && "$selected_candidate_text" != "${candidates[0]}" ]]; then
            if ! is_prefix_only_candidate "$selected_candidate_text"; then
                learning_status_suggested_protocols+=("preference"$'\t'"$preedit_value"$'\t'"$selected_candidate_text"$'\t'"2")
                learning_status_signal_counts+=("selected_candidate:1")
            else
                selected_candidate_prefix_only=1
                learning_status_signal_counts+=("selected_candidate_prefix:1")
            fi
        fi
        for segment in "${pending_segment_fields[@]}"; do
            IFS=$'\t' read -r original consumed committed remaining _ <<< "$segment"
            [[ -n "${original:-}" && -n "${consumed:-}" && -n "${committed:-}" && "${remaining:-}" == "$preedit_value" ]] ||
                continue
            if [[ -n "$selected_candidate_text" && "$selected_candidate_index" =~ ^[1-9][0-9]*$ ]] &&
                candidate_is_present "$selected_candidate_text"; then
                corrected="${consumed:-}${preedit_value}"
                combined="${committed:-}${selected_candidate_text}"
                if valid_segment_chain_shape "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}" \
                    "$corrected" "$combined"; then
                    learning_status_suggested_protocols+=("segment_chain"$'\t'"${original:-}"$'\t'"${consumed:-}"$'\t'"${committed:-}"$'\t'"${remaining:-}"$'\t'"$corrected"$'\t'"$combined"$'\t'"1")
                    learning_status_signal_counts+=("pending_segment:1")
                fi
            else
                learning_status_awaiting_suffix+=("${original:-}"$'\t'"${consumed:-}"$'\t'"${committed:-}"$'\t'"${remaining:-}")
                learning_status_signal_counts+=("pending_segment:1")
            fi
        done
        if [[ "${#learning_status_suggested_protocols[@]}" -gt 0 ]]; then
            learning_status_primary="ready-to-learn"
            learning_status_next_step="prefer-suggested-protocols"
        elif [[ "${#learning_status_awaiting_suffix[@]}" -gt 0 ]]; then
            learning_status_primary="awaiting-suffix-confirmation"
            learning_status_next_step="wait-for-selected-suffix"
        elif [[ "$selected_candidate_prefix_only" == 1 ]]; then
            learning_status_primary="awaiting-suffix-confirmation"
            learning_status_next_step="wait-for-selected-suffix"
        elif [[ -n "$selected_candidate_text" && ( "$selected_candidate_index" == "0" ||
                  ( "${#candidates[@]}" -gt 0 && "$selected_candidate_text" == "${candidates[0]}" ) ) ]]; then
            learning_status_primary="selected-candidate-already-top"
            learning_status_next_step="no-persistent-preference-needed"
        elif [[ "${#candidates[@]}" -gt 0 ]]; then
            learning_status_primary="rank-only"
            learning_status_next_step="rerank-only-with-stronger-evidence"
        fi
    fi
    printf 'panel\tlearning\tstatus\t%s\tnext-step\t%s\n' "$learning_status_primary" "$learning_status_next_step"
    rank=0
    for item in "${learning_status_suggested_protocols[@]}"; do
        rank=$((rank + 1))
        printf 'panel\tlearning\tstatus-suggested-protocol\t%s\t%s\n' "$rank" "$item"
        if [[ "$rank" -ge 8 ]]; then
            break
        fi
    done
    rank=0
    for item in "${learning_status_awaiting_suffix[@]}"; do
        rank=$((rank + 1))
        printf 'panel\tlearning\tstatus-awaiting-suffix\t%s\t%s\n' "$rank" "$item"
        if [[ "$rank" -ge 4 ]]; then
            break
        fi
    done
    for item in "${learning_status_signal_counts[@]}"; do
        IFS=: read -r signal_kind signal_count <<< "$item"
        printf 'panel\tlearning\tstatus-signal-count\t%s\t%s\n' "${signal_kind:-}" "${signal_count:-0}"
    done
    if [[ -n "$selected_candidate_text" ]]; then
        selected_candidate_status="would-learn-preference"
        if [[ "$selected_candidate_index" == "0" || ( "${#candidates[@]}" -gt 0 && "$selected_candidate_text" == "${candidates[0]}" ) ]]; then
            selected_candidate_status="already-top"
        elif is_prefix_only_candidate "$selected_candidate_text"; then
            selected_candidate_status="prefix-only-no-preference"
        fi
        printf 'panel\tlearning\tselected-candidate-signal\t%s\t%s\t%s\t%s\n' \
            "$preedit_value" "$selected_candidate_index" "$selected_candidate_text" "$selected_candidate_status"
    fi
    rank=0
    for chain in "${segment_chain_fields[@]}"; do
        rank=$((rank + 1))
        IFS=$'\t' read -r original consumed committed remaining corrected combined _ <<< "$chain"
        chain_status="continuation"
        if [[ -n "${corrected:-}" && -n "${original:-}" && "${corrected:-}" != "${original:-}" ]]; then
            chain_status="correction-chain"
        fi
        if [[ "${original:-}" != "$preedit_value" && "${remaining:-}" == "$preedit_value" &&
            "${#context_fields[@]}" -gt 0 && "${context_fields[$((${#context_fields[@]} - 1))]}" == "${committed:-}" ]]; then
            if [[ "$chain_status" == "correction-chain" ]]; then
                chain_status="suffix-correction-chain"
            else
                chain_status="suffix-continuation"
            fi
        fi
        printf 'panel\tlearning\tsegment-chain-signal\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$rank" "${original:-}" "${consumed:-}" "${committed:-}" "${remaining:-}" "${corrected:-}" \
            "${combined:-}" "$chain_status"
        if [[ "$rank" -ge 5 ]]; then
            break
        fi
    done
    print_panel_top_rows "top-preference" "${top_preferences[@]}"
    print_panel_top_rows "top-correction" "${top_corrections[@]}"
fi

if command -v python3 >/dev/null 2>&1; then
    TIPE_EXPLAIN_PREEDIT="$preedit_value" \
    TIPE_EXPLAIN_APPLICATION="$application_value" \
    TIPE_EXPLAIN_SURROUNDING_BEFORE="$surrounding_before_value" \
    TIPE_EXPLAIN_SURROUNDING_AFTER="$surrounding_after_value" \
    TIPE_EXPLAIN_EVENTS="$(printf '%s\n' "${events_fields[@]}")" \
    TIPE_EXPLAIN_CORRECTION_EVENTS="$(printf '%s\n' "${correction_event_fields[@]}")" \
    TIPE_EXPLAIN_EVENT_COUNTS="$(printf '%s\n' "${event_count_fields[@]}")" \
    TIPE_EXPLAIN_CORRECTION_EVENT_COUNTS="$(printf '%s\n' "${correction_event_count_fields[@]}")" \
    TIPE_EXPLAIN_PREFERENCES="$(printf '%s\n' "${top_preferences[@]}")" \
    TIPE_EXPLAIN_CORRECTIONS="$(printf '%s\n' "${correction_pattern_rows[@]}")" \
    TIPE_EXPLAIN_PANEL="$panel_output" \
    python3 - <<'PY'
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
        if event_kind(item) in {"letter", "digit", "symbol", "rerank-requested"}:
            break
        leading.append(item)
    return leading


def declared_event_counts(items):
    counts = []
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
        counts.append((kind, parsed_count))
    return counts


def default_history_path():
    configured = os.environ.get("TIPE_SUPERVISION_HISTORY")
    if configured:
        return Path(configured)
    cache_home = os.environ.get("XDG_CACHE_HOME")
    if cache_home:
        return Path(cache_home) / "tipe" / "supervision-history.tsv"
    home = os.environ.get("HOME")
    if home:
        return Path(home) / ".cache" / "tipe" / "supervision-history.tsv"
    return Path("/tmp/tipe/supervision-history.tsv")


def recent_history_event_summary():
    path = default_history_path()
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return {
            "available": False,
            "path": str(path),
            "records": 0,
            "active_records": 0,
            "pass_through_records": 0,
            "event_counts": [],
            "active_event_counts": [],
            "pass_through_event_counts": [],
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
    for _header, body in records:
        fields = {"preedit": "", "supervision_mode": "", "event_counts": []}
        for line in body:
            parts = line.split("\t")
            if not parts:
                continue
            kind = parts[0]
            if kind == "preedit" and len(parts) >= 2:
                fields["preedit"] = parts[1]
            elif kind == "event_counts":
                fields["event_counts"].extend(parts[1:])
            elif kind == "supervision_state":
                for index in range(1, len(parts) - 1, 2):
                    if parts[index] == "mode":
                        fields["supervision_mode"] = parts[index + 1]
        parsed.append(fields)

    def count_pairs(items):
        counts = {}
        for item in items:
            name, separator, value = item.partition(":")
            if not name or not separator:
                continue
            try:
                amount = int(value)
            except ValueError:
                amount = 1
            counts[name] = counts.get(name, 0) + amount
        return sorted(counts.items(), key=lambda item: (-item[1], item[0]))

    def record_mode(item):
        mode = item.get("supervision_mode", "")
        if mode in {"active-preedit", "pass-through-only"}:
            return mode
        return "active-preedit" if item.get("preedit", "") else "pass-through-only"

    active_items = [item for item in parsed if record_mode(item) == "active-preedit"]
    pass_through_items = [item for item in parsed if record_mode(item) == "pass-through-only"]
    return {
        "available": bool(parsed),
        "path": str(path),
        "records": len(parsed),
        "active_records": len(active_items),
        "pass_through_records": len(pass_through_items),
        "event_counts": count_pairs(value for item in parsed for value in item.get("event_counts", [])),
        "active_event_counts": count_pairs(value for item in active_items for value in item.get("event_counts", [])),
        "pass_through_event_counts": count_pairs(
            value for item in pass_through_items for value in item.get("event_counts", [])
        ),
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
        count_text, learned_preedit, learned_candidate = fields[:3]
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
        return (True, "learned-raw-preference", learned_count)
    if preedit and lower_ascii(preedit) in KNOWN_ENGLISH_TOKENS:
        return (True, "known-english-token", 0)
    return (False, "none", learned_count)


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
            result.append((source, typo, corrected_preedit))
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
            "corrected": corrected,
        })
    return decisions


preedit = os.environ.get("TIPE_EXPLAIN_PREEDIT", "")
application = unescape_tipe_field(os.environ.get("TIPE_EXPLAIN_APPLICATION", ""))
surrounding_before = unescape_tipe_field(os.environ.get("TIPE_EXPLAIN_SURROUNDING_BEFORE", ""))
surrounding_after = unescape_tipe_field(os.environ.get("TIPE_EXPLAIN_SURROUNDING_AFTER", ""))
events = [
    unescape_tipe_field(line)
    for line in os.environ.get("TIPE_EXPLAIN_EVENTS", "").splitlines()
    if line
]
correction_events = [
    unescape_tipe_field(line)
    for line in os.environ.get("TIPE_EXPLAIN_CORRECTION_EVENTS", "").splitlines()
    if line
]
declared_recent_counts = declared_event_counts([
    unescape_tipe_field(line)
    for line in os.environ.get("TIPE_EXPLAIN_EVENT_COUNTS", "").splitlines()
    if line
])
declared_correction_counts = declared_event_counts([
    unescape_tipe_field(line)
    for line in os.environ.get("TIPE_EXPLAIN_CORRECTION_EVENT_COUNTS", "").splitlines()
    if line
])
correction_rows_for_patterns = [
    line for line in os.environ.get("TIPE_EXPLAIN_CORRECTIONS", "").splitlines() if line
]
preference_rows_for_raw = [
    line for line in os.environ.get("TIPE_EXPLAIN_PREFERENCES", "").splitlines() if line
]
panel = os.environ.get("TIPE_EXPLAIN_PANEL", "") == "1"

recent_counts = declared_recent_counts or event_counts(events)
correction_counts = declared_correction_counts or event_counts(correction_events)
leading_context = preedit_leading_context(events, bool(preedit))
possible_corrections = possible_corrections_from_events(correction_events, preedit)
edit_summary = edit_summary_from_events(correction_events)
correction_patterns = correction_patterns_from_rows(correction_rows_for_patterns)
realtime_decisions = realtime_correction_decisions(preedit, correction_patterns)
hint, hint_source, hint_count = raw_english_hint(
    preedit, application, surrounding_before, surrounding_after, preference_rows_for_raw
)
history_summary = recent_history_event_summary()

for kind, count in recent_counts:
    print(f"behavior_recent_event_count\t{kind}\t{count}")
for kind, count in correction_counts:
    print(f"behavior_correction_event_count\t{kind}\t{count}")
if history_summary["available"]:
    print(
        "behavior_history_summary\t"
        f"records\t{history_summary['records']}\t"
        f"active\t{history_summary['active_records']}\t"
        f"pass-through\t{history_summary['pass_through_records']}\t"
        f"path\t{history_summary['path']}"
    )
    for kind, count in history_summary["event_counts"][:8]:
        print(f"behavior_history_event_count\t{kind}\t{count}")
    for kind, count in history_summary["active_event_counts"][:8]:
        print(f"behavior_history_active_event_count\t{kind}\t{count}")
    for kind, count in history_summary["pass_through_event_counts"][:8]:
        print(f"behavior_history_pass_through_event_count\t{kind}\t{count}")
print(f"behavior_raw_english_hint\t{1 if hint else 0}\t{hint_source}\tcount\t{hint_count}")
print(f"behavior_preedit_leading_context\tactive\t{1 if leading_context else 0}\tevents\t{len(leading_context)}")
for rank, item in enumerate(leading_context, 1):
    print(f"behavior_preedit_leading_event\t{rank}\t{event_kind(item)}\t{event_text(item)}")
print(
    "behavior_edit_summary\t"
    f"current\t{edit_summary['current']}\t"
    f"cursor\t{edit_summary['cursor']}\t"
    f"typed-tail\t{edit_summary['typed_tail']}\t"
    f"last-erased\t{edit_summary['last_fully_erased']}\t"
    f"last-edited\t{edit_summary['last_edited_original']}\t"
    f"middle-edit\t{edit_summary['middle_edit_original']}"
)
for source, typo, corrected in possible_corrections:
    print(f"behavior_possible_correction\t{source}\t{typo}\t{corrected}")
for rank, pattern in enumerate(correction_patterns, 1):
    position_text = f"end-{pattern['position']}" if pattern.get("relative_to_end") else str(pattern["position"])
    print(
        f"learning_correction_pattern\t{rank}\t{pattern['count']}\t{pattern['kind']}\t"
        f"{pattern['text']}\t{position_text}"
    )
    print(
        f"behavior_correction_pattern\t{rank}\t{pattern['kind']}\t{pattern['text']}\t"
        f"{position_text}\t{pattern['count']}"
    )
for rank, decision in enumerate(realtime_decisions, 1):
    position_text = f"end-{decision['position']}" if decision.get("relative_to_end") else str(decision["position"])
    print(
        f"behavior_realtime_correction\t{rank}\t{decision['status']}\t{decision['reason']}\t"
        f"{decision['kind']}\t{decision['text']}\t{position_text}\t{decision['count']}\t"
        f"{decision['corrected']}"
    )

if panel:
    print(f"panel\tbehavior\traw-english-hint\t{1 if hint else 0}\tsource\t{hint_source}\tcount\t{hint_count}")
    print(f"panel\tbehavior\tpreedit-leading-context\tactive\t{1 if leading_context else 0}\tevents\t{len(leading_context)}")
    for rank, item in enumerate(leading_context[-8:], 1):
        print(f"panel\tbehavior\tpreedit-leading-event\t{rank}\t{event_kind(item)}\t{event_text(item)}")
    print(
        "panel\tbehavior\tedit-summary\t"
        f"current\t{edit_summary['current']}\t"
        f"cursor\t{edit_summary['cursor']}\t"
        f"typed-tail\t{edit_summary['typed_tail']}\t"
        f"last-erased\t{edit_summary['last_fully_erased']}\t"
        f"last-edited\t{edit_summary['last_edited_original']}\t"
        f"middle-edit\t{edit_summary['middle_edit_original']}"
    )
    for kind, count in recent_counts:
        print(f"panel\tbehavior\trecent-event\t{kind}\t{count}")
    for kind, count in correction_counts:
        print(f"panel\tbehavior\tcorrection-event\t{kind}\t{count}")
    for rank, (source, typo, corrected) in enumerate(possible_corrections, 1):
        print(f"panel\tlearning\tcorrection-signal\t{rank}\t{source}\t{typo}\t{corrected}")
        print(f"panel\tbehavior\tpossible-correction\t{rank}\t{source}\t{typo}\t{corrected}")
    for rank, pattern in enumerate(correction_patterns, 1):
        position_text = f"end-{pattern['position']}" if pattern.get("relative_to_end") else str(pattern["position"])
        print(
            f"panel\ttop-correction-pattern\t{rank}\t{pattern['count']}\t{pattern['kind']}\t"
            f"{pattern['text']}\t{position_text}"
        )
        print(
            f"panel\tbehavior\tcorrection-pattern\t{rank}\t{pattern['kind']}\t{pattern['text']}\t"
            f"{position_text}\t{pattern['count']}"
        )
    for rank, decision in enumerate(realtime_decisions, 1):
        position_text = f"end-{decision['position']}" if decision.get("relative_to_end") else str(decision["position"])
        print(
            f"panel\tbehavior\trealtime-correction\t{rank}\t{decision['status']}\t{decision['reason']}\t"
            f"{decision['kind']}\t{decision['text']}\t{position_text}\t{decision['count']}\t"
            f"{decision['corrected']}"
        )
PY
fi
