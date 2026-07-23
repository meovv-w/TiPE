#!/usr/bin/env bash
set -euo pipefail

command_line=""
config_path=""
use_current=0
keep_request=0
adapter_dry_run=0

usage() {
    cat <<'EOF'
Usage:
  tipe-model-self-test [--command COMMAND] [--current] [--config PATH] [--keep-request] [--adapter-dry-run]

Runs a sample TiPE model request through a model helper and validates the output.
By default this uses the offline heuristic adapter, so it does not call network
endpoints, restart fcitx5, switch input methods, or edit model configuration.

Options:
  --command CMD    wrapper command to test
  --current        test tipe-model-current and the selected model config
  --config PATH    TIPE_MODEL_CONFIG path when using --current or a command
  --keep-request   keep the temporary sample request and print its path
  --adapter-dry-run
                   set TIPE_MODEL_DRY_RUN=1 and validate request-json output
EOF
}

require_value() {
    local option="$1"
    if [[ $# -lt 2 || -z "${2:-}" ]]; then
        echo "$option requires a value" >&2
        exit 2
    fi
}

config_export_value() {
    local name="$1"
    local file="$2"
    [[ -r "$file" ]] || return 1
    sed -n "s/^export $name=['\"]\\{0,1\\}\\([^'\"]*\\)['\"]\\{0,1\\}$/\\1/p" "$file" | sed -n '1p'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --command)
            require_value "$1" "${2:-}"
            command_line="$2"
            shift
            ;;
        --current)
            use_current=1
            ;;
        --config)
            require_value "$1" "${2:-}"
            config_path="$2"
            shift
            ;;
        --keep-request)
            keep_request=1
            ;;
        --adapter-dry-run)
            adapter_dry_run=1
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

if [[ "$use_current" == "1" && -n "$command_line" ]]; then
    echo "--current and --command cannot be used together" >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

helper_path() {
    local helper_name="$1"
    local source_name="$2"
    if [[ -x "$script_dir/$source_name" ]]; then
        printf '%s\n' "$script_dir/$source_name"
    elif [[ -x "${HOME:-}/.local/bin/$helper_name" ]]; then
        printf '%s\n' "$HOME/.local/bin/$helper_name"
    else
        return 1
    fi
}

adapter=$(helper_path tipe-model-adapter model-adapter.sh) || {
    echo "tipe-model-adapter helper is not available" >&2
    exit 1
}
checker=$(helper_path tipe-model-wrapper-check model-wrapper-check.sh) || {
    echo "tipe-model-wrapper-check helper is not available" >&2
    exit 1
}

if [[ "$use_current" == "1" ]]; then
    current=$(helper_path tipe-model-current model-current.sh) || {
        echo "tipe-model-current helper is not available" >&2
        exit 1
    }
    command_line="$current"
elif [[ -z "$command_line" ]]; then
    if [[ "$adapter_dry_run" == "1" ]]; then
        command_line="/usr/bin/env TIPE_MODEL_BACKEND=openai-compatible $adapter"
    else
        command_line="/usr/bin/env TIPE_MODEL_BACKEND=heuristic $adapter"
    fi
fi

if [[ -n "$config_path" && ! -r "$config_path" ]]; then
    echo "cannot read config: $config_path" >&2
    exit 1
fi

if [[ "$adapter_dry_run" == "1" && "$use_current" == "1" ]]; then
    effective_config_path="$config_path"
    if [[ -z "$effective_config_path" ]]; then
        if [[ -z "${HOME:-}" ]]; then
            echo "HOME is not set; cannot locate the current TiPE model config" >&2
            exit 1
        fi
        effective_config_path="${TIPE_MODEL_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/tipe/model-env}"
    fi
    configured_mode=$(config_export_value TIPE_MODEL_MODE "$effective_config_path" || true)
    configured_mode="${configured_mode:-heuristic}"
    case "$configured_mode" in
        llama-cpp|ollama|openai|openai-compatible)
            ;;
        *)
            echo "--adapter-dry-run is not supported for configured TiPE model mode: $configured_mode" >&2
            echo "Run the current model self-test without --adapter-dry-run." >&2
            exit 2
            ;;
    esac
fi

