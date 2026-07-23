#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
self_path="$script_dir/$(basename -- "${BASH_SOURCE[0]}")"

usage() {
    cat <<'EOF'
用法：tipe-learning-panel [--raw-panel] [--window] [--window-title TEXT] [--wait-missing] [--fallback-request PATH] [--replay] [--defer-replay] [--check] [--dry-run-model] [--explain-output] [--learn-output] [--preferences PATH] [--command CMD] [--config PATH] [REQUEST_TSV]

从 TiPE 模型请求 TSV 渲染只读学习/调试面板。
默认不会调用模型，不会重启 fcitx5，不会切换输入法，也不会写入数据。

不传 REQUEST_TSV 时，如果有管道输入就读取 stdin，否则读取默认 TiPE 模型请求路径。
--raw-panel 会输出底层 panel<TAB>... 协议行。
--window 会打开 GTK 学习/调试窗口。
--window-title TEXT 用来设置 GTK 窗口标题。
--replay 会对同一个请求运行当前配置的模型辅助程序。
--dry-run-model 搭配 --replay 时，只预览适配器请求 JSON，不发 HTTP 请求。
--explain-output 搭配 --replay 时，总结模型返回的安全结果。
--learn-output 搭配 --replay 时，把模型返回的安全结果写入 TiPE 偏好。
--window 读取文件路径时，窗口打开期间会刷新面板文件，所以可以直接观察实时请求路径。
--wait-missing 搭配 --window 时，实时请求文件暂时不存在也会先打开窗口，等请求出现后刷新。
--fallback-request PATH 搭配 --wait-missing 时，等待当前实时请求期间先显示上一次请求。
如果没有当前请求和上一次请求，会自动从 supervision-history.tsv 抽取最近一条请求作为只读回放。
--window 搭配 --replay 时，窗口会出现“分析”按钮，后台不会反复调用模型。
加上 --defer-replay 后，打开窗口时也不先调用模型，只在点击“分析”后运行一次。
EOF
}

require_value() {
    local option="$1"
    if [[ $# -lt 2 || -z "${2:-}" ]]; then
        echo "$option 需要一个值" >&2
        exit 2
    fi
}

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
    local live_request=""
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        live_request="$XDG_CACHE_HOME/tipe/supervision-current.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        live_request="$HOME/.cache/tipe/supervision-current.tsv"
    fi
    if [[ -n "$live_request" && -r "$live_request" ]]; then
        printf '%s\n' "$live_request"
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

default_dictionary_history_path() {
    if [[ ${TIPE_LIBIME_USER_HISTORY+x} ]]; then
        printf '%s\n' "$TIPE_LIBIME_USER_HISTORY"
    elif [[ -n "${XDG_DATA_HOME:-}" ]]; then
        printf '%s\n' "$XDG_DATA_HOME/tipe/libime/user.history"
    elif [[ -n "${HOME:-}" ]]; then
        printf '%s\n' "$HOME/.local/share/tipe/libime/user.history"
    else
        printf '%s\n' ""
    fi
}

raw_panel=0
window_output=0
wait_missing=0
replay_model=0
defer_replay=0
check_replay=0
dry_run_model=0
explain_output=0
learn_output=0
model_command=""
config_path=""
preferences_path=""
request_path=""
fallback_request_path=""
window_title="TiPE 学习面板"
used_default_request=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --raw-panel)
            raw_panel=1
            shift
            ;;
        --window)
            window_output=1
            shift
            ;;
        --window-title)
            require_value "$1" "${2:-}"
            window_title="$2"
            window_output=1
            shift 2
            ;;
        --wait-missing)
            wait_missing=1
            shift
            ;;
        --fallback-request)
            require_value "$1" "${2:-}"
            fallback_request_path="$2"
            shift 2
            ;;
        --replay)
            replay_model=1
            shift
            ;;
        --defer-replay)
            defer_replay=1
            replay_model=1
            shift
            ;;
        --check)
            check_replay=1
            shift
            ;;
        --dry-run-model)
            dry_run_model=1
            replay_model=1
            shift
            ;;
        --explain-output)
            explain_output=1
            replay_model=1
            shift
            ;;
        --learn-output)
            learn_output=1
            replay_model=1
            shift
            ;;
        --preferences)
            require_value "$1" "${2:-}"
            preferences_path="$2"
            replay_model=1
            shift 2
            ;;
        --command)
            require_value "$1" "${2:-}"
            model_command="$2"
            replay_model=1
            shift 2
            ;;
        --config)
            require_value "$1" "${2:-}"
            config_path="$2"
            replay_model=1
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "未知参数：$1" >&2
            exit 2
            ;;
        *)
            if [[ -n "$request_path" ]]; then
                usage >&2
                exit 2
            fi
            request_path="$1"
            shift
            ;;
    esac
done

if [[ -z "$request_path" ]]; then
    live_request=""
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        live_request="$XDG_CACHE_HOME/tipe/supervision-current.tsv"
    elif [[ -n "${HOME:-}" ]]; then
        live_request="$HOME/.cache/tipe/supervision-current.tsv"
    fi
    if [[ -n "$live_request" && -r "$live_request" ]]; then
        request_path="$live_request"
        used_default_request=1
    elif [[ -t 0 ]]; then
        request_path=$(default_request_path)
        used_default_request=1
    else
        request_path="/dev/stdin"
    fi
fi
if [[ "$request_path" == "-" ]]; then
    request_path="/dev/stdin"
fi

cleanup_paths=()
cleanup_pids=()
cleanup() {
    local pid
    for pid in "${cleanup_pids[@]}"; do
        kill "$pid" >/dev/null 2>&1 || true
        wait "$pid" >/dev/null 2>&1 || true
    done
    if [[ "${#cleanup_paths[@]}" -gt 0 ]]; then
        rm -f "${cleanup_paths[@]}"
    fi
}
trap cleanup EXIT

