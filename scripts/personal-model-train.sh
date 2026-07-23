#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${HOME:-}" ]]; then
    echo "HOME is not set; TiP model paths are unavailable" >&2
    exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
history_path=""
output_path=""
min_samples=4
epochs=8
dimension=262144
promotion_margin="0.5"
validation_percent=20
include_application=0
include_context=0
include_surrounding=0
include_evidence=1
deduplicate=0
dry_run=0
force=0
pinyin_dictionaries=()
no_pinyin_prior=0
preferences_path=""
runtime_distill=1
train_nice="${TIPE_PERSONAL_TRAIN_NICE:-10}"

usage() {
    cat <<'EOF'
Usage: tipe-personal-model-train [options]

Exports terminal TiPE supervision, trains TiP once, and
leaves model activation as an explicit separate choice.

Options:
  --history PATH          supervision history override
  --output PATH           TiP model output path
  --min-samples N         minimum labeled candidate choices, default 4
  --epochs N              training epochs, default 8
  --dimension N           hashed feature dimension, default 262144
  --promotion-margin N    minimum score lead before changing candidate order, default 0.5
  --validation-percent N  newest holdout percentage, 0..50, default 20
  --pinyin-dictionary P   Rime pinyin dictionary for the correction prior; repeatable
  --no-pinyin-prior       train without the compact pinyin correction prior
  --preferences PATH      lightweight runtime preference file to update
  --no-runtime-distill    keep TiP keyboard habits and English tokens out of the lightweight runtime
  --include-application   include focused application features
  --include-context       include hashed recent-commit context features
  --include-surrounding   include hashed before/after-cursor context features
  --exclude-evidence      ignore validated correction evidence already present in snapshots
  --deduplicate           collapse repeated equivalent choices
  --dry-run               print dataset statistics without training
  --force                 replace an existing model even when validation is not ready
EOF
}

require_value() {
    if [[ $# -lt 2 || -z "${2:-}" ]]; then
        echo "$1 requires a value" >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --history)
            require_value "$1" "${2:-}"
            history_path="$2"
            shift
            ;;
        --output)
            require_value "$1" "${2:-}"
            output_path="$2"
            shift
            ;;
        --min-samples)
            require_value "$1" "${2:-}"
            min_samples="$2"
            shift
            ;;
        --epochs)
            require_value "$1" "${2:-}"
            epochs="$2"
            shift
            ;;
        --dimension)
            require_value "$1" "${2:-}"
            dimension="$2"
            shift
            ;;
        --promotion-margin)
            require_value "$1" "${2:-}"
            promotion_margin="$2"
            shift
            ;;
        --validation-percent)
            require_value "$1" "${2:-}"
            validation_percent="$2"
            shift
            ;;
        --pinyin-dictionary)
            require_value "$1" "${2:-}"
            pinyin_dictionaries+=("$2")
            shift
            ;;
        --no-pinyin-prior)
            no_pinyin_prior=1
            ;;
        --preferences)
            require_value "$1" "${2:-}"
            preferences_path="$2"
            shift
            ;;
        --no-runtime-distill)
            runtime_distill=0
            ;;
        --include-application)
            include_application=1
            ;;
        --include-context)
            include_context=1
            ;;
        --include-surrounding)
            include_surrounding=1
            ;;
        --exclude-evidence)
            include_evidence=0
            ;;
        --deduplicate)
            deduplicate=1
            ;;
        --dry-run)
            dry_run=1
            ;;
        --force)
            force=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

for value_name in min_samples epochs dimension; do
    value="${!value_name}"
    if [[ ! "$value" =~ ^[0-9]+$ || "$value" == "0" ]]; then
        echo "--${value_name//_/-} must be a positive integer" >&2
        exit 2
    fi
done
if [[ ! "$validation_percent" =~ ^[0-9]+$ ]]; then
    echo "--validation-percent must be a non-negative integer" >&2
    exit 2
fi
if (( epochs > 100 )); then
    echo "--epochs must be 100 or less" >&2
    exit 2
