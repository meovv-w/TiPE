#!/usr/bin/env bash
set -euo pipefail
umask 077

MAX_PINYIN_BYTES=128
MAX_CANDIDATE_BYTES=256
MAX_CANDIDATES_PER_ROW=32
MAX_ENTRY_ROWS=4096

byte_length() {
    LC_ALL=C printf '%s' "$1" | wc -c
}

usage() {
    cat <<EOF
Usage: $0 [--explain] [--path PATH] [PATH]
       $0 --add PINYIN CANDIDATE... [--first] [--path PATH]

Validate a TiPE user dictionary TSV file.
Use --explain to print each valid row's candidate order.
Use --add to create or update one row without duplicating candidates.
Use --first with --add to move the given candidates to the front in the given order.
EOF
}

default_dictionary_path() {
    if [[ -n "${TIPE_USER_DICTIONARY:-}" ]]; then
        printf '%s\n' "$TIPE_USER_DICTIONARY"
    elif [[ -n "${XDG_DATA_HOME:-}" ]]; then
        printf '%s\n' "$XDG_DATA_HOME/tipe/user-dictionary.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.local/share/tipe/user-dictionary.tsv"
    else
        return 1
    fi
}

validate_pinyin() {
    local pinyin="$1"
    if [[ -z "$pinyin" ]]; then
        echo "pinyin field is empty" >&2
        return 1
    fi
    if [[ ! "$pinyin" =~ ^[a-z]+$ ]]; then
        echo "pinyin should use lowercase a-z letters only: $pinyin" >&2
        return 1
    fi
    if (( $(byte_length "$pinyin") > MAX_PINYIN_BYTES )); then
        echo "pinyin exceeds $MAX_PINYIN_BYTES bytes" >&2
        return 1
    fi
}

pinyin_error() {
    local pinyin="$1"
    if [[ -z "$pinyin" ]]; then
        echo "pinyin field is empty"
        return 0
    fi
    if [[ ! "$pinyin" =~ ^[a-z]+$ ]]; then
        echo "pinyin should use lowercase a-z letters only: $pinyin"
        return 0
    fi
    if (( $(byte_length "$pinyin") > MAX_PINYIN_BYTES )); then
        echo "pinyin exceeds $MAX_PINYIN_BYTES bytes"
        return 0
    fi
}

validate_candidate() {
    local candidate="$1"
    if [[ -z "$candidate" || "$candidate" == *$'\t'* || "$candidate" == *$'\n'* || "$candidate" == *$'\r'* ]]; then
        echo "candidate should be non-empty and must not contain TAB or newlines" >&2
        return 1
    fi
    if (( $(byte_length "$candidate") > MAX_CANDIDATE_BYTES )); then
        echo "candidate exceeds $MAX_CANDIDATE_BYTES bytes" >&2
        return 1
    fi
}