history_fallback_request_path=""
history_source_path=""
materialize_latest_history_request() {
    local history_path="$1"
    local output_path
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
        history_fallback_request_path="$output_path"
        return 0
    fi
    rm -f "$output_path"
    return 1
}

if [[ "$request_path" == "/dev/stdin" ]]; then
    tmp_request=$(mktemp)
    cleanup_paths+=("$tmp_request")
    cat >"$tmp_request"
    request_path="$tmp_request"
fi

if [[ -z "$fallback_request_path" || ! -r "$fallback_request_path" ]]; then
    history_path=$(default_history_path)
    if materialize_latest_history_request "$history_path"; then
        history_source_path="$history_path"
        fallback_request_path="$history_fallback_request_path"
    fi
fi

emit_history_panel_rows() {
    local history_path current_request_path current_preedit
    history_path="${history_source_path:-$(default_history_path)}"
    [[ -r "$history_path" ]] || return 0
    current_request_path="$request_path"
    if [[ ! -r "$current_request_path" && -n "$fallback_request_path" && -r "$fallback_request_path" ]]; then
        current_request_path="$fallback_request_path"
    fi
    if [[ $# -gt 0 ]]; then
        current_preedit="$1"
    else
        current_preedit=$(request_preedit "$current_request_path" 2>/dev/null || true)
    fi
    TIPE_HISTORY_PATH="$history_path" TIPE_PANEL_CURRENT_PREEDIT="$current_preedit" python3 - <<'PY'
import collections
import os
from pathlib import Path

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

path = Path(os.environ.get("TIPE_HISTORY_PATH", ""))
current_preedit = os.environ.get("TIPE_PANEL_CURRENT_PREEDIT", "")
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

parsed = []
for header, body in records[-24:]:
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
            fields["candidate_count"] = str(max(0, len(parts) - 1))
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

if not parsed:
    raise SystemExit(0)

def count_pairs(items):
    counts = collections.Counter()
    for item in items:
        name, _, value = item.partition(":")
        if not name:
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
apps = collections.Counter(item.get("application", "") for item in parsed if item.get("application"))
selected = collections.Counter(item.get("selected_candidate", "") for item in parsed if learnable_selected(item))
preedit_selected = collections.Counter(
    (item.get("preedit", ""), item.get("selected_candidate", ""))
    for item in parsed
    if learnable_selected(item)
)
last_seen = {}
for index, item in enumerate(parsed):
    for key in ("preedit", "application"):
        if key == "preedit" and record_mode(item) != "active-preedit":
            continue
        value = item.get(key, "")
        if value:
            last_seen[(key, value)] = index
    if learnable_selected(item):
        last_seen[("selected_candidate", item["selected_candidate"])] = index
        last_seen[("preedit_selected", item["preedit"], item["selected_candidate"])] = index
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
    for pair in possible_corrections_from_events(record_events, record_preedit):
        history_corrections[pair] += 1
        last_correction_seen[pair] = index
active = sum(1 for item in parsed if record_mode(item) == "active-preedit")
pass_through = sum(1 for item in parsed if record_mode(item) == "pass-through-only")
segment_chains = sum(int(item.get("segment_chain_count", "0")) for item in parsed)
pending_segments = sum(int(item.get("pending_segment_count", "0")) for item in parsed)

print(
    "panel\thistory\tsummary"
    f"\trecords\t{len(parsed)}"
    f"\tactive\t{active}"
    f"\tpass-through\t{pass_through}"
    f"\tsegment-chains\t{segment_chains}"
    f"\tpending-segments\t{pending_segments}"
    f"\tpath\t{path}"
)
ranked_preedits = sorted(preedits.items(), key=lambda item: (-item[1], -last_seen.get(("preedit", item[0]), -1), item[0]))
ranked_selected = sorted(selected.items(), key=lambda item: (-item[1], -last_seen.get(("selected_candidate", item[0]), -1), item[0]))
ranked_preedit_selected = sorted(
    preedit_selected.items(),
    key=lambda item: (-item[1], -last_seen.get(("preedit_selected", item[0][0], item[0][1]), -1), item[0][0], item[0][1]),
)
ranked_apps = sorted(apps.items(), key=lambda item: (-item[1], -last_seen.get(("application", item[0]), -1), item[0]))

for rank, (preedit, count) in enumerate(ranked_preedits[:5], 1):
    print(f"panel\thistory\tpreedit\t{rank}\t{preedit}\t{count}")
for rank, (candidate, count) in enumerate(ranked_selected[:5], 1):
    print(f"panel\thistory\tselected-candidate\t{rank}\t{candidate}\t{count}")
for rank, ((preedit, candidate), count) in enumerate(ranked_preedit_selected[:8], 1):
    print(f"panel\thistory\tpreedit-selected\t{rank}\t{preedit}\t{candidate}\t{count}")
learnable_rank = 0
for (preedit, candidate), count in ranked_preedit_selected[:8]:
    if count < 2:
        continue
    learnable_rank += 1
    weight = min(max(count, 2), 10)
    print(
        "panel\thistory\tlearnable-preference"
        f"\t{learnable_rank}\t{preedit}\t{candidate}\t{count}\tpreference\t{preedit}\t{candidate}\t{weight}"
    )
for rank, (app, count) in enumerate(ranked_apps[:3], 1):
    print(f"panel\thistory\tapplication\t{rank}\t{app}\t{count}")
for rank, (kind, count) in enumerate(events.most_common(8), 1):
    print(f"panel\thistory\tevent-count\t{rank}\t{kind}\t{count}")
for rank, (kind, count) in enumerate(active_events.most_common(8), 1):
    print(f"panel\thistory\tactive-event-count\t{rank}\t{kind}\t{count}")
for rank, (kind, count) in enumerate(pass_through_events.most_common(8), 1):
    print(f"panel\thistory\tpass-through-event-count\t{rank}\t{kind}\t{count}")
for rank, (kind, count) in enumerate(correction_events.most_common(8), 1):
    print(f"panel\thistory\tcorrection-event-count\t{rank}\t{kind}\t{count}")
for rank, ((typo, corrected), count) in enumerate(
    sorted(
        history_corrections.items(),
        key=lambda item: (-item[1], -last_correction_seen.get(item[0], -1), item[0][0], item[0][1]),
    )[:8],
    1,
):
    print(f"panel\thistory\tcorrection\t{rank}\t{typo}\t{corrected}\t{count}")
    if count >= 2 and current_preedit and current_preedit in {typo, corrected}:
        print(
            "panel\thistory\tlearnable-correction"
            f"\t{rank}\t{typo}\t{corrected}\t{count}\tcorrection\t{typo}\t{corrected}"
        )
        print(f"panel\tlearning\tstatus-suggested-protocol\t{rank}\tcorrection\t{typo}\t{corrected}")
        print(f"panel\tlearning\tstatus-signal-count\thistory_correction\t1")
PY
}

emit_training_panel_rows() {
    local training_export stats
    training_export=$(helper_path tipe-training-export) || return 0
    stats=$("$training_export" --stats 2>/dev/null) || return 0
    TIPE_TRAINING_STATS="$stats" python3 - <<'PY'
import json
import os

try:
    stats = json.loads(os.environ.get("TIPE_TRAINING_STATS", "{}"))
except (TypeError, ValueError):
    raise SystemExit(0)
if stats.get("schema") != "tipe.training-export.stats.v1":
    raise SystemExit(0)
tasks = stats.get("tasks", {})
choices = int(tasks.get("candidate-selected", 0)) + int(tasks.get("raw-committed", 0))
observations = int(tasks.get("observation", 0))
print(
    "panel\ttraining\tsummary"
    f"\trecords\t{int(stats.get('records', 0))}"
    f"\tlearnable\t{int(stats.get('samples', 0))}"
    f"\tchoices\t{choices}"
    f"\tobservations\t{observations}"
)
PY
}

emit_dictionary_history_panel_rows() {
    local path status bytes reason owner mode mode_value disabled history_override_set
    path=$(default_dictionary_history_path)
    disabled="${TIPE_DISABLE_LIBIME_LEARNING-0}"
    history_override_set=0
    if [[ ${TIPE_LIBIME_USER_HISTORY+x} ]]; then
        history_override_set=1
    fi
    status="waiting"
    bytes=0
    reason="first-selection"

    if [[ "$disabled" != 0 ]] ||
        [[ "$history_override_set" == 1 && -z "${TIPE_LIBIME_USER_HISTORY:-}" ]]; then
        status="disabled"
        reason="environment"
    elif [[ -z "$path" ]]; then
        status="disabled"
        reason="no-data-home"
    elif [[ -e "$path" || -L "$path" ]]; then
        status="error"
        reason="invalid-file"
        if [[ -f "$path" && ! -L "$path" && -r "$path" ]]; then
            owner=$(stat -c '%u' -- "$path" 2>/dev/null || true)
            mode=$(stat -c '%a' -- "$path" 2>/dev/null || true)
            bytes=$(stat -c '%s' -- "$path" 2>/dev/null || printf '0')
            if [[ "$mode" =~ ^[0-7]{3,4}$ ]]; then
                mode_value=$((8#$mode))
            else
                mode_value=-1
            fi
            if [[ "$owner" == "$(id -u)" && "$bytes" =~ ^[0-9]+$ && "$bytes" -gt 0 && "$bytes" -le 4194304 &&
                "$mode_value" -ge 0 && $((mode_value & 077)) -eq 0 ]]; then
                status="ready"
                reason="healthy"
            fi
        fi
    fi

    printf 'panel\tlearning\tdictionary-history\tstatus\t%s\tbytes\t%s\tpath\t%s\treason\t%s\n' \
        "$status" "$bytes" "$path" "$reason"
}

if [[ "$request_path" != "/dev/stdin" && ! -r "$request_path" && "$wait_missing" != 1 ]]; then
    echo "无法读取模型请求：$request_path" >&2
    if [[ "$used_default_request" == 1 ]]; then
        echo "目前还没有默认 TiPE 模型请求文件。" >&2
        echo "可以先用 dump 模型配置启动 TiPE，然后在输入拼音时按 F9 生成：" >&2
        echo "  tipe-model-config --write dump --dump-path $request_path" >&2
        echo "  tipe-restart-fcitx5" >&2
        echo "也可以直接给 tipe-learning-panel 传入一个明确的请求文件路径。" >&2
    fi
    exit 1
fi

model_explain=$(helper_path tipe-model-explain) || {
    echo "找不到 tipe-model-explain 辅助程序" >&2
    exit 1
}

request_source_for_path() {
    local path="$1"
    local cache_tipe_dir=""
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        cache_tipe_dir="$XDG_CACHE_HOME/tipe"
    elif [[ -n "${HOME:-}" ]]; then
        cache_tipe_dir="$HOME/.cache/tipe"
    fi
    if [[ -n "$cache_tipe_dir" && "$path" == "$cache_tipe_dir/supervision-current.tsv" ]]; then
        printf '%s\n' live-supervision
    elif [[ -n "$cache_tipe_dir" && "$path" == "$cache_tipe_dir/supervision-last.tsv" ]]; then
        printf '%s\n' last-supervision
    elif [[ -n "$history_fallback_request_path" && "$path" == "$history_fallback_request_path" ]]; then
        printf '%s\n' history-supervision
    else
        printf '%s\n' request-file
    fi
}

emit_request_state_rows() {
    local path="$1"
    local source="$2"
    local mtime=""
    printf 'panel\tstate\trequest-source\t%s\t%s\n' "$source" "$path"
    if [[ -r "$path" ]]; then
        mtime=$(stat -c '%Y' "$path" 2>/dev/null || true)
        if [[ -n "$mtime" ]]; then
            printf 'panel\tstate\trequest-mtime\t%s\n' "$mtime"
        fi
    fi
}

request_preedit() {
    local path="$1"
    [[ -r "$path" ]] || return 1
    awk -F '\t' '$1 == "preedit" { print $2; exit }' "$path"
}

emit_preedit_learning_evidence_rows() {
    local path="$1"
    local preedit check_preferences query_output status line kind first second third fourth fifth sixth seventh eighth ninth
    preedit=$(request_preedit "$path" || true)
    [[ -n "$preedit" ]] || return 0
    check_preferences=$(helper_path tipe-check-preferences) || return 0
    if [[ -n "$preferences_path" ]]; then
        query_output=$("$check_preferences" --query-only --preedit "$preedit" "$preferences_path" 2>/dev/null) || status=$?
    else
        query_output=$("$check_preferences" --query-only --preedit "$preedit" 2>/dev/null) || status=$?
    fi
    if [[ -n "${status:-}" ]]; then
        printf 'panel\tlearning\tevidence-status\terror\t%s\t%s\n' "$preedit" "$status"
        return 0
    fi
    while IFS= read -r line || [[ -n "$line" ]]; do
        IFS=$'\t' read -r kind first second third fourth fifth sixth seventh eighth ninth <<< "$line"
        case "$kind" in
            query-preference)
                printf 'panel\tlearning\tevidence-preference\t%s\t%s\t%s\t%s\n' \
                    "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                ;;
            query-legacy-preference)
                printf 'panel\tlearning\tevidence-legacy-preference\t%s\t%s\t%s\n' \
                    "${first:-}" "${second:-}" "${third:-}"
                ;;
            query-supervised-raw-token)
                printf 'panel\tlearning\tevidence-supervised-raw-token\t%s\t%s\t%s\t%s\n' \
                    "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                ;;
            query-correction)
                printf 'panel\tlearning\tevidence-correction\t%s\t%s\t%s\t%s\t%s\n' \
                    "${first:-}" "${second:-}" "${third:-}" "${fourth:-}" "${fifth:-}"
                ;;
            query-segment-chain)
                printf 'panel\tlearning\tevidence-segment-chain\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "${first:-}" "${second:-}" "${third:-}" "${fourth:-}" "${fifth:-}" "${sixth:-}" "${seventh:-}" \
                    "${eighth:-}"
                ;;
            query-effect)
                case "${first:-}" in
                    rank-preference|raw-preference|correction-borrow|correction-target)
                        printf 'panel\tlearning\tevidence-effect\t%s\t%s\t%s\t%s\t%s\n' \
                            "${first:-}" "${second:-}" "${third:-}" "${fourth:-}" "${fifth:-}"
                        ;;
                    legacy-raw-preference|supervised-raw-token)
                        printf 'panel\tlearning\tevidence-effect\t%s\t%s\t%s\t%s\n' \
                            "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                        ;;
                    segment-chain-full|segment-chain-suffix|segment-chain-corrected-full)
                        printf 'panel\tlearning\tevidence-effect\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                            "${first:-}" "${second:-}" "${third:-}" "${fourth:-}" "${fifth:-}" "${sixth:-}" \
                            "${seventh:-}" "${eighth:-}" "${ninth:-}"
                        ;;
                esac
                ;;
            query-summary)
                printf 'panel\tlearning\tevidence-summary\t%s\n' "${line#query-summary	}"
                ;;
        esac
    done <<< "$query_output"
}