fi
if (( dimension < 1024 || dimension > 1048576 )); then
    echo "--dimension must be between 1024 and 1048576" >&2
    exit 2
fi
if (( validation_percent > 50 )); then
    echo "--validation-percent must be 50 or less" >&2
    exit 2
fi
if [[ ! "$promotion_margin" =~ ^([0-9]+([.][0-9]*)?|[.][0-9]+)$ ]]; then
    echo "--promotion-margin must be a non-negative number" >&2
    exit 2
fi
if [[ ! "$train_nice" =~ ^[0-9]+$ ]] || (( train_nice > 19 )); then
    echo "TIPE_PERSONAL_TRAIN_NICE must be an integer from 0 to 19" >&2
    exit 2
fi
for pinyin_dictionary in "${pinyin_dictionaries[@]}"; do
    if [[ ! -r "$pinyin_dictionary" ]]; then
        echo "--pinyin-dictionary is not readable: $pinyin_dictionary" >&2
        exit 2
    fi
done

helper_path() {
    local installed_name="$1"
    local source_name="$2"
    if [[ -x "$script_dir/$source_name" ]]; then
        printf '%s\n' "$script_dir/$source_name"
    elif [[ -x "$script_dir/$installed_name" ]]; then
        printf '%s\n' "$script_dir/$installed_name"
    elif [[ -x "$HOME/.local/bin/$installed_name" ]]; then
        printf '%s\n' "$HOME/.local/bin/$installed_name"
    else
        return 1
    fi
}

exporter=$(helper_path tipe-training-export training-export.py) || {
    echo "tipe-training-export is not available" >&2
    exit 1
}
personal_model=$(helper_path tipe-personal-model personal-model.py) || {
    echo "tipe-personal-model is not available" >&2
    exit 1
}

export_args=()
[[ -z "$history_path" ]] || export_args+=(--history "$history_path")
[[ "$include_application" == "0" ]] || export_args+=(--include-application)
[[ "$include_context" == "0" ]] || export_args+=(--include-context)
[[ "$include_surrounding" == "0" ]] || export_args+=(--include-surrounding)
[[ "$include_evidence" == "0" ]] || export_args+=(--include-evidence)
[[ "$deduplicate" == "0" ]] || export_args+=(--deduplicate)

if [[ "$dry_run" == "1" ]]; then
    exec "$exporter" "${export_args[@]}" --include-correction-trail --stats
fi

final_output="${output_path:-${XDG_DATA_HOME:-$HOME/.local/share}/tipe/personal-reranker.json}"
output_dir=$(dirname -- "$final_output")
mkdir -p "$output_dir"
if ! command -v flock >/dev/null 2>&1; then
    echo "flock is required to serialize TiP training" >&2
    exit 1
fi
model_lock_path="${final_output}.lock"
umask 077
exec {model_lock_fd}>>"$model_lock_path"
chmod 0600 "$model_lock_path"
if ! flock -n "$model_lock_fd"; then
    echo "another TiP training process is already running: $model_lock_path" >&2
    exit 1
fi
dataset=$(mktemp)
candidate_model=$(mktemp "$output_dir/.personal-reranker.candidate.XXXXXX")
cleanup() {
    rm -f "$dataset"
    [[ -z "${candidate_model:-}" ]] || rm -f "$candidate_model"
}
trap cleanup EXIT
"$exporter" "${export_args[@]}" --include-correction-trail >"$dataset"

train_args=(train --input "$dataset" --min-samples "$min_samples" --epochs "$epochs" --dimension "$dimension"
    --promotion-margin "$promotion_margin" --validation-percent "$validation_percent" --output "$candidate_model")
for pinyin_dictionary in "${pinyin_dictionaries[@]}"; do
    train_args+=(--pinyin-dictionary "$pinyin_dictionary")
done
[[ "$no_pinyin_prior" == "0" ]] || train_args+=(--no-pinyin-prior)
nice_command=()
if command -v nice >/dev/null 2>&1; then
    nice_command=(nice -n "$train_nice")