validate_dictionary() {
    local dictionary_path="$1"
    local explain="$2"

    if [[ ! -e "$dictionary_path" ]]; then
        echo "TiPE user dictionary not found: $dictionary_path"
        return 0
    fi

    local line_number=0 entry_rows=0
    local line pinyin rest seen_candidates candidate_index candidate
    local -a candidates explain_fields
    while IFS= read -r line || [[ -n "$line" ]]; do
        line_number=$((line_number + 1))
        line="${line%$'\r'}"
        [[ -z "$line" || "${line:0:1}" == "#" ]] && continue

        if [[ "$line" != *$'\t'* ]]; then
            echo "$dictionary_path:$line_number: expected TAB-separated pinyin and at least one candidate" >&2
            return 1
        fi

        pinyin="${line%%$'\t'*}"
        rest="${line#*$'\t'}"
        entry_rows=$((entry_rows + 1))
        if (( entry_rows > MAX_ENTRY_ROWS )); then
            echo "$dictionary_path:$line_number: dictionary exceeds $MAX_ENTRY_ROWS entry rows" >&2
            return 1
        fi
        if pinyin_message=$(pinyin_error "$pinyin"); [[ -n "$pinyin_message" ]]; then
            echo "$dictionary_path:$line_number: $pinyin_message" >&2
            return 1
        fi
        if [[ -z "$rest" || "$rest" =~ ^($'\t')*$ ]]; then
            echo "$dictionary_path:$line_number: at least one candidate is required" >&2
            return 1
        fi
        if [[ "$rest" == $'\t'* || "$rest" == *$'\t\t'* || "$rest" == *$'\t' ]]; then
            echo "$dictionary_path:$line_number: candidate field is empty" >&2
            return 1
        fi

        seen_candidates=$'\t'
        IFS=$'\t' read -r -a candidates <<< "$rest"
        if (( ${#candidates[@]} > MAX_CANDIDATES_PER_ROW )); then
            echo "$dictionary_path:$line_number: row exceeds $MAX_CANDIDATES_PER_ROW candidates" >&2
            return 1
        fi
        candidate_index=0
        explain_fields=()
        for candidate in "${candidates[@]}"; do
            candidate_index=$((candidate_index + 1))
            if ! validate_candidate "$candidate" >/dev/null; then
                echo "$dictionary_path:$line_number: invalid candidate at field $candidate_index" >&2
                return 1
            fi
            if [[ "$seen_candidates" == *$'\t'"$candidate"$'\t'* ]]; then
                echo "$dictionary_path:$line_number: duplicate candidate: $candidate" >&2
                return 1
            fi
            seen_candidates+="$candidate"$'\t'
            explain_fields+=("$candidate_index:$candidate")
        done
        if [[ "$explain" == 1 ]]; then
            printf 'entry\t%s\t%s' "$line_number" "$pinyin"
            for field in "${explain_fields[@]}"; do
                printf '\t%s' "$field"
            done
            printf '\n'
        fi
    done <"$dictionary_path"

    echo "TiPE user dictionary ok: $dictionary_path"
}

add_dictionary_entry() {
    local dictionary_path="$1"
    local add_pinyin="$2"
    local add_first="$3"
    shift 3
    local -a add_candidates=("$@")

    validate_pinyin "$add_pinyin" >/dev/null
    local seen=$'\t'
    local candidate
    for candidate in "${add_candidates[@]}"; do
        validate_candidate "$candidate" >/dev/null
        if [[ "$seen" == *$'\t'"$candidate"$'\t'* ]]; then
            echo "duplicate candidate in --add arguments: $candidate" >&2
            return 1
        fi
        seen+="$candidate"$'\t'
    done

    mkdir -p "$(dirname -- "$dictionary_path")"
    (
        local lock_path="${dictionary_path}.lock"
        local lock_fd tmp_path updated=0
        local line pinyin rest existing_seen
        local -a existing_candidates merged_candidates

        exec {lock_fd}>"$lock_path"
        chmod 600 "$lock_path"
        flock -x "$lock_fd"
        if [[ -e "$dictionary_path" ]]; then
            validate_dictionary "$dictionary_path" 0 >/dev/null
        fi

        tmp_path=$(mktemp "${dictionary_path}.tmp.XXXXXX")
        trap '[[ -z "${tmp_path:-}" ]] || rm -f -- "$tmp_path"' EXIT
        chmod 600 "$tmp_path"
        if [[ -e "$dictionary_path" ]]; then
            while IFS= read -r line || [[ -n "$line" ]]; do
                line="${line%$'\r'}"
                if [[ -z "$line" || "${line:0:1}" == "#" || "$line" != *$'\t'* ]]; then
                    printf '%s\n' "$line" >>"$tmp_path"
                    continue
                fi
                pinyin="${line%%$'\t'*}"
                rest="${line#*$'\t'}"
                if [[ "$pinyin" != "$add_pinyin" ]]; then
                    printf '%s\n' "$line" >>"$tmp_path"
                    continue
                fi

                IFS=$'\t' read -r -a existing_candidates <<< "$rest"
                merged_candidates=()
                existing_seen=$'\t'
                if [[ "$add_first" == 1 ]]; then
                    for candidate in "${add_candidates[@]}"; do
                        merged_candidates+=("$candidate")
                        existing_seen+="$candidate"$'\t'
                    done
                    for candidate in "${existing_candidates[@]}"; do
                        if [[ "$existing_seen" != *$'\t'"$candidate"$'\t'* ]]; then
                            merged_candidates+=("$candidate")
                            existing_seen+="$candidate"$'\t'
                        fi
                    done
                else
                    merged_candidates=("${existing_candidates[@]}")
                    for candidate in "${existing_candidates[@]}"; do
                        existing_seen+="$candidate"$'\t'
                    done
                    for candidate in "${add_candidates[@]}"; do
                        if [[ "$existing_seen" != *$'\t'"$candidate"$'\t'* ]]; then
                            merged_candidates+=("$candidate")
                            existing_seen+="$candidate"$'\t'
                        fi
                    done
                fi
                if (( ${#merged_candidates[@]} > MAX_CANDIDATES_PER_ROW )); then
                    echo "row exceeds $MAX_CANDIDATES_PER_ROW candidates after --add" >&2
                    exit 1
                fi
                printf '%s' "$pinyin" >>"$tmp_path"
                for candidate in "${merged_candidates[@]}"; do
                    printf '\t%s' "$candidate" >>"$tmp_path"
                done
                printf '\n' >>"$tmp_path"
                updated=1
            done <"$dictionary_path"
        fi

        if [[ $updated -eq 0 ]]; then
            printf '%s' "$add_pinyin" >>"$tmp_path"
            for candidate in "${add_candidates[@]}"; do
                printf '\t%s' "$candidate" >>"$tmp_path"
            done
            printf '\n' >>"$tmp_path"
        fi

        validate_dictionary "$tmp_path" 0 >/dev/null
        sync -d "$tmp_path"
        mv -f -- "$tmp_path" "$dictionary_path"
        tmp_path=""
        chmod 600 "$dictionary_path"
        sync -d "$dictionary_path"
        sync -f "$(dirname -- "$dictionary_path")"
    )
    echo "TiPE user dictionary updated: $dictionary_path"
}

explain=0
dictionary_path=""
add_mode=0
add_first=0
add_pinyin=""
add_candidates=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --explain)
            explain=1
            shift
            ;;
        --path)
            if [[ $# -lt 2 ]]; then
                echo "--path requires PATH" >&2
                exit 2
            fi
            dictionary_path="$2"
            shift 2
            ;;
        --add)
            if [[ $# -lt 3 ]]; then
                echo "--add requires PINYIN and at least one CANDIDATE" >&2
                exit 2
            fi
            add_mode=1
            add_pinyin="$2"
            shift 2
            while [[ $# -gt 0 && "$1" != --* ]]; do
                add_candidates+=("$1")
                shift
            done
            ;;
        --first)
            add_first=1
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
            if [[ -n "$dictionary_path" ]]; then
                echo "usage: $0 [--explain] [--path PATH] [PATH]" >&2
                exit 2
            fi
            dictionary_path="$1"
            shift
            ;;
    esac
done

if [[ -z "$dictionary_path" ]]; then
    if ! dictionary_path=$(default_dictionary_path); then
        echo "HOME is not set and no dictionary path was provided" >&2
        exit 1
    fi
fi

if [[ $add_mode -eq 1 ]]; then
    if [[ ${#add_candidates[@]} -eq 0 ]]; then
        echo "--add requires PINYIN and at least one CANDIDATE" >&2
        exit 2
    fi
    add_dictionary_entry "$dictionary_path" "$add_pinyin" "$add_first" "${add_candidates[@]}"
else
    if [[ $add_first -eq 1 ]]; then
        echo "--first requires --add" >&2
        exit 2
    fi
    validate_dictionary "$dictionary_path" "$explain"
fi