panel_rows_for_request() {
    local include_preference_query="${1:-1}"
    if [[ ! -r "$request_path" ]]; then
        if [[ -n "$fallback_request_path" && -r "$fallback_request_path" ]]; then
            "$model_explain" --panel "$fallback_request_path" | awk -F '\t' '$1 == "panel"'
            if [[ "$include_preference_query" == 1 ]]; then
                emit_preedit_learning_evidence_rows "$fallback_request_path"
            fi
            fallback_source=$(request_source_for_path "$fallback_request_path")
            if [[ "$fallback_source" == "history-supervision" ]]; then
                printf 'panel\tstate\tstatus\tshowing-history-supervision\n'
            else
                printf 'panel\tstate\tstatus\tshowing-last-supervision\n'
            fi
            emit_request_state_rows "$fallback_request_path" "$fallback_source"
            return 0
        fi
        printf 'panel\tstate\tpreedit\t\n'
        printf 'panel\tstate\tstatus\twaiting-for-live-supervision\n'
        printf 'panel\tstate\trequest-source\twaiting-for-live-supervision\t%s\n' "$request_path"
        printf 'panel\tcandidates\ttotal\t0\tvisible\t0\tnumbered\t0\n'
        printf 'panel\tsupervision\trecent-events\t0\tcorrection-events\t0\tcontext\t0\tsegment-chains\t0\tpending-segments\t0\n'
        printf 'panel\tsupervision\tmodel-input\tpreedit\t\tcandidates\t0\tvisible\t0\tnumbered\t0\tcontext\t0\tsegment-chains\t0\tpending-segments\t0\n'
        printf 'panel\tsupervision\tevent-trail\trecent\t0\tlimit\t64\tpurpose\twaiting-for-current-input\n'
        printf 'panel\tsupervision\tcorrection-trail\trecent\t0\tlimit\t256\tpurpose\twaiting-for-current-input\n'
        printf 'panel\tbehavior\traw-english-hint\t0\tsource\twaiting-for-live-supervision\n'
        return 0
    fi
    "$model_explain" --panel "$request_path" | awk -F '\t' '$1 == "panel"'
    if [[ "$include_preference_query" == 1 ]]; then
        emit_preedit_learning_evidence_rows "$request_path"
    fi
    emit_request_state_rows "$request_path" "$(request_source_for_path "$request_path")"
}