request_path=$(mktemp)
pass_through_request_path=$(mktemp)
segment_suffix_request_path=$(mktemp)
pending_segment_request_path=$(mktemp)
generalized_correction_request_path=$(mktemp)
guarded_long_request_path=$(mktemp)
if [[ "$keep_request" != "1" ]]; then
    trap 'rm -f "$request_path" "$pass_through_request_path" "$segment_suffix_request_path" "$pending_segment_request_path" "$generalized_correction_request_path" "$guarded_long_request_path"' EXIT
fi

cat >"$request_path" <<'EOF'
protocol	1
preedit	woc
application	TiPESelfTest
candidates	我操	我曹	我
state	preedit_cursor	3	candidate_cursor	0	expanded	0
runtime_state	continuous	0
supervision_state	mode	active-preedit	active_preedit	1	recent_events	4	correction_events	9
selected_candidate	0	我操
visible_candidates	0:我操	1:我曹	2:我
numbered_candidates	1:0:我操	2:1:我曹	3:2:我
events	letter:w	letter:o	letter:c	rerank-requested:woc
correction_events	letter:w	letter:o	letter:c	candidate-selected:我	candidate-selected:操	letter:w	letter:o	letter:c	rerank-requested:woc
context	我	操
segment_chain	woc	wo	我	c	wocao	我操
preference	woc	我操	2
correction	woc	wocao	2
EOF

cat >"$pass_through_request_path" <<'EOF'
protocol	1
preedit
application	TiPESelfTest
candidates
state	preedit_cursor	0	candidate_cursor	0	expanded	0
runtime_state	continuous	0
supervision_state	mode	pass-through-only	active_preedit	0	recent_events	4	correction_events	4
events	space:	delete:	cursor-move:Down	observed:Tab
event_counts	space:1	delete:1	cursor-move:1	observed:1
correction_events	space:	delete:	cursor-move:Down	observed:Tab
correction_event_counts	space:1	delete:1	cursor-move:1	observed:1
EOF

cat >"$segment_suffix_request_path" <<'EOF'
protocol	1
preedit	c
application	TiPESelfTest
candidates	从	操	曹
state	preedit_cursor	1	candidate_cursor	0	expanded	0
runtime_state	continuous	0
supervision_state	mode	active-preedit	active_preedit	1	recent_events	5	correction_events	5
events	letter:w	letter:o	candidate-selected:我	letter:c	rerank-requested:c
event_counts	letter:3	candidate-selected:1	rerank-requested:1
correction_events	letter:w	letter:o	candidate-selected:我	letter:c	rerank-requested:c
correction_event_counts	letter:3	candidate-selected:1	rerank-requested:1
context	我
segment_chain	woc	wo	我	c	wocao	我操
correction	woc	wocao	2
EOF

cat >"$pending_segment_request_path" <<'EOF'
protocol	1
preedit	c
application	TiPESelfTest
candidates	操	从	曹
state	preedit_cursor	1	candidate_cursor	1	expanded	0
runtime_state	continuous	0
supervision_state	mode	active-preedit	active_preedit	1	recent_events	5	correction_events	5
selected_candidate	1	从
visible_candidates	0:操	1:从	2:曹
numbered_candidates	1:0:操	2:1:从	3:2:曹
events	letter:w	letter:o	candidate-selected:我	letter:c	rerank-requested:c
event_counts	letter:3	candidate-selected:1	rerank-requested:1
correction_events	letter:w	letter:o	candidate-selected:我	letter:c	rerank-requested:c
correction_event_counts	letter:3	candidate-selected:1	rerank-requested:1
context	我
pending_segment	woc	wo	我	c
EOF

cat >"$generalized_correction_request_path" <<'EOF'
protocol	1
preedit	ong
application	TiPESelfTest
candidates	弄	哦嗯个
state	preedit_cursor	3	candidate_cursor	0	expanded	0
runtime_state	continuous	0
supervision_state	mode	active-preedit	active_preedit	1	recent_events	4	correction_events	4
events	letter:o	letter:n	letter:g	rerank-requested:ong
event_counts	letter:3	rerank-requested:1
correction_events	letter:o	letter:n	letter:g	rerank-requested:ong
correction_event_counts	letter:3	rerank-requested:1
correction	ihao	nihao	2
EOF

