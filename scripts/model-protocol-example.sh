#!/usr/bin/env bash
set -euo pipefail

preedit=""
candidates=()
segment_chains=()

while IFS=$'\t' read -r kind rest || [[ -n "${kind:-}" || -n "${rest:-}" ]]; do
    case "$kind" in
        preedit)
            preedit="$rest"
            ;;
        candidates)
            IFS=$'\t' read -r -a candidates <<< "$rest"
            ;;
        segment_chain)
            segment_chains+=("$rest")
            ;;
    esac
done

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

for chain in "${segment_chains[@]}"; do
    IFS=$'\t' read -r original consumed committed remaining corrected combined extra <<< "$chain"
    [[ "${original:-}" == "$preedit" && -n "${combined:-}" ]] || continue
    valid_segment_chain_shape "$original" "$consumed" "$committed" "$remaining" "$corrected" "$combined" || continue
    emit_candidate_if_present "$combined" || true
    if [[ -n "${corrected:-}" && "$corrected" != "$original" ]]; then
        printf 'correction\t%s\t%s\n' "$original" "$corrected"
    fi
done

# Example candidate reranker. Emit only candidates that were present in the input.
for candidate in "${candidates[@]}"; do
    if [[ "$candidate" == "你号" ]]; then
        printf 'candidate\t%s\n' "$candidate"
    fi
done