emit_model_config_panel_rows() {
    local model_config line key value
    model_config=$(helper_path tipe-model-config) || return 0
    if ! "$model_config" --show 2>/dev/null | while IFS=$'\t' read -r section key value rest; do
        [[ "$section" == "model-status" ]] || continue
        case "$key" in
            configured-mode|backend|kind|model|base-url|chat-path|api-key-env|api-key-file|api-key-source|api-key-runtime|custom-command|dump-path|timeout|http-timeout|temperature|max-tokens|invocation|llama-command|llama-command-valid|llama-model-readable|llama-threads|llama-context|continuous-default|training-context|training-surrounding|send-recent-input|send-surrounding)
                printf 'panel\tmodel-config\t%s\t%s\n' "$key" "${value:-}"
                ;;
            configured-command|configured-command-valid|process-command|process-command-scope|process-command-active|process-command-active-scope|runtime-verification)
                printf 'panel\tmodel-config\t%s\t%s\n' "$key" "${value:-}"
                ;;
            active-command|config-active|activation-hint)
                printf 'panel\tmodel-config\t%s\t%s\n' "$key" "${value:-}"
                ;;
            personal-model|personal-model-*)
                printf 'panel\tmodel-config\t%s\t%s\n' "$key" "${value:-}"
                ;;
            analyze-window|supervision-window|analyze-learn|self-test-command|dry-run-test-command|dry-run-test-supported)
                printf 'panel\tmodel-config\t%s\t%s\n' "$key" "${value:-}"
                ;;
        esac
    done; then
        return 0
    fi
}