cat >"$guarded_long_request_path" <<'EOF'
protocol	1
preedit	haodewokanyxiahaiyoumeiyu
application	TiPESelfTest
candidates	好的我看一下还有美誉	好的我看一下还有美与
state	preedit_cursor	24	candidate_cursor	0	expanded	0
runtime_state	continuous	0
supervision_state	mode	active-preedit	active_preedit	1	recent_events	4	correction_events	4
events	letter:h	letter:a	letter:o	rerank-requested:haodewokanyxiahaiyoumeiyu
event_counts	letter:24	rerank-requested:1
correction_events	letter:h	letter:a	letter:o	rerank-requested:haodewokanyxiahaiyoumeiyu
correction_event_counts	letter:24	rerank-requested:1
correction	ihao	nihao	2
EOF

run_model() {
    if [[ -n "$config_path" ]]; then
        TIPE_MODEL_CONFIG="$config_path" $command_line <"$request_path"
    else
        $command_line <"$request_path"
    fi
}

run_model_for() {
    local model_request_path="$1"
    if [[ -n "$config_path" ]]; then
        TIPE_MODEL_CONFIG="$config_path" $command_line <"$model_request_path"
    else
        $command_line <"$model_request_path"
    fi
}

run_model_dry_run() {
    local dry_run_request_path="${1:-$request_path}"
    if [[ -n "$config_path" ]]; then
        TIPE_MODEL_DRY_RUN=1 TIPE_MODEL_CONFIG="$config_path" $command_line <"$dry_run_request_path"
    else
        TIPE_MODEL_DRY_RUN=1 $command_line <"$dry_run_request_path"
    fi
}