fi
training_output=$("${nice_command[@]}" "$personal_model" "${train_args[@]}")
recommendation=$(printf '%s\n' "$training_output" | sed -n 's/^recommendation\t//p' | sed -n '1p')
component_update_safe=$(printf '%s\n' "$training_output" | sed -n 's/^component-update-safe\t//p' | sed -n '1p')
candidate_generic_ranking_safe=$(printf '%s\n' "$training_output" |
    sed -n 's/^generic-ranking-safe\t//p' | sed -n '1p')
candidate_raw_profile_safe=$(printf '%s\n' "$training_output" |
    sed -n 's/^raw-profile-safe\t//p' | sed -n '1p')
candidate_keyboard_correction_safe=$(printf '%s\n' "$training_output" |
    sed -n 's/^keyboard-correction-safe\t//p' | sed -n '1p')
candidate_validation_strategy=$(printf '%s\n' "$training_output" |
    sed -n 's/^validation-strategy\t//p' | sed -n '1p')
existing_generic_ranking_safe=0
existing_raw_profile_safe=0
existing_keyboard_correction_safe=0
existing_validation_strategy=""
existing_recommendation=""
existing_model_valid=0
if [[ -e "$final_output" ]]; then
    if existing_inspection=$("$personal_model" inspect --model "$final_output" 2>/dev/null); then
        existing_model_valid=1
    else
        existing_inspection=""
    fi
    existing_generic_ranking_safe=$(printf '%s\n' "$existing_inspection" |
        sed -n 's/^generic-ranking-safe\t//p' | sed -n '1p')
    [[ "$existing_generic_ranking_safe" == "1" ]] || existing_generic_ranking_safe=0
    existing_raw_profile_safe=$(printf '%s\n' "$existing_inspection" |
        sed -n 's/^raw-profile-safe\t//p' | sed -n '1p')
    [[ "$existing_raw_profile_safe" == "1" ]] || existing_raw_profile_safe=0
    existing_keyboard_correction_safe=$(printf '%s\n' "$existing_inspection" |
        sed -n 's/^keyboard-correction-safe\t//p' | sed -n '1p')
    [[ "$existing_keyboard_correction_safe" == "1" ]] || existing_keyboard_correction_safe=0
    existing_validation_strategy=$(printf '%s\n' "$existing_inspection" |
        sed -n 's/^training-validation-strategy\t//p' | sed -n '1p')
    existing_recommendation=$(printf '%s\n' "$existing_inspection" |
        sed -n 's/^training-recommendation\t//p' | sed -n '1p')
fi
model_updated=1
model_update_kind="full-model"
merge_output=""
safe_component_upgrade=0
safe_validation_upgrade=0
safe_capability_upgrade=0
if [[ "$component_update_safe" == "1" && "$existing_model_valid" == "1" &&
    "$existing_recommendation" != "ready" ]]; then
    safe_component_upgrade=1
fi
if [[ "$component_update_safe" == "1" &&
    "$candidate_validation_strategy" == "capability-isolated-temporal-v4" &&
    "$existing_validation_strategy" != "capability-isolated-temporal-v4" &&
    "$existing_generic_ranking_safe" == "0" ]]; then
    safe_validation_upgrade=1
fi
if [[ "$candidate_raw_profile_safe" == "1" && "$existing_raw_profile_safe" == "0" ]] ||
    [[ "$candidate_keyboard_correction_safe" == "1" && "$existing_keyboard_correction_safe" == "0" ]]; then
    safe_capability_upgrade=1
fi
capability_regression=0
if [[ "$existing_generic_ranking_safe" == "1" && "$candidate_generic_ranking_safe" != "1" ]] ||
    [[ "$existing_raw_profile_safe" == "1" && "$candidate_raw_profile_safe" != "1" ]] ||
    [[ "$existing_keyboard_correction_safe" == "1" && "$candidate_keyboard_correction_safe" != "1" ]]; then
    capability_regression=1
fi
if [[ -e "$final_output" && "$force" == "0" && "$capability_regression" == "1" ]]; then
    model_updated=0
    model_update_kind="preserved-safe-capability"
    rm -f "$candidate_model"
    candidate_model=""