replay_args_for_request() {
    replay_args=(--request "$request_path")
    if [[ "$check_replay" == 1 ]]; then
        replay_args+=(--check)
    fi
    if [[ "$dry_run_model" == 1 ]]; then
        replay_args+=(--dry-run-model)
    fi
    if [[ "$explain_output" == 1 ]]; then
        replay_args+=(--explain-output)
    fi
    if [[ "$learn_output" == 1 ]]; then
        replay_args+=(--learn-output)
    fi
    if [[ -n "$preferences_path" ]]; then
        replay_args+=(--preferences "$preferences_path")
    fi
    if [[ -n "$model_command" ]]; then
        replay_args+=(--command "$model_command")
    fi
    if [[ -n "$config_path" ]]; then
        replay_args+=(--config "$config_path")
    fi
}

raw_panel_args_for_request() {
    raw_panel_args=(--raw-panel)
    if [[ "$wait_missing" == 1 ]]; then
        raw_panel_args+=(--wait-missing)
    fi
    if [[ -n "$fallback_request_path" ]]; then
        raw_panel_args+=(--fallback-request "$fallback_request_path")
    fi
    if [[ "$replay_model" == 1 ]]; then
        raw_panel_args+=(--replay)
    fi
    if [[ "$check_replay" == 1 ]]; then
        raw_panel_args+=(--check)
    fi
    if [[ "$dry_run_model" == 1 ]]; then
        raw_panel_args+=(--dry-run-model)
    fi
    if [[ "$explain_output" == 1 ]]; then
        raw_panel_args+=(--explain-output)
    fi
    if [[ "$learn_output" == 1 ]]; then
        raw_panel_args+=(--learn-output)
    fi
    if [[ -n "$preferences_path" ]]; then
        raw_panel_args+=(--preferences "$preferences_path")
    fi
    if [[ -n "$model_command" ]]; then
        raw_panel_args+=(--command "$model_command")
    fi
    if [[ -n "$config_path" ]]; then
        raw_panel_args+=(--config "$config_path")
    fi
    raw_panel_args+=("$request_path")
}