validate_adapter_dry_run() {
    local output="$1"
    local expected_mode="$2"
    local ok_label="$3"
    local request_url request_json
    request_url=$(printf '%s\n' "$output" | sed -n 's/^request\t//p' | sed -n '1p')
    request_json=$(printf '%s\n' "$output" | sed -n 's/^request-json\t//p' | sed -n '1p')
    if [[ -z "$request_url" || -z "$request_json" ]]; then
        echo "adapter dry-run output must include request and request-json rows" >&2
        return 1
    fi
    command -v python3 >/dev/null 2>&1 || {
        echo "python3 is required to validate adapter dry-run JSON" >&2
        return 1
    }
    TIPE_SELF_TEST_JSON="$request_json" TIPE_SELF_TEST_EXPECT_MODE="$expected_mode" \
        TIPE_SELF_TEST_OK_LABEL="$ok_label" python3 - <<'PY'
import json
import os
import sys

try:
    payload = json.loads(os.environ["TIPE_SELF_TEST_JSON"])
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
sharing = prompt.get("data_sharing", {})
recent_shared = sharing.get("recent_input") is True
surrounding_shared = sharing.get("surrounding_text_and_application") is True
required = {
    "preedit",
    "candidates",
    "behavior_summary",
    "supervision_state",
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
expected_mode = os.environ["TIPE_SELF_TEST_EXPECT_MODE"]
ok_label = os.environ["TIPE_SELF_TEST_OK_LABEL"]
if prompt.get("supervision_mode") != expected_mode:
    print(f"unexpected supervision_mode: {prompt.get('supervision_mode')!r}", file=sys.stderr)
    sys.exit(1)
if prompt.get("supervision_state", {}).get("mode") != expected_mode:
    print(f"unexpected supervision_state mode: {prompt.get('supervision_state')!r}", file=sys.stderr)
    sys.exit(1)
if not recent_shared:
    private_fields = (
        "recent_events",
        "correction_events",
        "recent_context",
        "pending_segments",
        "recent_segment_chains",
        "known_preferences",
        "known_corrections",
    )
    if any(prompt.get(key) for key in private_fields):
        print("private cloud prompt should omit recent input and learned history", file=sys.stderr)
        sys.exit(1)
    if prompt.get("recent_history_summary", {}).get("available"):
        print("private cloud prompt should omit the history summary", file=sys.stderr)
        sys.exit(1)
if not surrounding_shared:
    if prompt.get("application") or any(prompt.get("surrounding_context", {}).values()):
        print("private cloud prompt should omit surrounding text and application", file=sys.stderr)
        sys.exit(1)
if expected_mode == "active-preedit" and ok_label == "request-json":
    if prompt.get("preedit") != "woc" or prompt.get("candidates") != ["我操", "我曹", "我"]:
        print("active-preedit prompt should preserve the composing request", file=sys.stderr)
        sys.exit(1)
if ok_label == "segment-chain-suffix-request-json":
    if prompt.get("preedit") != "c" or prompt.get("candidates") != ["从", "操", "曹"]:
        print("segment-chain suffix prompt should preserve the remaining-preedit request", file=sys.stderr)
        sys.exit(1)
    expected_chain = {
        "original_preedit": "woc",
        "consumed_preedit": "wo",
        "committed_text": "我",
        "remaining_preedit": "c",
        "corrected_full_preedit": "wocao",
        "combined_candidate": "我操",
    }
    if recent_shared and expected_chain not in prompt.get("recent_segment_chains", []):
        print("segment-chain suffix prompt should include the matching stored chain", file=sys.stderr)
        sys.exit(1)
    if recent_shared and prompt.get("recent_context") != ["我"]:
        print("segment-chain suffix prompt should include recent committed prefix context", file=sys.stderr)
        sys.exit(1)
if ok_label == "pending-segment-request-json":
    if prompt.get("preedit") != "c" or prompt.get("candidates") != ["操", "从", "曹"]:
        print("pending-segment prompt should preserve the selected suffix request", file=sys.stderr)
        sys.exit(1)
    expected_pending = {
        "original_preedit": "woc",
        "consumed_preedit": "wo",
        "committed_text": "我",
        "remaining_preedit": "c",
    }
    if recent_shared and expected_pending not in prompt.get("pending_segments", []):
        print("pending-segment prompt should include the current pending segment", file=sys.stderr)
        sys.exit(1)
    signals = prompt.get("behavior_summary", {}).get("supervised_learning_signals", [])
    expected_signal = {
        "kind": "pending_segment",
        "status": "confirmed_suffix",
        "original_preedit": "woc",
        "consumed_preedit": "wo",
        "committed_text": "我",
        "remaining_preedit": "c",
        "suffix_candidate": "从",
        "selected_index": 1,
        "corrected_full_preedit": "woc",
        "combined_candidate": "我从",
        "suggested_protocol": "segment_chain\twoc\two\t我\tc\twoc\t我从\t1",
    }
    if recent_shared and expected_signal not in signals:
        print(f"pending-segment confirmation signal missing: {signals!r}", file=sys.stderr)
        sys.exit(1)
elif expected_mode == "pass-through-only":
    if prompt.get("preedit") != "" or prompt.get("candidates") != []:
        print("pass-through-only prompt should keep empty preedit and no candidates", file=sys.stderr)
        sys.exit(1)
    expected_counts = {"space": 1, "delete": 1, "cursor-move": 1, "observed": 1}
    if recent_shared:
        if prompt.get("recent_events") != ["space:", "delete:", "cursor-move:Down", "observed:Tab"]:
            print("pass-through-only prompt should preserve observed key order", file=sys.stderr)
            sys.exit(1)
        if prompt.get("behavior_summary", {}).get("recent_event_counts") != expected_counts:
            print("pass-through-only prompt should summarize observed keys", file=sys.stderr)
            sys.exit(1)
        expected_context = {
            "active": True,
            "events": ["space:", "delete:", "cursor-move:Down", "observed:Tab"],
            "event_counts": expected_counts,
            "events_before_preedit": 4,
            "meaning": "all supervised pass-through keys because no preedit is active",
        }
        if prompt.get("behavior_summary", {}).get("preedit_leading_context") != expected_context:
            print("pass-through-only prompt should expose pass-through context explicitly", file=sys.stderr)
            sys.exit(1)
    elif prompt.get("behavior_summary", {}).get("recent_event_counts"):
        print("private pass-through prompt should not summarize hidden keys", file=sys.stderr)
        sys.exit(1)
if ok_label == "generalized-correction-request-json" and recent_shared:
    decisions = prompt.get("behavior_summary", {}).get("realtime_correction_decisions", [])
    expected = {
        "kind": "missing",
        "text": "n",
        "position": 0,
        "relative_to_end": False,
        "count": 2,
        "status": "applied",
        "reason": "ok",
        "corrected_preedit": "nong",
    }
    if expected not in decisions:
        print(f"generalized correction decision missing: {decisions!r}", file=sys.stderr)
        sys.exit(1)
if ok_label == "guarded-long-request-json" and recent_shared:
    decisions = prompt.get("behavior_summary", {}).get("realtime_correction_decisions", [])
    if not any(
        item.get("status") == "guarded" and item.get("reason") == "long-preedit" and item.get("text") == "n"
        for item in decisions
    ):
        print(f"long preedit should guard generalized correction decisions: {decisions!r}", file=sys.stderr)
        sys.exit(1)
PY
    printf 'model-dry-run-request\t%s\n' "$request_url"
    printf 'model-dry-run-ok\t%s\n' "$ok_label"
}

if [[ "$adapter_dry_run" == "1" ]]; then
    model_output=$(run_model_dry_run)
    pass_through_model_output=$(run_model_dry_run "$pass_through_request_path")
    segment_suffix_model_output=$(run_model_dry_run "$segment_suffix_request_path")
    pending_segment_model_output=$(run_model_dry_run "$pending_segment_request_path")
    generalized_correction_model_output=$(run_model_dry_run "$generalized_correction_request_path")
    guarded_long_model_output=$(run_model_dry_run "$guarded_long_request_path")
    printf 'self-test-command\t%s\n' "$command_line"
    validate_adapter_dry_run "$model_output" active-preedit request-json
    validate_adapter_dry_run "$pass_through_model_output" pass-through-only pass-through-request-json
    validate_adapter_dry_run "$segment_suffix_model_output" active-preedit segment-chain-suffix-request-json
    validate_adapter_dry_run "$pending_segment_model_output" active-preedit pending-segment-request-json
    validate_adapter_dry_run "$generalized_correction_model_output" active-preedit generalized-correction-request-json
    validate_adapter_dry_run "$guarded_long_model_output" active-preedit guarded-long-request-json
elif [[ -n "$config_path" ]]; then
    check_output=$(TIPE_MODEL_CONFIG="$config_path" "$checker" --command "$command_line" --request "$request_path")
    model_output=$(run_model)
    pending_check_output=$(TIPE_MODEL_CONFIG="$config_path" "$checker" --command "$command_line" --request "$pending_segment_request_path")
    pending_model_output=$(run_model_for "$pending_segment_request_path")
    printf 'self-test-command\t%s\n' "$command_line"
    printf '%s\n' "$check_output"
    if [[ -n "$model_output" ]]; then
        printf '%s\n' "$model_output" | sed 's/^/model-output\t/'
    else
        printf 'model-output\t(empty)\n'
    fi
    printf '%s\n' "$pending_check_output" | sed 's/^/pending-check\t/'
    if [[ -n "$pending_model_output" ]]; then
        printf '%s\n' "$pending_model_output" | sed 's/^/model-output-pending\t/'
    else
        printf 'model-output-pending\t(empty)\n'
    fi
else
    check_output=$("$checker" --command "$command_line" --request "$request_path")
    model_output=$(run_model)
    pending_check_output=$("$checker" --command "$command_line" --request "$pending_segment_request_path")
    pending_model_output=$(run_model_for "$pending_segment_request_path")
    printf 'self-test-command\t%s\n' "$command_line"
    printf '%s\n' "$check_output"
    if [[ -n "$model_output" ]]; then
        printf '%s\n' "$model_output" | sed 's/^/model-output\t/'
    else
        printf 'model-output\t(empty)\n'
    fi
    printf '%s\n' "$pending_check_output" | sed 's/^/pending-check\t/'
    if [[ -n "$pending_model_output" ]]; then
        printf '%s\n' "$pending_model_output" | sed 's/^/model-output-pending\t/'
    else
        printf 'model-output-pending\t(empty)\n'
    fi
fi
if [[ "$keep_request" == "1" ]]; then
    printf 'request\t%s\n' "$request_path"
    printf 'pass-through-request\t%s\n' "$pass_through_request_path"
    printf 'segment-suffix-request\t%s\n' "$segment_suffix_request_path"
    printf 'pending-segment-request\t%s\n' "$pending_segment_request_path"
fi