elif [[ -e "$final_output" && "$force" == "0" && "$recommendation" != "ready" &&
    "$safe_component_upgrade" != "1" && "$safe_validation_upgrade" != "1" &&
    "$safe_capability_upgrade" != "1" ]]; then
    model_updated=0
    model_update_kind="preserved"
    rm -f "$candidate_model"
    candidate_model=""
else
    if [[ -e "$final_output" && "$recommendation" != "ready" && "$force" == "0" ]]; then
        if [[ "$safe_validation_upgrade" == "1" && "$safe_component_upgrade" == "1" ]]; then
            model_update_kind="safe-component-validation-upgrade"
        elif [[ "$safe_validation_upgrade" == "1" ]]; then
            model_update_kind="safe-validation-upgrade"
        elif [[ "$safe_component_upgrade" == "1" ]]; then
            model_update_kind="safe-component-upgrade"
        else
            model_update_kind="safe-capability-upgrade"
        fi
    elif [[ ! -e "$final_output" ]]; then
        model_update_kind="initial-model"
    elif [[ "$force" == "1" && "$recommendation" != "ready" ]]; then
        model_update_kind="forced"
    fi
    if [[ "$existing_model_valid" == "1" && "$force" == "0" ]]; then
        merge_output=$("$personal_model" merge-safe --existing "$final_output" \
            --candidate "$candidate_model" --output "$candidate_model")
    fi
    mv -f "$candidate_model" "$final_output"
    candidate_model=""
fi
runtime_distill_output=""
runtime_distill_status="disabled"
runtime_distill_error=""
if [[ "$runtime_distill" == "1" ]]; then
    distill_args=(distill-runtime --input "$dataset" --model "$final_output")
    [[ -z "$preferences_path" ]] || distill_args+=(--preferences "$preferences_path")
    runtime_distill_status="failed"
    for attempt in 1 2 3; do
        set +e
        runtime_distill_attempt_output=$("$personal_model" "${distill_args[@]}" 2>&1)
        runtime_distill_attempt_status=$?
        set -e
        if [[ "$runtime_distill_attempt_status" == "0" ]]; then
            runtime_distill_output="$runtime_distill_attempt_output"
            runtime_distill_status="ok"
            runtime_distill_error=""
            break
        fi
        runtime_distill_error=${runtime_distill_attempt_output%%$'\n'*}
        runtime_distill_error=${runtime_distill_error//$'\t'/ }
        runtime_distill_error=${runtime_distill_error//$'\r'/}
        runtime_distill_error=${runtime_distill_error:0:300}
        [[ "$attempt" == "3" ]] || sleep 0.1
    done
fi
printf 'model\t%s\n' "$final_output"
printf '%s\n' "$training_output" | sed '/^model\t/d'
printf 'model-updated\t%s\n' "$model_updated"
printf 'model-update-kind\t%s\n' "$model_update_kind"
if [[ -n "$merge_output" ]]; then
    printf '%s\n' "$merge_output"
fi
if [[ "$runtime_distill_status" == "ok" ]]; then
    printf '%s\n' "$runtime_distill_output"
elif [[ "$runtime_distill_status" == "failed" ]]; then
    printf 'runtime-distill\tfailed\n'
    printf 'runtime-distill-error\t%s\n' "${runtime_distill_error:-unknown error}"
    printf 'warning\tTiP model update succeeded; runtime preference synchronization will retry next time.\n'
else
    printf 'runtime-distill\tdisabled\n'
fi
if [[ "$model_updated" == "0" ]]; then
    if [[ "$model_update_kind" == "preserved-safe-capability" ]]; then
        printf 'model-preserved\tan existing capability passed its safety gate; the candidate model regressed it\n'
    else
        printf 'model-preserved\tvalidation recommendation was %s\n' "${recommendation:-unknown}"
    fi
fi
printf 'activation-hint\ttipe-model-config --write personal --personal-model %q\n' "$final_output"
if [[ "$model_updated" == "1" ]]; then
    printf 'note\tThe model configuration was not changed; an active personal backend reads the updated model on its next request.\n'
else
    printf 'note\tThe model configuration and existing model were not changed.\n'
fi