emit_replay_panel_rows() {
    [[ "$replay_model" == 1 ]] || return 0
    local model_replay replay_stdout replay_stderr status output_rank line replay_request_path
    local kind first second third fourth fifth sixth seventh eighth
    model_replay=$(helper_path tipe-model-replay) || {
        printf 'panel\tmodel-replay\tstatus\terror\t%s\n' "找不到 tipe-model-replay 辅助程序"
        return 0
    }
    replay_request_path="$request_path"
    if [[ ! -r "$replay_request_path" && -n "$fallback_request_path" && -r "$fallback_request_path" ]]; then
        replay_request_path="$fallback_request_path"
    fi
    replay_stdout=$(mktemp)
    replay_stderr=$(mktemp)
    replay_args_for_request
    for index in "${!replay_args[@]}"; do
        if [[ "${replay_args[$index]}" == "$request_path" ]]; then
            replay_args[$index]="$replay_request_path"
        fi
    done
    if "$model_replay" "${replay_args[@]}" >"$replay_stdout" 2>"$replay_stderr"; then
        printf 'panel\tmodel-replay\tstatus\tok\n'
    else
        status=$?
        printf 'panel\tmodel-replay\tstatus\terror\t%s\n' "$status"
    fi
    while IFS= read -r line || [[ -n "$line" ]]; do
        IFS=$'\t' read -r kind first second third fourth fifth sixth seventh eighth ninth <<< "$line"
        case "$kind" in
            wrapper-ok)
                printf 'panel\tmodel-replay\twrapper\t%s\trows\t%s\n' "${first:-}" "${third:-}"
                ;;
            model-output-accepted)
                case "$first" in
                    candidate)
                        printf 'panel\tmodel-output\taccepted-candidate\t%s\t%s\n' "${second:-}" "${third:-}"
                        ;;
                    correction)
                        printf 'panel\tmodel-output\taccepted-correction\t%s\t%s\t%s\n' "${second:-}" "${third:-}" "${fourth:-}"
                        ;;
                    preference)
                        printf 'panel\tmodel-output\taccepted-preference\t%s\t%s\t%s\n' "${second:-}" "${third:-}" "${fourth:-}"
                        ;;
                    segment_chain)
                        printf 'panel\tmodel-output\taccepted-segment-chain\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                            "${second:-}" "${third:-}" "${fourth:-}" "${fifth:-}" "${sixth:-}" "${seventh:-}" "${eighth:-}"
                        ;;
                    *)
                        printf 'panel\tmodel-output\taccepted\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                        ;;
                esac
                ;;
            model-output-summary)
                printf 'panel\tmodel-output\tsummary\t%s\n' "${line#model-output-summary	}"
                ;;
            model-output-learned)
                printf 'panel\tmodel-output\tlearned\t%s\n' "${line#model-output-learned	}"
                ;;
            model-output-learned-top-preference)
                printf 'panel\tmodel-output\tlearned-top-preference\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                ;;
            model-output-learned-top-correction)
                printf 'panel\tmodel-output\tlearned-top-correction\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                ;;
            model-output-learned-top-correction-pattern)
                printf 'panel\tmodel-output\tlearned-top-correction-pattern\t%s\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}" "${fifth:-}"
                ;;
            model-output-learned-top-segment-chain)
                if [[ -n "${eighth:-}" ]]; then
                    printf 'panel\tmodel-output\tlearned-top-segment-chain\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                        "${first:-}" "${second:-}" "${third:-}" "${fourth:-}" "${fifth:-}" "${sixth:-}" "${seventh:-}" "${eighth:-}"
                else
                    printf 'panel\tmodel-output\tlearned-top-segment-chain\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                fi
                ;;
            model-output-preferences-summary)
                printf 'panel\tmodel-output\tpreferences-summary\t%s\n' "${line#model-output-preferences-summary	}"
                ;;
            model-output-preferences-top-preference)
                printf 'panel\tmodel-output\tpreferences-top-preference\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                ;;
            model-output-preferences-top-legacy-preference)
                printf 'panel\tmodel-output\tpreferences-top-legacy-preference\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}"
                ;;
            model-output-preferences-top-correction)
                printf 'panel\tmodel-output\tpreferences-top-correction\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                ;;
            model-output-preferences-top-correction-pattern)
                printf 'panel\tmodel-output\tpreferences-top-correction-pattern\t%s\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}" "${fifth:-}"
                ;;
            model-output-preferences-top-segment-chain)
                printf 'panel\tmodel-output\tpreferences-top-segment-chain\t%s\t%s\t%s\t%s\n' "${first:-}" "${second:-}" "${third:-}" "${fourth:-}"
                ;;
            model-output-note)
                printf 'panel\tmodel-output\tnote\t%s\n' "${line#model-output-note	}"
                ;;
            model-output-rejected-summary)
                printf 'panel\tmodel-output\trejected-summary\t%s\n' "${line#model-output-rejected-summary	}"
                ;;
            model-output-rejected-row)
                printf 'panel\tmodel-output\trejected-row\t%s\n' "${line#model-output-rejected-row	}"
                ;;
            *)
                printf 'panel\tmodel-replay\tcheck\t%s\n' "$line"
                ;;
        esac
    done <"$replay_stderr"
    output_rank=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        output_rank=$((output_rank + 1))
        IFS=$'\t' read -r kind first second third fourth <<< "$line"
        case "$kind" in
            candidate)
                printf 'panel\tmodel-output\trow-candidate\t%s\t%s\n' "$output_rank" "${first:-}"
                ;;
            correction)
                printf 'panel\tmodel-output\trow-correction\t%s\t%s\t%s\n' "$output_rank" "${first:-}" "${second:-}"
                ;;
            *)
                printf 'panel\tmodel-output\trow\t%s\t%s\n' "$output_rank" "$line"
                ;;
        esac
    done <"$replay_stdout"
    rm -f "$replay_stdout" "$replay_stderr"
}

panel_rows_with_optional_replay() {
    panel_rows_for_request
    emit_history_panel_rows
    emit_training_panel_rows
    emit_dictionary_history_panel_rows
    emit_model_config_panel_rows
    if [[ "$replay_model" == 1 && "$defer_replay" == 1 ]]; then
        printf 'panel\tmodel-replay\tmode\tmanual\tlearn-output\t%s\ttrigger\tAnalyze\n' "$learn_output"
    fi
    emit_replay_panel_rows
}

if [[ "$raw_panel" == 1 ]]; then
    panel_rows=$(panel_rows_with_optional_replay)
    printf '%s\n' "$panel_rows"
    exit 0
fi

if [[ "$window_output" == 1 ]]; then
    panel_window_args=()
    if [[ -n "${TIPE_LEARNING_PANEL_WINDOW_COMMAND:-}" ]]; then
        read -r -a panel_window_args <<< "$TIPE_LEARNING_PANEL_WINDOW_COMMAND"
    else
        panel_window=$(helper_path tipe-learning-panel-window) || {
            echo "找不到 tipe-learning-panel-window 窗口程序" >&2
            exit 1
        }
        panel_window_args=("$panel_window")
    fi
    panel_window_args+=(--title "$window_title")
    if model_config_helper=$(helper_path tipe-model-config); then
        panel_window_args+=(--model-config-command "$model_config_helper")
    fi
    if personal_train_helper=$(helper_path tipe-personal-model-train); then
        panel_window_args+=(--personal-train-command "$personal_train_helper")
    fi
    panel_path=$(mktemp)
    panel_tmp="$panel_path.next"
    panel_live_tmp="$panel_path.live.next"
    panel_context_path=$(mktemp)
    cleanup_paths+=("$panel_path" "$panel_tmp" "$panel_live_tmp" "$panel_context_path")
    refresh_context_file() {
        local panel_context_tmp="$panel_context_path.next.$BASHPID"
        if {
            emit_history_panel_rows ""
            emit_training_panel_rows
            emit_dictionary_history_panel_rows
            emit_model_config_panel_rows
        } >"$panel_context_tmp"; then
            mv "$panel_context_tmp" "$panel_context_path"
            return 0
        fi
        rm -f "$panel_context_tmp"
        return 1
    }
    emit_cached_history_learning_rows() {
        local current_request_path current_preedit panel section kind rank typo corrected count rest
        current_request_path="$request_path"
        if [[ ! -r "$current_request_path" && -n "$fallback_request_path" && -r "$fallback_request_path" ]]; then
            current_request_path="$fallback_request_path"
        fi
        current_preedit=$(request_preedit "$current_request_path" 2>/dev/null || true)
        [[ -n "$current_preedit" && -r "$panel_context_path" ]] || return 0
        while IFS=$'\t' read -r panel section kind rank typo corrected count rest; do
            [[ "$panel" == "panel" && "$section" == "history" && "$kind" == "correction" &&
                "$count" =~ ^[0-9]+$ && "$count" -ge 2 &&
                ("$current_preedit" == "$typo" || "$current_preedit" == "$corrected") ]] || continue
            printf 'panel\thistory\tlearnable-correction\t%s\t%s\t%s\t%s\tcorrection\t%s\t%s\n' \
                "$rank" "$typo" "$corrected" "$count" "$typo" "$corrected"
            printf 'panel\tlearning\tstatus-suggested-protocol\t%s\tcorrection\t%s\t%s\n' \
                "$rank" "$typo" "$corrected"
            printf 'panel\tlearning\tstatus-signal-count\thistory_correction\t1\n'
        done <"$panel_context_path"
    }
    emit_cached_context_rows() {
        cat "$panel_context_path"
        emit_cached_history_learning_rows
    }
    refresh_panel_file() {
        if {
            panel_rows_for_request
            emit_cached_context_rows
            if [[ "$replay_model" == 1 && "$defer_replay" == 1 ]]; then
                printf 'panel\tmodel-replay\tmode\tmanual\tlearn-output\t%s\ttrigger\tAnalyze\n' "$learn_output"
            fi
            emit_replay_panel_rows
        } >"$panel_tmp"; then
            mv "$panel_tmp" "$panel_path"
            return 0
        fi
        return 1
    }
    refresh_readonly_panel_file() {
        if {
            panel_rows_for_request 0
            emit_cached_context_rows
            printf 'panel\tmodel-replay\tmode\tmanual\tlearn-output\t%s\ttrigger\tAnalyze\n' "$learn_output"
        } >"$panel_live_tmp"; then
            mv "$panel_live_tmp" "$panel_path"
            return 0
        fi
        return 1
    }
    refresh_context_file || {
        echo "无法生成 TiPE 监督窗口上下文" >&2
        exit 1
    }
    context_refresh_changes="${TIPE_LEARNING_PANEL_CONTEXT_REFRESH_CHANGES:-10}"
    if [[ ! "$context_refresh_changes" =~ ^[1-9][0-9]*$ ]]; then
        context_refresh_changes=10
    fi
    request_snapshot_signature() {
        local watched_path
        for watched_path in "$request_path" "$fallback_request_path"; do
            if [[ -n "$watched_path" && -e "$watched_path" ]]; then
                printf '%s\t' "$watched_path"
                stat -Lc '%d:%i:%s:%Y:%y' -- "$watched_path" 2>/dev/null || printf 'unreadable\n'
            else
                printf '%s\tmissing\n' "$watched_path"
            fi
        done
    }
    refresh_script=""
    if [[ "$replay_model" == 1 ]]; then
        refresh_script=$(mktemp)
        cleanup_paths+=("$refresh_script")
        raw_panel_args_for_request
        context_filter='$1 == "panel" && ($2 == "history" || $2 == "training" || $2 == "model-config" || ($2 == "learning" && $3 == "dictionary-history"))'
        {
            printf '%s\n' '#!/usr/bin/env bash'
            printf '%s\n' 'set -euo pipefail'
            printf 'panel_path=%q\n' "$panel_path"
            printf 'panel_tmp=%q\n' "$panel_tmp"
            printf 'panel_context_path=%q\n' "$panel_context_path"
            printf '%s\n' 'panel_context_tmp="${panel_context_path}.next.${BASHPID}"'
            printf 'helper=%q\n' "$self_path"
            printf '%s\n' 'rm -f "$panel_tmp" "$panel_context_tmp"'
            printf '%s\n' 'trap '\''rm -f "$panel_tmp" "$panel_context_tmp"'\'' EXIT'
            printf '"$helper"'
            for arg in "${raw_panel_args[@]}"; do
                printf ' %q' "$arg"
            done
            printf ' >"$panel_tmp"\n'
            printf 'awk -F %q %q "$panel_tmp" >"$panel_context_tmp"\n' $'\t' "$context_filter"
            printf '%s\n' 'mv "$panel_context_tmp" "$panel_context_path"'
            printf '%s\n' 'mv "$panel_tmp" "$panel_path"'
        } >"$refresh_script"
        chmod +x "$refresh_script"
        if [[ "$defer_replay" == 1 ]]; then
            if {
                panel_rows_for_request
                emit_cached_context_rows
                printf 'panel\tmodel-replay\tmode\tmanual\tlearn-output\t%s\ttrigger\tAnalyze\n' "$learn_output"
            } >"$panel_tmp"; then
                mv "$panel_tmp" "$panel_path"
            else
                echo "无法从请求生成学习面板数据：$request_path" >&2
                exit 1
            fi
        else
            "$refresh_script" || {
                echo "无法从请求生成学习面板数据：$request_path" >&2
                exit 1
            }
        fi
        panel_window_args+=(--refresh-command "$refresh_script")
        refresh_seconds="${TIPE_LEARNING_PANEL_REFRESH_SECONDS:-1}"
        initial_request_signature=$(request_snapshot_signature)
        (
            sleep_pid=""
            last_request_signature="$initial_request_signature"
            context_change_count=0
            stop_sleep() {
                if [[ -n "$sleep_pid" ]]; then
                    kill "$sleep_pid" >/dev/null 2>&1 || true
                    wait "$sleep_pid" >/dev/null 2>&1 || true
                fi
                exit 0
            }
            trap stop_sleep TERM INT
            while :; do
                sleep "$refresh_seconds" &
                sleep_pid=$!
                wait "$sleep_pid" || exit 0
                sleep_pid=""
                [[ -e "$panel_path" ]] || exit 0
                current_request_signature=$(request_snapshot_signature)
                if [[ "$current_request_signature" != "$last_request_signature" ]]; then
                    last_request_signature="$current_request_signature"
                    context_change_count=$((context_change_count + 1))
                    if [[ ! -r "$request_path" || $context_change_count -ge $context_refresh_changes ]]; then
                        refresh_context_file >/dev/null 2>&1 || true
                        context_change_count=0
                    fi
                    refresh_readonly_panel_file >/dev/null 2>&1 || true
                fi
            done
        ) &
        refresh_pid=$!
        cleanup_pids+=("$refresh_pid")
    else
        refresh_panel_file || {
            echo "无法从请求生成学习面板数据：$request_path" >&2
            exit 1
        }
        refresh_seconds="${TIPE_LEARNING_PANEL_REFRESH_SECONDS:-1}"
        initial_request_signature=$(request_snapshot_signature)
        (
            sleep_pid=""
            last_request_signature="$initial_request_signature"
            context_change_count=0
            stop_sleep() {
                if [[ -n "$sleep_pid" ]]; then
                    kill "$sleep_pid" >/dev/null 2>&1 || true
                    wait "$sleep_pid" >/dev/null 2>&1 || true
                fi
                exit 0
            }
            trap stop_sleep TERM INT
            while :; do
                sleep "$refresh_seconds" &
                sleep_pid=$!
                wait "$sleep_pid" || exit 0
                sleep_pid=""
                [[ -e "$panel_path" ]] || exit 0
                current_request_signature=$(request_snapshot_signature)
                if [[ "$current_request_signature" != "$last_request_signature" ]]; then
                    last_request_signature="$current_request_signature"
                    context_change_count=$((context_change_count + 1))
                    if [[ ! -r "$request_path" || $context_change_count -ge $context_refresh_changes ]]; then
                        refresh_context_file >/dev/null 2>&1 || true
                        context_change_count=0
                    fi
                    if {
                        panel_rows_for_request 0
                        emit_cached_context_rows
                    } >"$panel_live_tmp"; then
                        mv "$panel_live_tmp" "$panel_path"
                    fi
                fi
            done
        ) &
        refresh_pid=$!
        cleanup_pids+=("$refresh_pid")
    fi
    "${panel_window_args[@]}" "$panel_path"
    status=$?
    if [[ "${refresh_pid:-}" ]]; then
        kill "$refresh_pid" >/dev/null 2>&1 || true
        wait "$refresh_pid" >/dev/null 2>&1 || true
    fi
    exit "$status"
fi

panel_rows=$(
    panel_rows_for_request
    emit_dictionary_history_panel_rows
    emit_model_config_panel_rows
)
echo "TiPE 学习面板"
while IFS=$'\t' read -r prefix section key rest; do
    [[ "$prefix" == "panel" ]] || continue
    case "$section:$key" in
        state:preedit)
            printf 'state\tpreedit\t%s\n' "$rest"
            ;;
        state:application)
            printf 'state\tapplication\t%s\n' "$rest"
            ;;
        state:surrounding-before|state:surrounding-after)
            printf 'state\t%s\t%s\n' "$key" "$rest"
            ;;
        candidates:total)
            printf '候选\t%s\n' "$key${rest:+	$rest}"
            ;;
        candidates:first|candidates:selected|candidates:visible|candidates:numbered)
            printf '候选\t%s\t%s\n' "$key" "$rest"
            ;;
        supervision:recent-events)
            printf 'supervision\t%s\n' "$key${rest:+	$rest}"
            ;;
        supervision:model-input|supervision:mode|supervision:runtime-state|supervision:input-mode|supervision:event-trail|supervision:correction-trail|supervision:event-item|supervision:correction-event-item)
            printf 'supervision\t%s\t%s\n' "$key" "$rest"
            ;;
        segment-chain:*)
            printf 'segment-chain\t%s\t%s\n' "$key" "$rest"
            ;;
        learning:preferences)
            printf 'learning\t%s\n' "$key${rest:+	$rest}"
            ;;
        learning:dictionary-history|learning:selected-candidate-signal|learning:segment-chain-signal|learning:pending-segment-signal|learning:correction-signal|learning:status|learning:status-suggested-protocol|learning:status-awaiting-suffix|learning:status-signal-count|learning:evidence-status|learning:evidence-summary|learning:evidence-preference|learning:evidence-legacy-preference|learning:evidence-correction|learning:evidence-segment-chain|learning:evidence-effect)
            printf 'learning\t%s\t%s\n' "$key" "$rest"
            ;;
        model-config:*)
            printf 'model-config\t%s\t%s\n' "$key" "$rest"
            ;;
        top-preference:*|top-correction:*|top-correction-pattern:*)
            printf '%s\t%s\t%s\n' "$section" "$key" "$rest"
            ;;
        behavior:*)
            printf 'behavior\t%s\t%s\n' "$key" "$rest"
            ;;
        history:*)
            printf 'history\t%s\t%s\n' "$key" "$rest"
            ;;
    esac
done <<< "$panel_rows"

if [[ "$replay_model" == 1 ]]; then
    model_replay=$(helper_path tipe-model-replay) || {
        echo "找不到 tipe-model-replay 辅助程序" >&2
        exit 1
    }
    replay_stdout=$(mktemp)
    replay_stderr=$(mktemp)
    cleanup_paths+=("$replay_stdout" "$replay_stderr")
    replay_args_for_request
    if "$model_replay" "${replay_args[@]}" >"$replay_stdout" 2>"$replay_stderr"; then
        echo "模型回放"
        if [[ -s "$replay_stderr" ]]; then
            while IFS= read -r line || [[ -n "$line" ]]; do
                printf 'replay-check\t%s\n' "$line"
            done <"$replay_stderr"
        fi
        if [[ -s "$replay_stdout" ]]; then
            while IFS= read -r line || [[ -n "$line" ]]; do
                printf 'replay-output\t%s\n' "$line"
            done <"$replay_stdout"
        else
            printf 'replay-output\t(empty)\n'
        fi
    else
        status=$?
        while IFS= read -r line || [[ -n "$line" ]]; do
            printf 'replay-error\t%s\n' "$line" >&2
        done <"$replay_stderr"
        exit "$status"
    fi
fi
