#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export TIPE_TEST_FALLBACK_DICTIONARY="$ROOT/tests/fixtures/state-dictionary.tsv"
export TIPE_DOCTOR_SOURCE_HELPERS=1
export TIPE_DISABLE_LIBIME_LEARNING=1
source "$ROOT/scripts/installed-files.sh"
tmp_dir=$(mktemp -d)
doctor_default_data="$tmp_dir/doctor-default-data"
doctor_default_cache="$tmp_dir/doctor-default-cache"
doctor_default_runtime="$tmp_dir/doctor-default-runtime"
doctor_default_model_config="$tmp_dir/doctor-default-model-env"
mkdir -p "$doctor_default_data" "$doctor_default_cache" "$doctor_default_runtime/tipe"
doctor_fake_runtime_pids=()
cleanup() {
    local pid
    for pid in "${doctor_fake_runtime_pids[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${doctor_fake_runtime_pids[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$tmp_dir"
}
trap cleanup EXIT

fingerprint_root="$tmp_dir/source-fingerprint"
mkdir -p "$fingerprint_root"/{src,tests,tools,scripts,addon,data,third_party}
printf '%s\n' 'cmake_minimum_required(VERSION 3.20)' >"$fingerprint_root/CMakeLists.txt"
printf '%s\n' 'int value = 1;' >"$fingerprint_root/src/sample.cpp"
fingerprint_first=$(tipe_build_source_fingerprint "$fingerprint_root")
fingerprint_same=$(tipe_build_source_fingerprint "$fingerprint_root")
mkdir -p "$fingerprint_root/scripts/__pycache__"
printf '%s\n' 'generated' >"$fingerprint_root/scripts/__pycache__/sample.pyc"
fingerprint_ignored_cache=$(tipe_build_source_fingerprint "$fingerprint_root")
printf '%s\n' 'int changed = 2;' >>"$fingerprint_root/src/sample.cpp"
fingerprint_changed=$(tipe_build_source_fingerprint "$fingerprint_root")
if [[ ! "$fingerprint_first" =~ ^[0-9a-f]{64}$ || "$fingerprint_same" != "$fingerprint_first" ||
    "$fingerprint_ignored_cache" != "$fingerprint_first" || "$fingerprint_changed" == "$fingerprint_first" ]]; then
    echo "build source fingerprint should ignore generated Python cache and detect content changes" >&2
    exit 1
fi
tested_build_stamp="$ROOT/build/.tipe-build-tested"
if [[ -f "$tested_build_stamp" ]]; then
    tested_build_fingerprint=$(sed -n '1p' "$tested_build_stamp")
    if TIPE_BUILD_JOBS=0 "$ROOT/scripts/build.sh" >/dev/null 2>&1 ||
        TIPE_BUILD_NICE=20 "$ROOT/scripts/build.sh" >/dev/null 2>&1 ||
        [[ "$(sed -n '1p' "$tested_build_stamp")" != "$tested_build_fingerprint" ]]; then
        echo "invalid build performance overrides should fail without deleting the tested-source fingerprint" >&2
        exit 1
    fi
fi

start_doctor_runtime() {
    local model_command="$1"
    local model_config="$2"
    local model_mode="$3"
    local model_backend="$4"
    TIPE_MODEL_COMMAND="$model_command" TIPE_MODEL_CONFIG="$model_config" \
        TIPE_MODEL_MODE="$model_mode" TIPE_MODEL_BACKEND="$model_backend" \
        tail -f /dev/null >/dev/null 2>&1 &
    doctor_started_pid=$!
    doctor_fake_runtime_pids+=("$doctor_started_pid")
}
stop_doctor_runtime() {
    local pid="${1:-}"
    [[ -n "$pid" ]] || return 0
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
if [[ ! -r "$TIPE_TEST_FALLBACK_DICTIONARY" ]]; then
    echo "state-machine fallback fixture is missing" >&2
    exit 1
fi
if rg -q "TIPE_TEST_LEGACY_FALLBACK|houxuanchuangxianshidebushigithubzhegeyingwenershizhongwen" \
    "$ROOT/src/dictionary.cpp"; then
    echo "production dictionary source should not contain the historical feedback fallback or its old switch" >&2
    exit 1
fi
production_dictionary_probe=$(
    TIPE_TEST_FALLBACK_DICTIONARY= TIPE_USER_DICTIONARY="$tmp_dir/no-user-dictionary.tsv" \
        "$ROOT/build/tipe-state-probe" houxuanchuangxianshidebushigithubzhegeyingwenershizhongwen
)
if [[ "$production_dictionary_probe" == *'候选窗显示的不是github这个英文而是中文'* ]]; then
    echo "production dictionary should not expose historical feedback sentences as built-in candidates" >&2
    exit 1
fi
fixture_dictionary_probe=$("$ROOT/build/tipe-state-probe" houxuanchuangxianshidebushigithubzhegeyingwenershizhongwen)
if [[ "$fixture_dictionary_probe" != *'候选窗显示的不是github这个英文而是中文'* ]]; then
    echo "test fallback fixture should preserve historical state-machine behavior coverage" >&2
    exit 1
fi
production_basic_probe=$(
    TIPE_TEST_FALLBACK_DICTIONARY= TIPE_USER_DICTIONARY="$tmp_dir/no-user-dictionary.tsv" \
        "$ROOT/build/tipe-state-probe" nihao
)
if [[ "$production_basic_probe" != *$'candidate\t0\t你好'* ]]; then
    echo "production dictionary should keep basic Chinese composition without legacy feedback fallbacks" >&2
    exit 1
fi
uppercase_raw_probe=$("$ROOT/build/tipe-state-probe" nihao --key A)
if [[ "$uppercase_raw_probe" != *$'action\tkey\t1\tupdate'* ||
    "$uppercase_raw_probe" != *$'preedit\tnihaoA'* ||
    "$uppercase_raw_probe" != *$'candidate\t0\tnihaoA'* ||
    "$uppercase_raw_probe" != *$'candidate-meta\t0\tconsumed-prefix\t0\tsource\traw'* ||
    "$uppercase_raw_probe" == *$'commit\t'* ||
    "$uppercase_raw_probe" == *$'passthrough\tA'* ]]; then
    echo "uppercase letters should keep an active composition as case-preserving raw English" >&2
    exit 1
fi
if ! rg -q "history_correction|top_corrections" "$ROOT/README.md" "$ROOT/docs/architecture.md" "$ROOT/docs/debugging.md"; then
    echo "docs should describe history-derived correction supervision" >&2
    exit 1
fi
destroyed_handler=$(sed -n '/void Engine::onInputContextDestroyed/,/^}/p' "$ROOT/src/engine.cpp")
if [[ "$destroyed_handler" != *'clearSupervisionSnapshot(ic);'* ||
    "$destroyed_handler" != *'clearCandidateWindow();'* ]]; then
    echo "input-context destruction should clear only its owned live supervision and fallback candidate window state" >&2
    exit 1
fi
candidate_clear_handler=$(sed -n '/void Engine::clearCandidateWindow/,/^}/p' "$ROOT/src/engine.cpp")
candidate_key_handler=$(sed -n '/void Engine::keyEvent/,/^}/p' "$ROOT/src/engine.cpp")
if [[ "$candidate_clear_handler" != *'sendCandidateWindowMessage(clearMessage, false)'* ||
    "$candidate_clear_handler" == *'closeCandidateWindow();'* ||
    "$candidate_key_handler" == *'restartCandidateWindow'* ]]; then
    echo "fallback candidate window should hide on clear and persist across compositions" >&2
    exit 1
fi
engine_supervision_clear_block=$(sed -n '/void Engine::clearSupervisionSnapshot/,/^}/p' "$ROOT/src/engine.cpp")
if [[ "$engine_supervision_clear_block" != *'owner != supervisionSnapshotOwner_'* ||
    "$engine_supervision_clear_block" != *'supervision-clear ignored-non-owner'* ]]; then
    echo "a delayed clear from an old input context should not remove the current context's supervision snapshot" >&2
    exit 1
fi
tipeui_status_block=$(sed -n '/closeCandidateFallbackWindow(popup, "status-popup");/,/renderStatusPopupOrFallback(display, popup, auxUp, inputContext.scaleFactor());/p' "$ROOT/src/tipe_ui.cpp")
if [[ "$tipeui_status_block" != *'popup.awaitingFreshTextRect || !popup.lastTextRect'* ||
    "$tipeui_status_block" == *'popup.awaitingFreshTextRect || popup.candidateTextRectStale'* ||
    "$tipeui_status_block" != *'status-render-with-candidate-stale-rect'* ]]; then
    echo "TiPE status popup should not wait for a fresh candidate text rectangle when a status-only rect is already available" >&2
    exit 1
fi
tipeui_candidate_block=$(sed -n '/const auto fallbackRect =/,/defer-candidate-render/p' "$ROOT/src/tipe_ui.cpp")
if [[ "$tipeui_candidate_block" != *'popup.awaitingFreshTextRect || popup.candidateTextRectStale'* ]]; then
    echo "TiPE candidate popup should still wait for a fresh text rectangle after hide/focus changes" >&2
    exit 1
fi
tipeui_update_block=$(sed -n '/void update(fcitx::UserInterfaceComponent/,/^    }/p' "$ROOT/src/tipe_ui.cpp")
if [[ "$tipeui_update_block" != *'hidePopupForInputContext(*inputContext);'* ||
    "$tipeui_update_block" == *$'if (inputPanelEmpty(*inputContext)) {\n            hideAllPopups();'* ]]; then
    echo "an empty panel update should hide only the popup owned by that input context" >&2
    exit 1
fi
tipeui_context_switch_block=$(sed -n '/auto popupIter = popupByDisplay_.find(key);/,/auto \*inputMethod =/p' \
    "$ROOT/src/tipe_ui.cpp")
if [[ "$tipeui_context_switch_block" != *'lastInputContext.get() == &inputContext'* ||
    "$tipeui_context_switch_block" != *'input-context-changed-recreate'* ||
    "$tipeui_context_switch_block" != *'popupByDisplay_.erase(popupIter);'* ]]; then
    echo "a Wayland input-context switch should recreate the popup surface instead of accepting queued old rectangles" >&2
    exit 1
fi
if rg -q 'wl_display_roundtrip' "$ROOT/src/tipe_ui.cpp"; then
    echo "TiPE UI must not dispatch nested Wayland events while fcitx is flushing its UI update list" >&2
    exit 1
fi
if ! rg -q 'TrackableObjectReference<fcitx::InputContext> lastInputContext' "$ROOT/src/tipe_ui.cpp" ||
    ! rg -q 'InputContextDestroyed.*EventWatcherPhase::InputMethod' "$ROOT/src/tipe_ui.cpp" ||
    ! rg -q 'popup.*input-context-destroyed' "$ROOT/src/tipe_ui.cpp"; then
    echo "TiPE UI should weakly track and explicitly release destroyed input contexts" >&2
    exit 1
fi
if ! rg -q 'TrackableObjectReference<fcitx::InputContext> inputContext' "$ROOT/src/engine.h" ||
    ! rg -q 'const auto inputContext = ic->watch()' "$ROOT/src/engine.cpp"; then
    echo "TiPE engine should weakly track input contexts used by asynchronous jobs and delayed callbacks" >&2
    exit 1
fi
if ! rg -q 'wl_registry_add_listener' "$ROOT/src/tipe_ui.cpp" ||
    ! rg -q 'scheduleWaylandPopupRetry' "$ROOT/src/tipe_ui.cpp"; then
    echo "TiPE UI should discover Wayland globals asynchronously and retry popup creation" >&2
    exit 1
fi
engine_update_panel_block=$(sed -n '/void Engine::updatePanel/,/void Engine::maybeContinuousRerank/p' "$ROOT/src/engine.cpp")
if [[ "$engine_update_panel_block" != *'tipeUIHandlesInputContext(instance_, *ic)'* ||
    "$engine_update_panel_block" == *'const bool useTipeUI = tipeUIActive(instance_);'* ]]; then
    echo "TiPE engine should use the tipeui popup path only for input contexts that tipeui can actually render" >&2
    exit 1
fi
engine_status_block=$(sed -n '/void Engine::showInputModeStatus/,/void Engine::clearActivationStatusState/p' "$ROOT/src/engine.cpp")
if [[ "$engine_status_block" != *'const bool useTipeUI = tipeUIHandlesInputContext(instance_, *ic);'* ||
    "$engine_status_block" != *$'if (useTipeUI) {\n        panel.setAuxUp(fcitx::Text(status));'* ||
    "$engine_status_block" != *$'if (!useTipeUI) {\n        showStatusWindow(*ic, status);'* ]]; then
    echo "TiPE engine should publish status through exactly one frontend-specific UI path" >&2
    exit 1
fi
focus_transfer_block=$(sed -n '/void Engine::scheduleFocusTransferSuppression/,/void Engine::clearActivationStatusState/p' "$ROOT/src/engine.cpp")
if [[ "$engine_status_block" == *'showStatusWindow(*ic, status, true)'* ||
    "$focus_transfer_block" == *'showStatusWindow('* ||
    "$focus_transfer_block" != *'focusTransferTimer_.reset();'* ]]; then
    echo "focus transfer suppression should not create a second detached input-mode indicator" >&2
    exit 1
fi
if ! grep -Fxq 'Icon=tipe' "$ROOT/addon/tipe-inputmethod.conf" ||
    ! rg -q '\.setIcon\("tipe"\)' "$ROOT/src/engine.cpp"; then
    echo "TiPE input-method metadata and runtime registration should use the geometric T icon" >&2
    exit 1
fi
"$ROOT/build/tipe-state-test"
probe_output=$("$ROOT/build/tipe-state-probe" nihao)
if [[ "$probe_output" != *$'candidate\t0\t你好'* || "$probe_output" != *$'visible\t1\t0\t你好'* ||
    "$probe_output" != *$'visible\t2\t1\t你'* || "$probe_output" != *$'events\tletters=5'* ]]; then
    echo "state probe should show nihao first candidate" >&2
    exit 1
fi
probe_visible_expanded_output=$("$ROOT/build/tipe-state-probe" nihao --move Down --move Down)
probe_visible_expanded_candidate=$(awk -F $'\t' '$1 == "candidate" && $2 == "6" { print $3; exit }' \
    <<<"$probe_visible_expanded_output")
probe_visible_expanded_slot=$(awk -F $'\t' '$1 == "visible" && $2 == "1" { print $3 "\t" $4; exit }' \
    <<<"$probe_visible_expanded_output")
if [[ "$probe_visible_expanded_output" != *$'expanded\t1'* ||
    "$probe_visible_expanded_output" != *$'selected\t6'* ||
    -z "$probe_visible_expanded_candidate" ||
    "$probe_visible_expanded_slot" != $'6\t'"$probe_visible_expanded_candidate" ]]; then
    echo "state probe should show expanded-row visible digit mapping" >&2
    exit 1
fi
probe_navigation_output=$("$ROOT/build/tipe-state-probe" nihao --move End --move Home --move PageDown --move ShiftTab)
probe_navigation_candidate=$(awk -F $'\t' '$1 == "candidate" && $2 == "5" { print $3; exit }' \
    <<<"$probe_navigation_output")
probe_navigation_slot=$(awk -F $'\t' '$1 == "visible" && $2 == "6" { print $3 "\t" $4; exit }' \
    <<<"$probe_navigation_output")
if [[ "$probe_navigation_output" != *$'expanded\t1'* ||
    "$probe_navigation_output" != *$'selected\t5'* ||
    "$probe_navigation_output" != *$'cursor_moves=4'* ||
    -z "$probe_navigation_candidate" ||
    "$probe_navigation_slot" != $'5\t'"$probe_navigation_candidate" ]]; then
    echo "state probe should expose Home/End/PageDown/ShiftTab navigation" >&2
    exit 1
fi
probe_key_alias_output=$("$ROOT/build/tipe-state-probe" nihao --move KP_Down --move KP_Down --key ISO_Left_Tab)
probe_key_alias_candidate=$(awk -F $'\t' '$1 == "candidate" && $2 == "5" { print $3; exit }' \
    <<<"$probe_key_alias_output")
probe_key_alias_slot=$(awk -F $'\t' '$1 == "visible" && $2 == "6" { print $3 "\t" $4; exit }' \
    <<<"$probe_key_alias_output")
if [[ "$probe_key_alias_output" != *$'action\tkey\t1\tupdate'* ||
    "$probe_key_alias_output" != *$'expanded\t1'* ||
    "$probe_key_alias_output" != *$'selected\t5'* ||
    -z "$probe_key_alias_candidate" ||
    "$probe_key_alias_slot" != $'5\t'"$probe_key_alias_candidate" ]]; then
    echo "state probe should mirror engine key aliases for keypad arrows and ISO_Left_Tab" >&2
    exit 1
fi
probe_shift_tab_alias_output=$("$ROOT/build/tipe-state-probe" nihao --move KP_Down --move KP_Down --key Shift+Tab)
probe_shift_tab_alias_candidate=$(awk -F $'\t' '$1 == "candidate" && $2 == "5" { print $3; exit }' \
    <<<"$probe_shift_tab_alias_output")
probe_shift_tab_alias_slot=$(awk -F $'\t' '$1 == "visible" && $2 == "6" { print $3 "\t" $4; exit }' \
    <<<"$probe_shift_tab_alias_output")
if [[ "$probe_shift_tab_alias_output" != *$'action\tkey\t1\tupdate'* ||
    "$probe_shift_tab_alias_output" != *$'expanded\t1'* ||
    "$probe_shift_tab_alias_output" != *$'selected\t5'* ||
    -z "$probe_shift_tab_alias_candidate" ||
    "$probe_shift_tab_alias_slot" != $'5\t'"$probe_shift_tab_alias_candidate" ]]; then
    echo "state probe should mirror engine key aliases for keypad arrows and Shift+Tab" >&2
    exit 1
fi
probe_collapsed_left_boundary_output=$(printf '%s\n' \
    'type nihao' \
    'expect-preedit-cursor 5' \
    'key Right' \
    'expect-selected 1' \
    'expect-preedit-cursor 5' \
    'key Right' \
    'expect-selected 2' \
    'expect-preedit-cursor 5' \
    'key Left' \
    'expect-selected 1' \
    'expect-preedit-cursor 5' \
    'key Left' \
    'expect-selected 0' \
    'expect-preedit-cursor 5' \
    'key Left' \
    'expect-selected 0' \
    'expect-preedit-cursor 4' |
    "$ROOT/build/tipe-state-probe" '' --script -)
if [[ "$probe_collapsed_left_boundary_output" != *$'preedit_cursor\t4'* ||
    "$probe_collapsed_left_boundary_output" != *$'selected\t0'* ||
    "$probe_collapsed_left_boundary_output" != *$'cursor_moves=5'* ]]; then
    echo "collapsed Left should return candidate selection to zero before editing the pinyin cursor" >&2
    exit 1
fi
probe_empty_key_events_output=$(printf '%s\n' 'key Space' 'key BackSpace' 'key Return' 'key Escape' 'key Delete' 'key Down' 'key Tab' |
    "$ROOT/build/tipe-state-probe" '' --script - --events)
if [[ "$probe_empty_key_events_output" != *$'action\tkey\t0\tnone'* ||
    "$probe_empty_key_events_output" != *$'event\t0\tspace\t'* ||
    "$probe_empty_key_events_output" != *$'event\t1\tbackspace\t'* ||
    "$probe_empty_key_events_output" != *$'event\t2\tenter\t'* ||
    "$probe_empty_key_events_output" != *$'event\t3\tescape\t'* ||
    "$probe_empty_key_events_output" != *$'event\t4\tdelete\t'* ||
    "$probe_empty_key_events_output" != *$'event\t5\tcursor-move\tDown'* ||
    "$probe_empty_key_events_output" != *$'event\t6\tcursor-move\tTab'* ]]; then
    echo "state probe should record empty-state pass-through keys as structured model events" >&2
    exit 1
fi
probe_pass_through_request=$("$ROOT/build/tipe-state-probe" '' --observe start --observe Left --space --request)
if [[ "$probe_pass_through_request" != $'protocol\t1'* ||
    "$probe_pass_through_request" == *$'action\t'* ||
    "$probe_pass_through_request" != *$'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t3\tcorrection_events\t3'* ||
    "$probe_pass_through_request" != *$'events\tobserved:start\tobserved:Left\tspace:'* ||
    "$probe_pass_through_request" != *$'event_counts\tobserved:2\tspace:1'* ]]; then
    echo "state probe --request should emit a clean pass-through supervision model request" >&2
    exit 1
fi
probe_event_assertion_output=$(printf '%s\n' \
    'key Space' \
    'key Delete' \
    'key BackSpace' \
    'key Down' \
    'key Tab' \
    'expect-event 0 space' \
    'expect-event 1 delete' \
    'expect-event 2 backspace' \
    'expect-event 3 cursor-move Down' \
    'expect-event 4 cursor-move Tab' |
    "$ROOT/build/tipe-state-probe" '' --script -)
if [[ "$probe_event_assertion_output" != *$'cursor_moves=2'* ]]; then
    echo "state probe script should assert ordered key events" >&2
    exit 1
fi
probe_partial_output=$("$ROOT/build/tipe-state-probe" shunxuyoudianwenti --select 顺序)
if [[ "$probe_partial_output" != *$'commit\t顺序'* || "$probe_partial_output" != *$'preedit\tyoudianwenti'* ||
    "$probe_partial_output" != *$'candidate\t0\t有点问题'* ]]; then
    echo "state probe should show partial commit remainder candidates" >&2
    exit 1
fi
probe_generated_partial_output=$("$ROOT/build/tipe-state-probe" changjuxianxuanqianbanduanhouhoumiandepinyinbubaoliu --select 长句先选前半段后)
if [[ "$probe_generated_partial_output" != *$'commit\t长句先选前半段后'* ||
    "$probe_generated_partial_output" != *$'preedit\thoumiandepinyinbubaoliu'* ||
    "$probe_generated_partial_output" != *$'candidate\t0\t后面的拼音不保留'* ]]; then
    echo "state probe should preserve remaining pinyin after generated long-sentence prefix commit" >&2
    exit 1
fi
probe_restore_partial_output=$(printf '%s\n' \
    'select 好的我看一下' \
    'expect-preedit haiyoumeiyu' \
    'expect-model-row pending_segment' \
    'delete' \
    'expect-preedit haodewokanyixiahaiyoumeiyu' \
    'expect-candidate 0 好的我看一下还有美誉' \
    'expect-no-context 好的我看一下' \
    'expect-no-model-row pending_segment' |
    "$ROOT/build/tipe-state-probe" haodewokanyixiahaiyoumeiyu --script -)
if [[ "$probe_restore_partial_output" != *$'action\tdelete\t1\tupdate'* ||
    "$probe_restore_partial_output" != *$'preedit\thaodewokanyixiahaiyoumeiyu'* ||
    "$probe_restore_partial_output" != *$'candidate\t0\t好的我看一下还有美誉'* ]]; then
    echo "state probe should restore full pinyin and clear pending supervision after undoing a prefix commit" >&2
    exit 1
fi
probe_typo_prefix_output=$("$ROOT/build/tipe-state-probe" haodewokanyxiahaiyoumeiyu --select 好的我看一下)
if [[ "$probe_typo_prefix_output" != *$'commit\t好的我看一下'* ||
    "$probe_typo_prefix_output" != *$'preedit\thaiyoumeiyu'* ||
    "$probe_typo_prefix_output" != *$'candidate\t0\t还有美誉'* ]]; then
    echo "state probe should preserve remaining pinyin after known typo prefix commit" >&2
    exit 1
fi
probe_common_sentence_output=$("$ROOT/build/tipe-state-probe" nihaowoxiangwenyixia --select 你好)
if [[ "$probe_common_sentence_output" != *$'commit\t你好'* ||
    "$probe_common_sentence_output" != *$'preedit\twoxiangwenyixia'* ||
    "$probe_common_sentence_output" != *$'candidate\t0\t我想问一下'* ]]; then
    echo "state probe should preserve and rank remaining common sentence pinyin after prefix commit" >&2
    exit 1
fi
probe_common_remainder_output=$("$ROOT/build/tipe-state-probe" woxiangwenyixia --select 我想)
if [[ "$probe_common_remainder_output" != *$'commit\t我想'* ||
    "$probe_common_remainder_output" != *$'preedit\twenyixia'* ||
    "$probe_common_remainder_output" != *$'candidate\t0\t问一下'* ]]; then
    echo "state probe should rank wenyixia as 问一下 after prefix commit" >&2
    exit 1
fi
probe_script="$tmp_dir/probe-actions.txt"
printf '%s\n' \
    '# Long composition can be committed one prefix candidate at a time.' \
    'type nihao' \
    'expect-candidate 0 你好' \
    'select 你' \
    'expect-preedit hao' \
    'expect-candidate 0 好' \
    'select 好' \
    'expect-context 0 你' \
    'expect-context 1 好' >"$probe_script"
probe_script_output=$("$ROOT/build/tipe-state-probe" "" --script "$probe_script")
if [[ "$probe_script_output" != *$'commit\t你'* || "$probe_script_output" != *$'commit\t好'* ||
    "$probe_script_output" != *$'context\t0\t你'* || "$probe_script_output" != *$'context\t1\t好'* ||
    "$probe_script_output" != *$'preedit\t'* || "$probe_script_output" != *$'candidate_selections=2'* ]]; then
    echo "state probe script should replay multi-step partial candidate selection" >&2
    exit 1
fi
printf '%s\n' 'type nihao' 'expect-candidate 0 错误候选' >"$probe_script"
if "$ROOT/build/tipe-state-probe" "" --script "$probe_script" >/dev/null 2>&1; then
    echo "state probe script assertions should fail when candidate order is unexpected" >&2
    exit 1
fi
probe_stdin_script_output=$(printf '%s\n' 'type nihao' 'expect-candidate 0 你好' 'select 你' 'expect-preedit hao' |
    "$ROOT/build/tipe-state-probe" "" --script -)
if [[ "$probe_stdin_script_output" != *$'commit\t你'* || "$probe_stdin_script_output" != *$'preedit\thao'* ]]; then
    echo "state probe should read action scripts from stdin" >&2
    exit 1
fi
probe_key_script_output=$(printf '%s\n' \
    'key n' \
    'key i' \
    'key h' \
    'key a' \
    'key o' \
    'expect-candidate 0 你好' \
    'key 2' \
    'expect-preedit hao' |
    "$ROOT/build/tipe-state-probe" "" --script -)
if [[ "$probe_key_script_output" != *$'action\tkey\t1\tupdate'* ||
    "$probe_key_script_output" != *$'action\tkey\t1\tcommit'* ||
    "$probe_key_script_output" != *$'commit\t你'* ||
    "$probe_key_script_output" != *$'preedit\thao'* ]]; then
    echo "state probe should replay engine-style key scripts from an empty preedit" >&2
    exit 1
fi
probe_partial_rewrite_script_output=$(printf '%s\n' \
    'type nhao' \
    'backspace' \
    'backspace' \
    'backspace' \
    'expect-preedit n' \
    'type ihao' \
    'select 你好' \
    'type nhao' \
    'backspace' \
    'backspace' \
    'backspace' \
    'expect-preedit n' \
    'type ihao' \
    'select 你好' \
    'type nhao' \
    'expect-candidate 0 你好' |
    "$ROOT/build/tipe-state-probe" "" --script -)
if [[ "$probe_partial_rewrite_script_output" != *$'commit\t你好'* ||
    "$probe_partial_rewrite_script_output" != *$'preedit\tnhao'* ||
    "$probe_partial_rewrite_script_output" != *$'candidate\t0\t你好'* ]]; then
    echo "state probe should learn partial-rewrite missing-letter corrections" >&2
    exit 1
fi
probe_cursor_insert_correction_output=$(printf '%s\n' \
    'type nhao' \
    'move Left' \
    'move Left' \
    'move Left' \
    'type i' \
    'select 你好' \
    'type nhao' \
    'move Left' \
    'move Left' \
    'move Left' \
    'type i' \
    'select 你好' \
    'type nhao' \
    'expect-candidate 0 你好' |
    "$ROOT/build/tipe-state-probe" "" --preferences "$tmp_dir/cursor-insert-preferences.tsv" --script -)
if [[ "$probe_cursor_insert_correction_output" != *$'commit\t你好'* ||
    "$probe_cursor_insert_correction_output" != *$'preedit\tnhao'* ||
    "$probe_cursor_insert_correction_output" != *$'candidate\t0\t你好'* ]]; then
    echo "state probe should learn cursor-insert missing-letter corrections" >&2
    exit 1
fi
probe_delete_correction_output=$(printf '%s\n' \
    'type niyhao' \
    'move Left' \
    'move Left' \
    'move Left' \
    'move Left' \
    'delete' \
    'select 你好' \
    'type niyhao' \
    'move Left' \
    'move Left' \
    'move Left' \
    'move Left' \
    'delete' \
    'select 你好' \
    'type niyhao' \
    'expect-candidate 0 你好' |
    "$ROOT/build/tipe-state-probe" "" --preferences "$tmp_dir/delete-correction-preferences.tsv" --script -)
if [[ "$probe_delete_correction_output" != *$'commit\t你好'* ||
    "$probe_delete_correction_output" != *$'preedit\tniyhao'* ||
    "$probe_delete_correction_output" != *$'candidate\t0\t你好'* ]]; then
    echo "state probe should learn delete-key extra-letter corrections" >&2
    exit 1
fi
edit_correction_model_preferences="$tmp_dir/edit-correction-model-preferences.tsv"
printf '%s\n' \
    'type nhao' \
    'move Left' \
    'move Left' \
    'move Left' \
    'type i' \
    'select 你好' \
    'type niyhao' \
    'move Left' \
    'move Left' \
    'move Left' \
    'move Left' \
    'delete' \
    'select 你好' \
    'type nhao' \
    'move Left' \
    'move Left' \
    'move Left' \
    'type i' \
    'select 你好' \
    'type niyhao' \
    'move Left' \
    'move Left' \
    'move Left' \
    'move Left' \
    'delete' \
    'select 你好' |
    "$ROOT/build/tipe-state-probe" "" --preferences "$edit_correction_model_preferences" --script - >/dev/null
edit_correction_model="$tmp_dir/edit-correction-model.sh"
cat >"$edit_correction_model" <<'MODEL'
#!/usr/bin/env bash
set -euo pipefail
input=$(cat)
grep -q $'^correction\tnhao\tnihao\t' <<< "$input"
grep -q $'^correction\tniyhao\tnihao\t' <<< "$input"
printf '%s\n' $'candidate\t你好'
MODEL
chmod +x "$edit_correction_model"
probe_edit_correction_model_output=$(
    TIPE_MODEL_COMMAND="$edit_correction_model" "$ROOT/build/tipe-state-probe" nhao --preferences "$edit_correction_model_preferences" --rerank
)
if [[ "$probe_edit_correction_model_output" != *$'action\trerank\t1\tupdate'* ||
    "$probe_edit_correction_model_output" != *$'candidate\t0\t你好'* ]]; then
    echo "state probe model request should include cursor-edit and delete learned correction rows" >&2
    exit 1
fi
probe_middle_omission_script_output=$(printf '%s\n' \
    'type iho' \
    'backspace' \
    'backspace' \
    'backspace' \
    'type nihao' \
    'select 你好' \
    'type iho' \
    'backspace' \
    'backspace' \
    'backspace' \
    'type nihao' \
    'select 你好' \
    'type iho' \
    'expect-candidate 0 你好' |
    "$ROOT/build/tipe-state-probe" "" --script -)
if [[ "$probe_middle_omission_script_output" != *$'commit\t你好'* ||
    "$probe_middle_omission_script_output" != *$'preedit\tiho'* ||
    "$probe_middle_omission_script_output" != *$'candidate\t0\t你好'* ]]; then
    echo "state probe should learn full-delete middle-letter omission corrections" >&2
    exit 1
fi
probe_enter_correction_script_output=$(printf '%s\n' \
    'type ihao' \
    'backspace' \
    'backspace' \
    'backspace' \
    'backspace' \
    'type nihao' \
    'enter' \
    'type ihao' \
    'backspace' \
    'backspace' \
    'backspace' \
    'backspace' \
    'type nihao' \
    'enter' \
    'type ihao' \
    'expect-candidate 0 你好' |
    "$ROOT/build/tipe-state-probe" "" --preferences "$tmp_dir/enter-correction-preferences.tsv" --script -)
if [[ "$probe_enter_correction_script_output" != *$'commit\tnihao'* ||
    "$probe_enter_correction_script_output" != *$'preedit\tihao'* ||
    "$probe_enter_correction_script_output" != *$'candidate\t0\t你好'* ]]; then
    echo "state probe should learn typo corrections from raw Enter commits" >&2
    exit 1
fi
example_probe_script_output=$("$ROOT/build/tipe-state-probe" "" --script "$ROOT/examples/probe-actions.txt")
if [[ "$example_probe_script_output" != *$'commit\t你'* || "$example_probe_script_output" != *$'commit\t好'* ||
    "$example_probe_script_output" != *$'commit\t光标跟随'* ||
    "$example_probe_script_output" != *$'commit\t失败'* ||
    "$example_probe_script_output" != *$'commit\t打github的时候'* ||
    "$example_probe_script_output" != *$'context\t4\t打github的时候'* ||
    "$example_probe_script_output" != *$'candidate_selections=1'* ]]; then
    echo "example probe script should replay checked multi-step actions" >&2
    exit 1
fi
probe_space_output=$("$ROOT/build/tipe-state-probe" nihao --space)
if [[ "$probe_space_output" != *$'action\tspace\t1\tcommit'* || "$probe_space_output" != *$'commit\t你好'* ||
    "$probe_space_output" != *$'preedit\t'* ]]; then
    echo "state probe should expose space commit behavior" >&2
    exit 1
fi
probe_enter_output=$("$ROOT/build/tipe-state-probe" zhongguo --enter)
if [[ "$probe_enter_output" != *$'action\tenter\t1\tcommit'* || "$probe_enter_output" != *$'commit\tzhongguo'* ]]; then
    echo "state probe should expose raw enter commit behavior" >&2
    exit 1
fi
probe_key_enter_output=$("$ROOT/build/tipe-state-probe" zhongguo --key KP_Enter)
if [[ "$probe_key_enter_output" != *$'action\tkey\t1\tcommit'* || "$probe_key_enter_output" != *$'commit\tzhongguo'* ]]; then
    echo "state probe should expose keypad enter through engine-style key replay" >&2
    exit 1
fi
probe_terminal_english_rerank_output=$("$ROOT/build/tipe-state-probe" started --application Alacritty --rerank)
if [[ "$probe_terminal_english_rerank_output" == *$'candidate\t0\tstarted'* ]]; then
    echo "state probe should not promote raw English identifiers just because the app is terminal/code" >&2
    exit 1
fi
probe_unlearned_raw_offer_output=$("$ROOT/build/tipe-state-probe" start)
if [[ "$probe_unlearned_raw_offer_output" != *$'visible\t2\t1\tstart'* ||
    "$probe_unlearned_raw_offer_output" != *$'candidate\t1\tstart'* ||
    "$probe_unlearned_raw_offer_output" != *$'candidate-meta\t1\tconsumed-prefix\t0\tsource\traw-offer'* ]]; then
    echo "state probe should expose an unlearned English-like raw candidate without promoting it first" >&2
    exit 1
fi
probe_generic_raw_offer_output=$("$ROOT/build/tipe-state-probe" goal)
if [[ "$probe_generic_raw_offer_output" != *$'candidate\t1\tgoal'* ||
    "$probe_generic_raw_offer_output" != *$'candidate-meta\t1\tconsumed-prefix\t0\tsource\traw-offer'* ]]; then
    echo "state probe should expose generic English-like terminal shapes as an unlearned raw candidate" >&2
    exit 1
fi
probe_normal_pinyin_raw_offer_guard=$("$ROOT/build/tipe-state-probe" shuruf)
if [[ "$probe_normal_pinyin_raw_offer_guard" == *$'\tcandidate\tshuruf'* ||
    "$probe_normal_pinyin_raw_offer_guard" == *$'source\traw-offer'* ]]; then
    echo "state probe should not expose unfinished normal pinyin as an unlearned raw candidate" >&2
    exit 1
fi
probe_learned_raw_offer_output=$(printf '%s\n' \
    'type start' \
    'select start' \
    'type start' \
    'expect-candidate 0 start' |
    "$ROOT/build/tipe-state-probe" "" --preferences "$tmp_dir/learned-raw-offer.tsv" --script -)
if [[ "$probe_learned_raw_offer_output" != *$'commit\tstart'* ||
    "$probe_learned_raw_offer_output" != *$'candidate\t0\tstart'* ]]; then
    echo "state probe should learn a visible raw-offer candidate after one explicit raw selection" >&2
    exit 1
fi
learned_raw_identifier_preferences="$tmp_dir/learned-raw-identifier.tsv"
probe_learned_raw_identifier_output=$(printf '%s\n' \
    'type started' \
    'enter' \
    'type started' \
    'enter' \
    'type started' \
    'expect-candidate 0 started' \
    'key 1' |
    "$ROOT/build/tipe-state-probe" "" --preferences "$learned_raw_identifier_preferences" --script -)
if [[ "$probe_learned_raw_identifier_output" != *$'preedit\tstarted1'* ||
    "$probe_learned_raw_identifier_output" != *$'candidate\t0\tstarted1'* ||
    "$probe_learned_raw_identifier_output" != *$'digits=1'* ]]; then
    echo "state probe should learn repeated raw identifiers and extend them with digit keys" >&2
    exit 1
fi
probe_plain_english_rerank_output=$("$ROOT/build/tipe-state-probe" started --rerank)
if [[ "$probe_plain_english_rerank_output" == *$'candidate\t0\tstarted'* ]]; then
    echo "state probe should not promote raw English identifiers without app context" >&2
    exit 1
fi
probe_surrounding_english_rerank_output=$(
    "$ROOT/build/tipe-state-probe" started --surrounding-before 'const taskStatus = ' --rerank
)
if [[ "$probe_surrounding_english_rerank_output" == *$'candidate\t0\tstarted'* ]]; then
    echo "state probe should not promote raw English identifiers just because surrounding text looks code-like" >&2
    exit 1
fi
probe_surrounding_chinese_rerank_output=$(
    "$ROOT/build/tipe-state-probe" started --surrounding-before '刚才说' --rerank
)
if [[ "$probe_surrounding_chinese_rerank_output" == *$'candidate\t0\tstarted'* ]]; then
    echo "state probe should not promote raw English identifiers in ordinary Chinese surrounding text" >&2
    exit 1
fi
probe_terminal_pinyin_rerank_output=$("$ROOT/build/tipe-state-probe" shurufa --application Alacritty --rerank)
if [[ "$probe_terminal_pinyin_rerank_output" == *$'candidate\t0\tshurufa'* ]]; then
    echo "state probe should not promote normal pinyin as raw English in terminal/code rerank" >&2
    exit 1
fi
probe_surrounding_pinyin_rerank_output=$(
    "$ROOT/build/tipe-state-probe" shurufa --surrounding-before 'const value = ' --rerank
)
if [[ "$probe_surrounding_pinyin_rerank_output" == *$'candidate\t0\tshurufa'* ]]; then
    echo "state probe should not promote normal pinyin as raw English in code-like surrounding text" >&2
    exit 1
fi
probe_external_terminal_english_rerank_output=$(
    TIPE_MODEL_COMMAND="$ROOT/scripts/model-adapter.sh" "$ROOT/build/tipe-state-probe" started --application Alacritty --rerank
)
if [[ "$probe_external_terminal_english_rerank_output" == *$'candidate\t0\tstarted'* ]]; then
    echo "external heuristic adapter should not add likely raw English identifiers from app context alone" >&2
    exit 1
fi
probe_external_terminal_pinyin_rerank_output=$(
    TIPE_MODEL_COMMAND="$ROOT/scripts/model-adapter.sh" "$ROOT/build/tipe-state-probe" shurufa --application Alacritty --rerank
)
if [[ "$probe_external_terminal_pinyin_rerank_output" == *$'candidate\t0\tshurufa'* ]]; then
    echo "external model adapter should not add normal pinyin as raw English" >&2
    exit 1
fi
probe_external_surrounding_english_rerank_output=$(
    TIPE_MODEL_COMMAND="$ROOT/scripts/model-adapter.sh" "$ROOT/build/tipe-state-probe" started --surrounding-before 'const taskStatus = ' --rerank
)
if [[ "$probe_external_surrounding_english_rerank_output" == *$'candidate\t0\tstarted'* ]]; then
    echo "external heuristic adapter should not promote likely raw English identifiers from surrounding text alone" >&2
    exit 1
fi
probe_external_surrounding_pinyin_rerank_output=$(
    TIPE_MODEL_COMMAND="$ROOT/scripts/model-adapter.sh" "$ROOT/build/tipe-state-probe" shurufa --surrounding-before 'const value = ' --rerank
)
if [[ "$probe_external_surrounding_pinyin_rerank_output" == *$'candidate\t0\tshurufa'* ]]; then
    echo "external model adapter should not promote normal pinyin as raw English in code-like surrounding text" >&2
    exit 1
fi
probe_digit_output=$("$ROOT/build/tipe-state-probe" nihao --digit 2)
if [[ "$probe_digit_output" != *$'action\tdigit\t1\tcommit'* || "$probe_digit_output" != *$'commit\t你'* ]]; then
    echo "state probe should expose collapsed digit selection behavior" >&2
    exit 1
fi
probe_digit_alnum_output=$("$ROOT/build/tipe-state-probe" qwen --digit 2)
if [[ "$probe_digit_alnum_output" != *$'action\tdigit\t1\tupdate'* ||
    "$probe_digit_alnum_output" != *$'preedit\tqwen2'* ||
    "$probe_digit_alnum_output" != *$'candidate\t0\tqwen2'* ]]; then
    echo "state probe --digit should mirror real digit-key input for alphanumeric tokens" >&2
    exit 1
fi
probe_preferences_path="$tmp_dir/probe-preferences.tsv"
"$ROOT/build/tipe-state-probe" nihao --preferences "$probe_preferences_path" --select 你号 >/dev/null
probe_preferences_output=$("$ROOT/build/tipe-state-probe" nihao --preferences "$probe_preferences_path")
if [[ "$probe_preferences_output" != *$'candidate\t0\t你号'* ]]; then
    echo "state probe should reuse an explicit preference file across processes" >&2
    exit 1
fi
probe_reset_script="$tmp_dir/probe-reset-script.txt"
printf '%s\n' \
    'type jixuzuo' \
    'select 继续' \
    'reset' \
    'type jixuzuo' \
    'expect-candidate 0 继续做' \
    'expect-has-candidate 继续' >"$probe_reset_script"
"$ROOT/build/tipe-state-probe" "" --preferences "$tmp_dir/probe-reset-preferences.tsv" --script "$probe_reset_script" >/dev/null
if "$ROOT/build/tipe-state-probe" nihao --user-data --preferences "$probe_preferences_path" >/dev/null 2>&1; then
    echo "state probe should reject conflicting --user-data and --preferences options" >&2
    exit 1
fi
for english_token in vscode cursor python wayland chatgpt javascript niri ollama waybar hyprland systemd gnome dbus git npm node rust cargo cmake build cmakebuild; do
    probe_english_output=$("$ROOT/build/tipe-state-probe" "$english_token")
    probe_english_commit_output=$("$ROOT/build/tipe-state-probe" "$english_token" --space)
    if [[ "$probe_english_output" != *$'candidate\t0\t'"$english_token"* ||
        "$probe_english_commit_output" != *$'commit\t'"$english_token"* ||
        "$probe_english_commit_output" != *$'context\t0\t'"$english_token"* ]]; then
        echo "state probe should prefer raw english developer token: $english_token" >&2
        exit 1
    fi
done
probe_alnum_key_output=$("$ROOT/build/tipe-state-probe" qwen --key 2)
if [[ "$probe_alnum_key_output" != *$'preedit\tqwen2'* ||
    "$probe_alnum_key_output" != *$'candidate\t0\tqwen2'* ||
    "$probe_alnum_key_output" != *$'digits=1'* ]]; then
    echo "state probe should let known alphanumeric English tokens consume digit keys" >&2
    exit 1
fi
probe_known_raw_digit_output=$("$ROOT/build/tipe-state-probe" react --key 1)
if [[ "$probe_known_raw_digit_output" != *$'preedit\treact1'* ||
    "$probe_known_raw_digit_output" != *$'candidate\t0\treact1'* ||
    "$probe_known_raw_digit_output" != *$'digits=1'* ]]; then
    echo "state probe should extend known raw English tokens with digit keys" >&2
    exit 1
fi
probe_known_raw_symbol_output=$("$ROOT/build/tipe-state-probe" react --key minus --key a --key underscore --key period --key slash --events)
if [[ "$probe_known_raw_symbol_output" != *$'preedit\treact-a_./'* ||
    "$probe_known_raw_symbol_output" != *$'candidate\t0\treact-a_./'* ||
    "$probe_known_raw_symbol_output" != *$'symbols=4'* ||
    "$probe_known_raw_symbol_output" != *$'event\t5\tsymbol\t-'* ||
    "$probe_known_raw_symbol_output" != *$'event\t7\tsymbol\t_'* ]]; then
    echo "state probe should extend raw English tokens with token symbols" >&2
    exit 1
fi
probe_pinyin_symbol_output=$("$ROOT/build/tipe-state-probe" nihao --key minus)
if [[ "$probe_pinyin_symbol_output" != *$'commit\t你好'* ||
    "$probe_pinyin_symbol_output" != *$'preedit\t'* ]]; then
    echo "state probe should keep punctuation commit behavior for normal pinyin" >&2
    exit 1
fi
probe_yishanyishande_output=$("$ROOT/build/tipe-state-probe" yishanyishande)
if [[ "$probe_yishanyishande_output" != *$'candidate\t0\t一闪一闪的'* ]]; then
    echo "state probe should keep common repeated phrase candidates ahead of awkward backend guesses" >&2
    exit 1
fi
for alnum_probe in "gpt 4 gpt4" "ipv 6 ipv6"; do
    read -r prefix digit token <<< "$alnum_probe"
    probe_alnum_key_output=$("$ROOT/build/tipe-state-probe" "$prefix" --key "$digit")
    if [[ "$probe_alnum_key_output" != *$'preedit\t'"$token"* ||
        "$probe_alnum_key_output" != *$'candidate\t0\t'"$token"* ||
        "$probe_alnum_key_output" != *$'digits=1'* ]]; then
        echo "state probe should let known alphanumeric English token consume digit key: $token" >&2
        exit 1
    fi
done
probe_chinese_digit_output=$("$ROOT/build/tipe-state-probe" nihao --key 2)
if [[ "$probe_chinese_digit_output" != *$'commit\t你'* ||
    "$probe_chinese_digit_output" == *$'preedit\tnihao2'* ]]; then
    echo "state probe should keep ordinary digit keys available for candidate selection" >&2
    exit 1
fi
probe_mixed_english_output=$("$ROOT/build/tipe-state-probe" dgithubdeshihou)
if [[ "$probe_mixed_english_output" != *$'candidate\t0\t打github的时候'* ||
    "$probe_mixed_english_output" != *$'\t打github'* ]]; then
    echo "state probe should prefer mixed English-Chinese phrase candidates" >&2
    exit 1
fi
probe_git_suffix_output=$("$ROOT/build/tipe-state-probe" gitdeshihou)
if [[ "$probe_git_suffix_output" != *$'candidate\t0\tgit的时候'* ]]; then
    echo "state probe should compose short git raw token with Chinese suffix" >&2
    exit 1
fi
probe_cargo_build_output=$("$ROOT/build/tipe-state-probe" cargobuild)
if [[ "$probe_cargo_build_output" != *$'candidate\t0\tcargobuild'* ]]; then
    echo "state probe should keep adjacent cargo/build letters raw while composing" >&2
    exit 1
fi
probe_cmake_build_output=$("$ROOT/build/tipe-state-probe" cmakebuild)
if [[ "$probe_cmake_build_output" != *$'candidate\t0\tcmakebuild'* ]]; then
    echo "state probe should keep adjacent cmake/build letters raw while composing" >&2
    exit 1
fi
probe_extended_english_suffix_output=$("$ROOT/build/tipe-state-probe" chatgptdeshihou --select chatgpt)
if [[ "$probe_extended_english_suffix_output" != *$'commit\tchatgpt'* ||
    "$probe_extended_english_suffix_output" != *$'preedit\tdeshihou'* ||
    "$probe_extended_english_suffix_output" != *$'candidate\t0\t的时候'* ]]; then
    echo "state probe should compose and split extended raw English token suffix phrases" >&2
    exit 1
fi
probe_ollama_suffix_output=$("$ROOT/build/tipe-state-probe" ollamadeshihou)
if [[ "$probe_ollama_suffix_output" != *$'candidate\t0\tollama的时候'* ]]; then
    echo "state probe should compose local-model English token suffix phrases" >&2
    exit 1
fi
probe_mixed_english_typo_prefix_output=$("$ROOT/build/tipe-state-probe" dgithubdeshihou --select '打github')
if [[ "$probe_mixed_english_typo_prefix_output" != *$'commit\t打github'* ||
    "$probe_mixed_english_typo_prefix_output" != *$'preedit\tdeshihou'* ||
    "$probe_mixed_english_typo_prefix_output" != *$'candidate\t0\t的时候'* ]]; then
    echo "state probe should keep remaining pinyin after typo mixed English-token prefix selection" >&2
    exit 1
fi
probe_mixed_english_prefix_output=$("$ROOT/build/tipe-state-probe" dagithubdeshihou --select '打github')
if [[ "$probe_mixed_english_prefix_output" != *$'commit\t打github'* ||
    "$probe_mixed_english_prefix_output" != *$'preedit\tdeshihou'* ||
    "$probe_mixed_english_prefix_output" != *$'candidate\t0\t的时候'* ]]; then
    echo "state probe should keep remaining pinyin after mixed English-token prefix selection" >&2
    exit 1
fi
if command -v timeout >/dev/null 2>&1; then
    if ! timeout 5s "$ROOT/build/tipe-state-probe" \
        houxuanchuangxianshidebushigithubzhegeyingwenershizhongwen >/dev/null; then
        echo "state probe should keep long mixed English prefix lookup bounded" >&2
        exit 1
    fi
fi
for real_phrase_pair in \
    "yidingyaobanihaodawan:一定要把你好打完" \
    "buranmeiyouhouxuanchuang:不然没有候选窗" \
    "dagithubdeshihou:打github的时候" \
    "houxuanchuangxianshidebushigithubzhegeyingwenershizhongwen:候选窗显示的不是github这个英文而是中文" \
    "andown:按down" \
    "bianchenggengchangdeyilie:变成更长的一列" \
    "suoyine:所以呢" \
    "qingwenyixia:请问一下" \
    "ruguo:如果" \
    "zhegebudui:这个不对" \
    "zhegebuxing:这个不行" \
    "dengyixia:等一下" \
    "shaodengyixia:稍等一下" \
    "woxiangwenyigewenti:我想问一个问题" \
    "nihaowoxiangyaozuoshenme:你好我想要做什么" \
    "woxiangyonggithub:我想用github" \
    "woxiangyongchatgpt:我想用chatgpt" \
    "woxiangyonggit:我想用git" \
    "woxiangyongdocker:我想用docker" \
    "woxiangyongpython:我想用python" \
    "woxiangyongollama:我想用ollama" \
    "woxiangyongniri:我想用niri" \
    "githubshang:github上" \
    "dockerlimian:docker里面" \
    "systemdfuwu:systemd服务" \
    "dbusxiaoxi:dbus消息" \
    "dakaigithub:打开github" \
    "dakaigit:打开git" \
    "dakaiollama:打开ollama" \
    "dakainiri:打开niri" \
    "xiugaidocker:修改docker" \
    "xiugaigit:修改git" \
    "xiugaipython:修改python" \
    "cikuzhexiekeyiyibudaoweia:词库这些可以一步到位啊" \
    "cikubuyinggaichushishezhiyigebijiaohaoyongdeshunxuma:词库不应该初始设置一个比较好用的顺序吗" \
    "erqiewoganjuemeiyougengxin:而且我感觉没有更新" \
    "pinyinjiuquanbuxiaoshile:拼音就全部消失了" \
    "bunengjiezhexuanhaozhegezi:不能接着选好这个字" \
    "zhexiewentixianchengdeshurufazaojiujiejuele:这些问题现成的输入法早就解决了"; do
    real_phrase_input="${real_phrase_pair%%:*}"
    real_phrase_candidate="${real_phrase_pair#*:}"
    real_phrase_output=$("$ROOT/build/tipe-state-probe" "$real_phrase_input")
    if [[ "$real_phrase_output" != *$'candidate\t0\t'"$real_phrase_candidate"* ]]; then
        echo "state probe should prefer real trial phrase candidate: $real_phrase_input -> $real_phrase_candidate" >&2
        exit 1
    fi
done
for alnum_mixed_probe in \
    "woxiangyongqwen 2 我想用qwen2" \
    "woxiangyonggpt 4 我想用gpt4" \
    "woxiangyongipv 6 我想用ipv6"; do
    read -r prefix digit expected <<< "$alnum_mixed_probe"
    probe_real_phrase_output=$(printf '%s\n' "type $prefix" "key $digit" |
        "$ROOT/build/tipe-state-probe" "" --script -)
    if [[ "$probe_real_phrase_output" != *$'candidate\t0\t'"$expected"* ]]; then
        echo "state probe should prefer alphanumeric mixed phrase candidate for $prefix$digit" >&2
        exit 1
    fi
done
probe_expanded_digit_output=$("$ROOT/build/tipe-state-probe" nihao --move Down --move Down --digit 1)
probe_expanded_digit_commit=$(awk -F $'\t' '$1 == "commit" { print $2; exit }' <<<"$probe_expanded_digit_output")
if [[ "$probe_expanded_digit_output" != *$'action\tdigit\t1\tcommit'* ||
    -z "$probe_expanded_digit_commit" ||
    "$probe_expanded_digit_commit" != "$probe_visible_expanded_candidate" ]]; then
    echo "state probe should expose expanded-row digit selection behavior" >&2
    exit 1
fi
probe_punct_output=$("$ROOT/build/tipe-state-probe" nihao --punct ,)
if [[ "$probe_punct_output" != *$'action\tpunct\t0\tcommit'* || "$probe_punct_output" != *$'commit\t你好'* ||
    "$probe_punct_output" != *$'observed=1'* || "$probe_punct_output" != *$'candidate_selections=1'* ]]; then
    echo "state probe should expose punctuation commit passthrough behavior" >&2
    exit 1
fi
probe_prefix_punct_script="$tmp_dir/probe-prefix-punct.txt"
printf '%s\n' \
    'type jixuzuo' \
    'select 继续' \
    'expect-preedit zuo' \
    'punct ,' \
    'expect-preedit' >"$probe_prefix_punct_script"
probe_prefix_punct_output=$("$ROOT/build/tipe-state-probe" "" --script "$probe_prefix_punct_script")
if [[ "$probe_prefix_punct_output" != *$'action\tpunct\t0\tcommit'* ||
    "$probe_prefix_punct_output" != *$'commit\t继续'* ||
    "$probe_prefix_punct_output" != *$'commit\t做'* ]]; then
    echo "state probe should keep remaining pinyin after punctuation commits a prefix candidate" >&2
    exit 1
fi
probe_key_punct_output=$("$ROOT/build/tipe-state-probe" nihao --key KP_Add)
if [[ "$probe_key_punct_output" != *$'action\tkey\t0\tcommit'* || "$probe_key_punct_output" != *$'commit\t你好'* ||
    "$probe_key_punct_output" != *$'observed=1'* || "$probe_key_punct_output" != *$'candidate_selections=1'* ]]; then
    echo "state probe should expose keypad punctuation through engine-style key replay" >&2
    exit 1
fi
probe_edit_output=$("$ROOT/build/tipe-state-probe" nihao --backspace --delete --escape)
if [[ "$probe_edit_output" != *$'action\tbackspace\t1\tupdate'* ||
    "$probe_edit_output" != *$'action\tdelete\t1\tnone'* ||
    "$probe_edit_output" != *$'action\tescape\t1\tclear'* || "$probe_edit_output" != *$'preedit\t'* ||
    "$probe_edit_output" != *$'backspaces=1'* || "$probe_edit_output" != *$'deletes=1'* ||
    "$probe_edit_output" != *$'escapes=1'* ]]; then
    echo "state probe should expose edit and clear key behavior" >&2
    exit 1
fi
probe_event_output=$("$ROOT/build/tipe-state-probe" nihao --observe Tab --move Down --rerank)
if [[ "$probe_event_output" != *$'observed=1'* || "$probe_event_output" != *$'cursor_moves=1'* ||
    "$probe_event_output" != *$'reranks=1'* ]]; then
    echo "state probe should expose observed, cursor, and rerank event counts" >&2
    exit 1
fi
probe_key_rerank_output=$("$ROOT/build/tipe-state-probe" nihao --key F9)
if [[ "$probe_key_rerank_output" != *$'action\tkey\t1\tupdate'* ||
    "$probe_key_rerank_output" != *$'reranks=1'* ||
    "$probe_key_rerank_output" != *$'expanded\t1'* ]]; then
    echo "state probe should expose F9 rerank through engine-style key replay" >&2
    exit 1
fi
probe_reserved_fcitx_toggle_output=$("$ROOT/build/tipe-state-probe" nihao --key Ctrl+Space)
if [[ "$probe_reserved_fcitx_toggle_output" == *'reranks=1'* ]]; then
    echo "state probe must not treat fcitx5's Ctrl+Space activation toggle as a TiPE rerank" >&2
    exit 1
fi
continuous_probe_model="$tmp_dir/continuous-probe-model.sh"
continuous_probe_marker="$tmp_dir/continuous-probe-called"
cat >"$continuous_probe_model" <<EOF
#!/usr/bin/env bash
touch "$continuous_probe_marker"
printf 'candidate\t你号\n'
EOF
chmod +x "$continuous_probe_model"
probe_continuous_rerank_output=$(TIPE_MODEL_COMMAND="$continuous_probe_model" \
    "$ROOT/build/tipe-state-probe" nihao --continuous-rerank)
if [[ "$probe_continuous_rerank_output" != *$'action\tcontinuous-rerank\t1\tupdate'* ||
    "$probe_continuous_rerank_output" != *$'reranks=1'* ||
    "$probe_continuous_rerank_output" != *$'expanded\t0'* ||
    -e "$continuous_probe_marker" ]]; then
    echo "continuous rerank should stay collapsed and avoid external model commands" >&2
    exit 1
fi
probe_recent_events_output=$("$ROOT/build/tipe-state-probe" nihao --observe Tab --move Down --rerank --events)
if [[ "$probe_recent_events_output" != *$'event\t0\tletter\tn'* ||
    "$probe_recent_events_output" != *$'event\t5\tobserved\tTab'* ||
    "$probe_recent_events_output" != *$'event\t6\tcursor-move\tDown'* ||
    "$probe_recent_events_output" != *$'event\t7\trerank-requested\tnihao'* ]]; then
    echo "state probe should expose ordered recent event details" >&2
    exit 1
fi
probe_escaped_events_output=$("$ROOT/build/tipe-state-probe" nihao --observe $'Tab\tLine\nSlash\\' --events)
if [[ "$probe_escaped_events_output" != *$'event\t5\tobserved\tTab\\tLine\\nSlash\\\\'* ]]; then
    echo "state probe should escape recent event text" >&2
    exit 1
fi
if "$ROOT/build/tipe-state-probe" nihao --select 不存在 >/dev/null 2>&1; then
    echo "state probe should reject unknown selected candidates" >&2
    exit 1
fi
probe_model="$tmp_dir/probe-model.sh"
cat >"$probe_model" <<'EOF'
#!/usr/bin/env bash
input=$(cat)
case "$input" in
    *$'preedit\tnihao'*)
        printf 'candidate\t你号\n'
        ;;
    *$'preedit\tihao'*)
        printf 'correction\tihao\tnihao\n'
        ;;
esac
EOF
chmod +x "$probe_model"
probe_rerank_output=$(TIPE_MODEL_COMMAND="$probe_model" "$ROOT/build/tipe-state-probe" nihao --observe Tab --move Down --rerank)
if [[ "$probe_rerank_output" != *$'action\trerank\t1\tupdate'* || "$probe_rerank_output" != *$'candidate\t0\t你号'* ]]; then
    echo "state probe should expose model rerank results" >&2
    exit 1
fi
probe_model_learning_preferences="$tmp_dir/probe-model-learning-preferences.tsv"
probe_model_learning_output=$(
    TIPE_MODEL_COMMAND="$probe_model" "$ROOT/build/tipe-state-probe" nihao \
        --preferences "$probe_model_learning_preferences" --rerank
)
probe_model_learning_restored_output=$(
    "$ROOT/build/tipe-state-probe" nihao --preferences "$probe_model_learning_preferences"
)
if [[ "$probe_model_learning_output" != *$'candidate\t0\t你号'* ||
    "$probe_model_learning_restored_output" != *$'candidate\t0\t你号'* ]]; then
    echo "state probe should persist external model candidate suggestions as local preferences" >&2
    exit 1
fi
probe_preference_only_model="$tmp_dir/probe-preference-only-model.sh"
cat >"$probe_preference_only_model" <<'EOF'
#!/usr/bin/env bash
cat >/dev/null
printf 'preference\tnihao\t你号\t4\n'
EOF
chmod +x "$probe_preference_only_model"
probe_preference_only_output=$(
    TIPE_MODEL_COMMAND="$probe_preference_only_model" "$ROOT/build/tipe-state-probe" nihao --rerank
)
if [[ "$probe_preference_only_output" != *$'action\trerank\t1\tupdate'* ||
    "$probe_preference_only_output" != *$'candidate\t0\t你号'* ]]; then
    echo "state probe should immediately apply explicit model preference rows to the current candidate order" >&2
    exit 1
fi
probe_segment_chain_only_model="$tmp_dir/probe-segment-chain-only-model.sh"
cat >"$probe_segment_chain_only_model" <<'EOF'
#!/usr/bin/env bash
cat >/dev/null
printf 'segment_chain\tnihao\tni\t你\thao\tnihao\t你号\t4\n'
EOF
chmod +x "$probe_segment_chain_only_model"
probe_segment_chain_only_output=$(
    TIPE_MODEL_COMMAND="$probe_segment_chain_only_model" "$ROOT/build/tipe-state-probe" nihao --rerank
)
if [[ "$probe_segment_chain_only_output" != *$'action\trerank\t1\tupdate'* ||
    "$probe_segment_chain_only_output" != *$'candidate\t0\t你号'* ]]; then
    echo "state probe should immediately apply explicit model segment-chain rows to the current candidate order" >&2
    exit 1
fi
probe_echo_known_model="$tmp_dir/probe-echo-known-model.sh"
cat >"$probe_echo_known_model" <<'EOF'
#!/usr/bin/env bash
while IFS= read -r line || [[ -n "$line" ]]; do
    case "$line" in
        preference$'\t'*)
            printf '%s\n' "$line"
            ;;
        correction$'\t'*)
            IFS=$'\t' read -r _ typo corrected _count <<< "$line"
            printf 'correction\t%s\t%s\n' "$typo" "$corrected"
            ;;
        segment_chain$'\t'*)
            printf '%s\n' "$line"
            ;;
    esac
done
EOF
chmod +x "$probe_echo_known_model"
probe_echo_known_preferences="$tmp_dir/probe-echo-known-preferences.tsv"
printf 'nihao\t你号\t4\n__correction__\tihao\tnihao\t2\n__segment_chain__\tnihao\tni\t你\thao\tnihao\t你号\t3\n' >"$probe_echo_known_preferences"
probe_echo_known_output=$(
    TIPE_MODEL_COMMAND="$probe_echo_known_model" "$ROOT/build/tipe-state-probe" nihao \
        --preferences "$probe_echo_known_preferences" --rerank
)
probe_echo_known_preferences_after=$(cat "$probe_echo_known_preferences")
if [[ "$probe_echo_known_output" != *$'candidate\t0\t你号'* ||
    "$probe_echo_known_preferences_after" != $'nihao\t你号\t4\n__correction__\tihao\tnihao\t2\n__segment_chain__\tnihao\tni\t你\thao\tnihao\t你号\t3' ]]; then
    echo "state probe should not amplify known preference, correction, or segment-chain rows echoed by the external model" >&2
    exit 1
fi
probe_correction_output=$(TIPE_MODEL_COMMAND="$probe_model" "$ROOT/build/tipe-state-probe" ihao --rerank)
if [[ "$probe_correction_output" != *$'action\trerank\t1\tupdate'* ||
    "$probe_correction_output" != *$'candidate\t0\t你好'* ]]; then
    echo "state probe should expose model correction results" >&2
    exit 1
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" && -x "$HOME/.local/bin/tipe-state-probe" ]]; then
    installed_probe_output=$("$HOME/.local/bin/tipe-state-probe" guangbiaogensuishibai)
    if [[ "$installed_probe_output" != *$'candidate\t0\t光标跟随失败'* ]]; then
        echo "installed state probe should show cursor-follow phrase first candidate" >&2
        exit 1
    fi
    installed_probe_preferences_path="$tmp_dir/installed-probe-preferences.tsv"
    "$HOME/.local/bin/tipe-state-probe" nihao --preferences "$installed_probe_preferences_path" --select 你号 >/dev/null
    installed_probe_preferences_output=$("$HOME/.local/bin/tipe-state-probe" nihao --preferences "$installed_probe_preferences_path")
    if [[ "$installed_probe_preferences_output" != *$'candidate\t0\t你号'* ]]; then
        echo "installed state probe should reuse an explicit preference file across processes" >&2
        exit 1
    fi
    installed_probe_prefix_preferences_path="$tmp_dir/installed-probe-prefix-preferences.tsv"
    "$HOME/.local/bin/tipe-state-probe" jixuzuo --preferences "$installed_probe_prefix_preferences_path" --select 继续 >/dev/null
    installed_probe_prefix_preferences_output=$("$HOME/.local/bin/tipe-state-probe" jixuzuo --preferences "$installed_probe_prefix_preferences_path")
    if [[ "$installed_probe_prefix_preferences_output" != *$'candidate\t0\t继续做'* &&
        "$installed_probe_prefix_preferences_output" != *$'candidate\t0\t继续'* ]]; then
        echo "installed state probe should expose learned prefix preference behavior" >&2
        exit 1
    fi
fi

if [[ -x "$ROOT/build/tipe-candidate-window" ]]; then
    "$ROOT/build/tipe-candidate-window" --self-test
    probe_snapshot_output=$("$ROOT/build/tipe-state-probe" nihao --move Down --move Down --snapshot 100,200,3,18)
    probe_snapshot_line=$(printf '%s\n' "$probe_snapshot_output" | awk -F '\t' '$1 == "snapshot" {sub(/^snapshot\t/, ""); print; exit}')
    parsed_probe_snapshot=$(printf '%s\n' "$probe_snapshot_line" | "$ROOT/build/tipe-candidate-window" --parse-snapshot -)
    parsed_probe_selected_row=$(awk -F $'\t' \
        '$1 == "candidate" && $2 == "6" { print $3 "\t" $4 "\t" $5 "\t" $6; exit }' \
        <<<"$parsed_probe_snapshot")
    if [[ "$parsed_probe_snapshot" != *$'preedit\tnihao'* ||
        "$parsed_probe_snapshot" != *$'preedit-cursor\t5'* ||
        "$parsed_probe_snapshot" != *$'expanded\t1'* ||
        "$parsed_probe_snapshot" != *$'selected\t6'* ||
        "$parsed_probe_snapshot" != *$'cursor\t100\t200\t3\t18'* ||
        "$parsed_probe_snapshot" != *$'layout\t596'* ||
        "$parsed_probe_snapshot" != *$'supervision\t7\t0\t0'* ||
        "$parsed_probe_snapshot" == *$'continuous\t1'* ||
        "$parsed_probe_selected_row" != $'1\t1\t1\t'"$probe_visible_expanded_candidate" ]]; then
        echo "state probe snapshot should round-trip through candidate window parser" >&2
        exit 1
    fi
    continuous_probe_snapshot_output=$("$ROOT/build/tipe-state-probe" nihao --continuous-mode --continuous-rerank --snapshot 100,200,3,18)
    continuous_probe_snapshot_line=$(printf '%s\n' "$continuous_probe_snapshot_output" | awk -F '\t' '$1 == "snapshot" {sub(/^snapshot\t/, ""); print; exit}')
    parsed_continuous_probe_snapshot=$(printf '%s\n' "$continuous_probe_snapshot_line" | "$ROOT/build/tipe-candidate-window" --parse-snapshot -)
    if [[ "$parsed_continuous_probe_snapshot" != *$'expanded\t0'* ||
        "$parsed_continuous_probe_snapshot" != *$'preedit-cursor\t5'* ||
        "$parsed_continuous_probe_snapshot" != *$'supervision\t5\t0\t1'* ||
        "$parsed_continuous_probe_snapshot" != *$'continuous\t1'* ]]; then
        echo "state probe snapshot should include continuous-mode supervision metadata" >&2
        exit 1
    fi
    scripted_continuous_snapshot_output=$(
        printf '%s\n' 'type nihao' 'continuous-rerank' |
            "$ROOT/build/tipe-state-probe" "" --script - --snapshot 100,200,3,18
    )
    scripted_continuous_snapshot_line=$(printf '%s\n' "$scripted_continuous_snapshot_output" | awk -F '\t' '$1 == "snapshot" {sub(/^snapshot\t/, ""); print; exit}')
    parsed_scripted_continuous_snapshot=$(printf '%s\n' "$scripted_continuous_snapshot_line" | "$ROOT/build/tipe-candidate-window" --parse-snapshot -)
    if [[ "$parsed_scripted_continuous_snapshot" != *$'continuous\t1'* ||
        "$parsed_scripted_continuous_snapshot" != *$'supervision\t5\t0\t1'* ]]; then
        echo "state probe script continuous-rerank should mark continuous-mode metadata" >&2
        exit 1
    fi
    parsed_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'nihao\t1\t7\t100\t200\t3\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好|A\\|B')
    if [[ "$parsed_snapshot" != *$'preedit\tnihao'* ||
        "$parsed_snapshot" != *$'expanded\t1'* ||
        "$parsed_snapshot" != *$'selected\t7'* ||
        "$parsed_snapshot" != *$'cursor\t100\t200\t3\t18'* ||
        "$parsed_snapshot" != *$'layout\t596'* ||
        "$parsed_snapshot" != *$'cell\t6\t1\t0\t1'* ||
        "$parsed_snapshot" != *$'cell\t7\t1\t1\t1'* ||
        "$parsed_snapshot" != *$'candidate\t6\t0\t1\t1\t你不好'* ||
        "$parsed_snapshot" != *$'candidate\t7\t1\t1\t2\t你很好'* ||
        "$parsed_snapshot" != *$'candidate\t8\t0\t1\t3\tA|B'* ]]; then
        echo "candidate window should parse snapshots without starting GUI" >&2
        exit 1
    fi
    parsed_long_cell_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'chang\t1\t1\t100\t200\t3\t18\t短|这是一个非常长的候选|二|三|四|五|六|七')
    if [[ "$parsed_long_cell_snapshot" != *$'layout\t596\t96\t1'* ||
        "$parsed_long_cell_snapshot" != *$'candidate\t1\t1\t1\t2\t这是一个非常长的候选'* ||
        "$parsed_long_cell_snapshot" != *$'cell\t1\t0\t1\t3'* ||
        "$parsed_long_cell_snapshot" != *$'candidate\t5\t0\t1\t\t五'* ||
        "$parsed_long_cell_snapshot" != *$'cell\t5\t1\t1\t1'* ]]; then
        echo "expanded candidate window should keep fixed width and let long candidates span cells" >&2
        exit 1
    fi
    parsed_collapsed_long_cell_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'chang\t0\t0\t100\t200\t3\t18\t长句先选前半段后|短|后面的拼音不保留|候选|另外|更多')
    if [[ "$parsed_collapsed_long_cell_snapshot" != *$'layout\t463\t68\t1'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'candidate\t0\t1\t1\t1\t长句先选前半段后'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'cell\t0\t0\t0\t2'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'draw-cell\t0\t10\t35\t168\t63\t1'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'draw-cell\t1\t170\t35\t223\t63\t1'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'candidate\t2\t0\t1\t3\t后面的拼音不保留'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'cell\t2\t0\t3\t2'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'draw-cell\t2\t225\t35\t383\t63\t1'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'draw-cell\t3\t385\t35\t453\t63\t1'* ||
        "$parsed_collapsed_long_cell_snapshot" != *$'candidate\t4\t0\t0\t\t另外'* ]]; then
        echo "collapsed candidate window should draw measured compact cells without shrinking them back to six fixed columns" >&2
        exit 1
    fi
    parsed_supervision_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'nihao\t0\t0\t100\t200\t3\t18\t你好|你号\tsupervision=1,keys=12,selects=2,reranks=1,continuous=1')
    if [[ "$parsed_supervision_snapshot" != *$'layout\t158\t68\t1'* ||
        "$parsed_supervision_snapshot" != *$'supervision\t12\t2\t1'* ||
        "$parsed_supervision_snapshot" != *$'continuous\t1'* ||
        "$parsed_supervision_snapshot" != *$'candidate\t0\t1\t1\t1\t你好'* ]]; then
        echo "candidate window should parse supervision metadata without adding visible footer rows" >&2
        exit 1
    fi
    visual_row_probe=$(printf '%s\n' \
        'type haodewokanyxiahaiyoumeiyu' \
        'move Down' \
        'move Down' |
        "$ROOT/build/tipe-state-probe" '' --script -)
    visual_row_candidate_4=$(awk -F $'\t' '$1 == "candidate" && $2 == "4" { print $3; exit }' \
        <<<"$visual_row_probe")
    visual_row_slot_2=$(awk -F $'\t' '$1 == "visible" && $2 == "2" { print $3 "\t" $4; exit }' \
        <<<"$visual_row_probe")
    visual_row_slot_4=$(awk -F $'\t' '$1 == "visible" && $2 == "4" { print $3 "\t" $4; exit }' \
        <<<"$visual_row_probe")
    if [[ "$visual_row_probe" != *$'selected\t3'* ||
        -z "$visual_row_candidate_4" ||
        "$visual_row_slot_2" != $'4\t'"$visual_row_candidate_4" ||
        "$visual_row_slot_4" == $'4\t'"$visual_row_candidate_4" ]]; then
        echo "state probe should print row-local visible digits for visual candidate rows" >&2
        exit 1
    fi
    parsed_visual_row_snapshot=$(printf '%s\n' \
        'type haodewokanyxiahaiyoumeiyu' \
        'move Down' \
        'move Down' |
        "$ROOT/build/tipe-state-probe" '' --script - --snapshot 100,200,3,18 |
        awk -F '\t' '$1 == "snapshot" {sub(/^snapshot\t/, ""); print; exit}' |
        "$ROOT/build/tipe-candidate-window" --parse-snapshot -)
    parsed_visual_candidate_4=$(awk -F $'\t' \
        '$1 == "candidate" && $2 == "4" { print $3 "\t" $4 "\t" $5 "\t" $6; exit }' \
        <<<"$parsed_visual_row_snapshot")
    parsed_visual_candidate_5=$(awk -F $'\t' \
        '$1 == "candidate" && $2 == "5" { print $3 "\t" $4 "\t" $5; exit }' \
        <<<"$parsed_visual_row_snapshot")
    parsed_visual_cell_4=$(awk -F $'\t' \
        '$1 == "cell" && $2 == "4" { print $3 "\t" $5; exit }' \
        <<<"$parsed_visual_row_snapshot")
    if [[ "$parsed_visual_candidate_4" != $'0\t1\t2\t'"$visual_row_candidate_4" ||
        "$parsed_visual_cell_4" != $'1\t3' ||
        "$parsed_visual_candidate_5" != $'0\t1\t' ]]; then
        echo "candidate window should use row-local shortcuts after long visual candidates" >&2
        exit 1
    fi
    parsed_long_sentence_snapshot=$("$ROOT/build/tipe-state-probe" \
        changjuxianxuanqianbanduanhoumiandepinyinbubaoliu --move Down --snapshot 100,200,3,18 |
        awk -F '\t' '$1 == "snapshot" {sub(/^snapshot\t/, ""); print; exit}' |
        "$ROOT/build/tipe-candidate-window" --parse-snapshot -)
    parsed_long_sentence_full=$(awk -F $'\t' \
        '$1 == "candidate" && $6 == "长句先选前半段后面的拼音不保留" { print $3 "\t" $4; exit }' \
        <<<"$parsed_long_sentence_snapshot")
    parsed_long_sentence_divergent=$(awk -F $'\t' \
        '$1 == "candidate" && $6 == "长局限选前半段后面的拼音不保留" { print $3 "\t" $4; exit }' \
        <<<"$parsed_long_sentence_snapshot")
    if [[ "$parsed_long_sentence_snapshot" != *$'candidate\t0\t1\t1\t1\t长句先选前半段后'* ||
        "$parsed_long_sentence_full" != $'0\t1' ||
        ( -n "$parsed_long_sentence_divergent" && "$parsed_long_sentence_divergent" != $'0\t0' ) ]]; then
        echo "long sentence divergent guesses should stay out of the first expanded page" >&2
        exit 1
    fi
    parsed_layout_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'nihao\t1\t7\t1240\t700\t2\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好' \
        --layout-geometry 0,0,1280,720)
    if [[ "$parsed_layout_snapshot" != *$'layout\t596\t96\t1'* ||
        "$parsed_layout_snapshot" != *$'edge-fallback\t1'* ||
        "$parsed_layout_snapshot" != *$'position\t646\t552\t1242\t648'* ]]; then
        echo "candidate window snapshot parser should expose cursor-attached edge position" >&2
        exit 1
    fi
    parsed_bottom_edge_layout_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'nihao\t1\t7\t600\t650\t2\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好' \
        --layout-geometry 0,0,1280,720)
    if [[ "$parsed_bottom_edge_layout_snapshot" != *$'layout\t596\t96\t1'* ||
        "$parsed_bottom_edge_layout_snapshot" != *$'edge-fallback\t1'* ||
        "$parsed_bottom_edge_layout_snapshot" != *$'position\t600\t550\t1196\t646'* ]]; then
        echo "candidate window snapshot parser should fallback before bottom edge overflow on 720p monitors" >&2
        exit 1
    fi
    parsed_fallback_layout_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'nihao\t0\t0\t0\t0\t0\t0\t你好|你号' \
        --layout-geometry 0,0,1280,720)
    if [[ "$parsed_fallback_layout_snapshot" != *$'layout\t158\t68\t0'* ||
        "$parsed_fallback_layout_snapshot" != *$'edge-fallback\t0'* ||
        "$parsed_fallback_layout_snapshot" != *$'position\t561\t496\t719\t564'* ]]; then
        echo "candidate window snapshot parser should expose fallback layout for unusable cursor rects" >&2
        exit 1
    fi
    parsed_right_monitor_layout_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'nihao\t1\t7\t2520\t700\t2\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好' \
        --layout-geometry 1280,0,1280,720)
    if [[ "$parsed_right_monitor_layout_snapshot" != *$'layout\t596\t96\t1'* ||
        "$parsed_right_monitor_layout_snapshot" != *$'edge-fallback\t1'* ||
        "$parsed_right_monitor_layout_snapshot" != *$'position\t1926\t552\t2522\t648'* ]]; then
        echo "candidate window snapshot parser should print global position on right-hand monitors" >&2
        exit 1
    fi
    parsed_left_monitor_layout_snapshot=$("$ROOT/build/tipe-candidate-window" --parse-snapshot \
        $'nihao\t0\t0\t-1279\t1\t1\t16\t你好|你号' \
        --layout-geometry -1280,0,1280,720)
    if [[ "$parsed_left_monitor_layout_snapshot" != *$'layout\t158\t68\t1'* ||
        "$parsed_left_monitor_layout_snapshot" != *$'edge-fallback\t0'* ||
        "$parsed_left_monitor_layout_snapshot" != *$'position\t-1256\t24\t-1098\t92'* ]]; then
        echo "candidate window snapshot parser should print global position on left-hand monitors" >&2
        exit 1
    fi
    if "$ROOT/build/tipe-candidate-window" --parse-snapshot $'bad\t1\t7x\t100\t200\t3\t18\t你好' >/dev/null 2>&1; then
        echo "candidate window snapshot parser should reject invalid numeric fields" >&2
        exit 1
    fi
    if "$ROOT/build/tipe-candidate-window" --parse-snapshot >/dev/null 2>&1; then
        echo "candidate window should reject missing --parse-snapshot argument without starting GUI" >&2
        exit 1
    fi
    if "$ROOT/build/tipe-candidate-window" --parse-snapshot $'nihao\t0\t0\t10\t20\t2\t16\t你好' --layout-geometry 0,0,0,720 >/dev/null 2>&1; then
        echo "candidate window should reject invalid layout geometry without starting GUI" >&2
        exit 1
    fi
    if "$ROOT/build/tipe-candidate-window" --snapshot $'bad\t1\t7x\t100\t200\t3\t18\t你好' --ttl-ms 200 >/dev/null 2>&1; then
        echo "candidate window should reject invalid render snapshots without starting GUI" >&2
        exit 1
    fi
    if "$ROOT/build/tipe-candidate-window" --snapshot $'nihao\t0\t0\t10\t20\t2\t16\t你好' --ttl-ms 20 >/dev/null 2>&1; then
        echo "candidate window should reject too-small TTL values" >&2
        exit 1
    fi
fi
if [[ -x "$ROOT/build/tipe-learning-panel-window" ]]; then
    "$ROOT/build/tipe-learning-panel-window" --self-test
    parsed_learning_panel=$(printf 'panel\tstate\tpreedit\tnihao\npanel\tbehavior\tpossible-correction\t1\tfull-delete-retype\tihao\tnihao\n' |
        "$ROOT/build/tipe-learning-panel-window" --parse-panel -)
    if [[ "$parsed_learning_panel" != *$'state\tpreedit\tnihao'* ||
        "$parsed_learning_panel" != *$'behavior\tpossible-correction\t1\tfull-delete-retype\tihao\tnihao'* ]]; then
        echo "learning panel window should parse raw panel rows without opening GTK" >&2
        exit 1
    fi
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" && -x "$HOME/.local/bin/tipe-candidate-window" ]]; then
    "$HOME/.local/bin/tipe-candidate-window" --self-test
    installed_parsed_snapshot=$("$HOME/.local/bin/tipe-candidate-window" --parse-snapshot \
        $'nihao\t0\t0\t10\t20\t2\t16\t你好|你号')
    if [[ "$installed_parsed_snapshot" != *$'candidate\t0\t1\t1\t1\t你好'* ]]; then
        echo "installed candidate window should parse snapshots without starting GUI" >&2
        exit 1
    fi
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" && -x "$HOME/.local/bin/tipe-learning-panel-window" ]]; then
    "$HOME/.local/bin/tipe-learning-panel-window" --self-test
fi

bash -n "$ROOT/scripts/model-protocol-example.sh"
bash -n "$ROOT/scripts/model-adapter.sh"
bash -n "$ROOT/scripts/model-dump.sh"
bash -n "$ROOT/scripts/model-explain.sh"
bash -n "$ROOT/scripts/learning-panel.sh"
bash -n "$ROOT/scripts/supervision-window.sh"
bash -n "$ROOT/scripts/analyze-window.sh"
bash -n "$ROOT/scripts/model-replay.sh"
bash -n "$ROOT/scripts/model-current.sh"
bash -n "$ROOT/scripts/model-config.sh"
bash -n "$ROOT/scripts/model-self-test.sh"
bash -n "$ROOT/scripts/model-wrapper-new.sh"
bash -n "$ROOT/scripts/model-wrapper-check.sh"
"$ROOT/scripts/training-export.py" --help >/dev/null
python3 "$ROOT/tests/training_export_test.py" "$ROOT/scripts/training-export.py" >/dev/null
"$ROOT/scripts/personal-model.py" --help >/dev/null
python3 "$ROOT/tests/personal_model_test.py" "$ROOT/scripts/personal-model.py" \
    "$ROOT/scripts/personal-model-train.sh" >/dev/null
bash -n "$ROOT/scripts/personal-model-train.sh"
bash -n "$ROOT/scripts/restart-fcitx5.sh"
bash -n "$ROOT/tests/restart_test.sh"
"$ROOT/tests/restart_test.sh" "$ROOT/scripts/restart-fcitx5.sh" >/dev/null
bash -n "$ROOT/scripts/check-user-dictionary.sh"
bash -n "$ROOT/scripts/check-preferences.sh"
bash -n "$ROOT/scripts/doctor.sh"
bash -n "$ROOT/scripts/installed-files.sh"
bash -n "$ROOT/scripts/install.sh"
bash -n "$ROOT/scripts/uninstall.sh"
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$ROOT/data/tipe-supervision.desktop"
elif ! grep -Fxq '[Desktop Entry]' "$ROOT/data/tipe-supervision.desktop" ||
    ! grep -Fxq 'Exec=tipe-supervision-window' "$ROOT/data/tipe-supervision.desktop"; then
    echo "desktop entry should expose the TiPE supervision window" >&2
    exit 1
fi
if ! grep -Fxq 'Name=TiPE' "$ROOT/data/tipe-supervision.desktop" ||
    ! grep -Fxq 'Icon=tipe' "$ROOT/data/tipe-supervision.desktop" ||
    grep -q '^Actions=' "$ROOT/data/tipe-supervision.desktop" ||
    [[ ! -r "$ROOT/data/icons/hicolor/scalable/apps/tipe.svg" ]] ||
    [[ ! -s "$ROOT/data/support/wechat.png" ]] ||
    [[ ! -s "$ROOT/data/support/alipay.png" ]]; then
    echo "desktop UI resources should include the TiPE icon and both support codes" >&2
    exit 1
fi
for support_image in "$ROOT/data/support/wechat.png" "$ROOT/data/support/alipay.png"; do
    if [[ "$(od -An -tx1 -N8 "$support_image" | tr -d ' \n')" != "89504e470d0a1a0a" ]]; then
        echo "support image should be a readable PNG: $support_image" >&2
        exit 1
    fi
done

source_helper_home="$tmp_dir/source-helper-home"
mkdir -p "$source_helper_home/.local/bin"
cat >"$source_helper_home/.local/bin/tipe-model-adapter" <<'EOF'
#!/usr/bin/env bash
printf 'stale-installed-helper\n'
EOF
chmod +x "$source_helper_home/.local/bin/tipe-model-adapter"
source_model_current_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\npreference\tnihao\t你号\t5\n' |
        HOME="$source_helper_home" TIPE_MODEL_CONFIG="$tmp_dir/missing-source-helper-config" \
        TIPE_MODEL_MODE=heuristic "$ROOT/scripts/model-current.sh"
)
if [[ "$source_model_current_output" == *stale-installed-helper* ||
    "$source_model_current_output" != *$'candidate\t你号'* ]]; then
    echo "source model-current should use same-tree helpers instead of stale installed copies" >&2
    exit 1
fi

ctest_list=$(ctest --test-dir "$ROOT/build" -N)
for registered_test in state training-export personal-model restart-helper nonblocking-pipe candidate-window learning-panel-window; do
    if ! grep -Eq "Test[[:space:]]+#[0-9]+:[[:space:]]+$registered_test$" <<<"$ctest_list"; then
        echo "CTest registration is missing: $registered_test" >&2
        exit 1
    fi
done

home_error="$tmp_dir/home.err"
if env -u HOME "$ROOT/scripts/build.sh" >/dev/null 2>"$home_error"; then
    echo "build helper should reject a missing HOME with a clear message" >&2
    exit 1
fi
if ! grep -qF "HOME is not set; cannot choose the TiPE user install prefix" "$home_error"; then
    echo "build helper should explain why HOME is required" >&2
    exit 1
fi
if "$ROOT/scripts/build.sh" --unknown >/dev/null 2>&1; then
    echo "build helper should reject unknown arguments" >&2
    exit 1
fi
sanitizer_config_dir="$tmp_dir/sanitizer-config"
set +e
sanitizer_config_output=$(cmake -S "$ROOT" -B "$sanitizer_config_dir" \
    -DCMAKE_BUILD_TYPE=Debug -DTIPE_ENABLE_SANITIZERS=ON 2>&1)
sanitizer_config_status=$?
set -e
if (( sanitizer_config_status == 0 )); then
    if ! grep -q '^TIPE_ENABLE_SANITIZERS:BOOL=ON$' "$sanitizer_config_dir/CMakeCache.txt"; then
        echo "sanitizer CMake option should remain enabled after a successful probe" >&2
        exit 1
    fi
elif [[ "$sanitizer_config_output" != *"TiPE sanitizer runtime probe failed"* ]]; then
    echo "sanitizer configuration should succeed or explain that compiler-matched runtimes are unavailable" >&2
    exit 1
fi
if env -u HOME "$ROOT/scripts/install.sh" >/dev/null 2>"$home_error"; then
    echo "install helper should reject a missing HOME with a clear message" >&2
    exit 1
fi
if ! grep -qF "HOME is not set; TiPE user install paths are unavailable" "$home_error"; then
    echo "install helper should explain why HOME is required" >&2
    exit 1
fi
if env -u HOME "$ROOT/scripts/uninstall.sh" --dry-run >/dev/null 2>"$home_error"; then
    echo "uninstall helper should reject a missing HOME with a clear message" >&2
    exit 1
fi
if ! grep -qF "HOME is not set; TiPE user install paths are unavailable" "$home_error"; then
    echo "uninstall helper should explain why HOME is required" >&2
    exit 1
fi
if env -u HOME "$ROOT/scripts/restart-fcitx5.sh" --dry-run >/dev/null 2>"$home_error"; then
    echo "restart helper should reject a missing HOME with a clear message" >&2
    exit 1
fi
if ! grep -qF "HOME is not set; cannot restart fcitx5 for the TiPE user session" "$home_error"; then
    echo "restart helper should explain why HOME is required" >&2
    exit 1
fi
if env -u HOME "$ROOT/scripts/doctor.sh" --no-runtime >/dev/null 2>"$home_error"; then
    echo "doctor helper should reject a missing HOME with a clear message" >&2
    exit 1
fi
if ! grep -qF "HOME is not set; TiPE user paths cannot be inspected" "$home_error"; then
    echo "doctor helper should explain why HOME is required" >&2
    exit 1
fi
if env -u HOME "$ROOT/scripts/model-config.sh" --show >/dev/null 2>"$home_error"; then
    echo "model config helper should reject a missing HOME with a clear message" >&2
    exit 1
fi
if ! grep -qF "HOME is not set; TiPE model configuration paths are unavailable" "$home_error"; then
    echo "model config helper should explain why HOME is required" >&2
    exit 1
fi
if env -u HOME "$ROOT/scripts/model-current.sh" >/dev/null 2>"$home_error"; then
    echo "model current helper should reject a missing HOME with a clear message" >&2
    exit 1
fi
if ! grep -qF "HOME is not set; TiPE model configuration is unavailable" "$home_error"; then
    echo "model current helper should explain why HOME is required" >&2
    exit 1
fi

printf 'english\ttest-request\n' >"$doctor_default_runtime/tipe/input-mode"
printf 'english\ttest-request\n' >"$doctor_default_runtime/tipe/input-mode-applied"
doctor_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    XDG_RUNTIME_DIR="$doctor_default_runtime" \
    TIPE_MODEL_CONFIG="$doctor_default_model_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_output" != *$'section\tenvironment\t'* ||
    "$doctor_output" != *$'section\tinstall\t'* ||
    "$doctor_output" != *$'section\tdata\t'* ||
    "$doctor_output" != *$'section\tlogs\t'* ||
    "$doctor_output" == *$'section\truntime\t'* ||
    "$doctor_output" != *$'env\tTIPE_MODEL_BACKEND\theuristic'* ||
    "$doctor_output" != *$'env\tTIPE_DEBUG\t0'* ||
    "$doctor_output" != *$'env\tTIPE_CANDIDATE_DEBUG\t0'* ||
    "$doctor_output" != *$'input-mode\tcurrent\tenglish'* ||
    "$doctor_output" != *$'ok\tinput-mode-applied\tenglish request acknowledged'* ||
    "$doctor_output" != *$'path\tstate-probe\t'* ||
    "$doctor_output" != *$'path\tmodel-explain\t'* ||
    "$doctor_output" != *$'\tlearning-panel\t'* ||
    "$doctor_output" != *$'\tsupervision-window\t'* ||
    "$doctor_output" != *$'model\tlive-supervision\t'*"supervision-current.tsv"* ||
    "$doctor_output" != *$'live-supervision\t'*"supervision-current.tsv"* ||
    "$doctor_output" != *$'\tanalyze-window\t'* ||
    "$doctor_output" != *$'model\tself-test-command\t'*"tipe-model-self-test --current --config"* ||
    "$doctor_output" != *$'model\tdry-run-test-command\t'*"tipe-model-self-test --current --config"* ||
    "$doctor_output" != *$'path\tmodel-self-test\t'* ||
    "$doctor_output" != *$'\tapp-icon\t'* ||
    "$doctor_output" != *$'\tapp-icon-wm-class\t'* ]]; then
    echo "doctor helper should print read-only TiPE environment, install, data, and log status" >&2
    exit 1
fi
doctor_niri_bad="$tmp_dir/doctor-niri-bad.kdl"
printf '%s\n' 'Mod+Space { spawn-sh "fcitx5-remote -t"; }' >"$doctor_niri_bad"
doctor_fcitx_bad="$tmp_dir/doctor-fcitx-bad.conf"
printf '%s\n' \
    '[Hotkey]' \
    'EnumerateWithTriggerKeys=True' \
    '[Hotkey/TriggerKeys]' \
    '0=Control+space' \
    '[Hotkey/AltTriggerKeys]' \
    '0=Shift_L' \
    '[Hotkey/EnumerateGroupForwardKeys]' \
    '0=Super+space' \
    '[Hotkey/EnumerateGroupBackwardKeys]' \
    '0=Shift+Super+space' >"$doctor_fcitx_bad"
doctor_niri_bad_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$doctor_default_model_config" TIPE_NIRI_KEYBINDS="$doctor_niri_bad" \
    TIPE_FCITX5_CONFIG="$doctor_fcitx_bad" \
    "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_niri_bad_output" != *$'section\tintegration\t'* ||
    "$doctor_niri_bad_output" != *$'warn\tniri-mode-toggle\tMod+Space deactivates fcitx5'* ||
    "$doctor_niri_bad_output" != *$'warn\tfcitx5-toggle-conflicts\t'*'Ctrl+Space trigger'* ||
    "$doctor_niri_bad_output" != *'bare Shift trigger'* ||
    "$doctor_niri_bad_output" != *'Super+Space group switch'* ]]; then
    echo "doctor helper should reject compositor and fcitx5 toggles that bypass English supervision" >&2
    exit 1
fi
doctor_niri_good="$tmp_dir/doctor-niri-good.kdl"
printf '%s\n' \
    'Mod+Space { spawn-sh "$HOME/.local/bin/tipe-toggle"; }' \
    'Mod+Shift+Space { spawn-sh "$HOME/.local/bin/tipe-toggle"; }' \
    'Ctrl+Space { spawn-sh "$HOME/.local/bin/tipe-toggle"; }' >"$doctor_niri_good"
doctor_fcitx_good="$tmp_dir/doctor-fcitx-good.conf"
printf '%s\n' \
    '[Hotkey]' \
    'EnumerateWithTriggerKeys=False' \
    '[Hotkey/TriggerKeys]' \
    '[Hotkey/AltTriggerKeys]' \
    '[Hotkey/EnumerateGroupForwardKeys]' \
    '[Hotkey/EnumerateGroupBackwardKeys]' >"$doctor_fcitx_good"
doctor_niri_good_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$doctor_default_model_config" TIPE_NIRI_KEYBINDS="$doctor_niri_good" \
    TIPE_FCITX5_CONFIG="$doctor_fcitx_good" \
    "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_niri_good_output" != *$'ok\tniri-mode-toggle\tMod+Space uses tipe-toggle'* ||
    "$doctor_niri_good_output" != *$'ok\tniri-control-toggle\tCtrl+Space uses tipe-toggle'* ||
    "$doctor_niri_good_output" != *$'ok\tfcitx5-toggle-conflicts\tno common fcitx5 shortcut bypasses tipe-toggle'* ||
    "$doctor_niri_good_output" == *$'warn\tniri-mode-toggle\t'* ||
    "$doctor_niri_good_output" == *$'warn\tfcitx5-toggle-conflicts\t'* ]]; then
    echo "doctor helper should accept unified TiPE internal-mode shortcuts" >&2
    exit 1
fi
doctor_ui_only_cache="$tmp_dir/doctor-ui-only-cache"
mkdir -p "$doctor_ui_only_cache/tipe"
printf '%s\n' 'Loaded addon tipeui' >"$doctor_ui_only_cache/tipe/fcitx5.log"
doctor_ui_only_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_ui_only_cache" \
    TIPE_MODEL_CONFIG="$doctor_default_model_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_ui_only_output" != *$'warn\tdirect-fcitx5-log\tLoaded addon tipe not found'* ||
    "$doctor_ui_only_output" != *$'ok\tdirect-fcitx5-log\tLoaded addon tipeui found'* ]]; then
    echo "doctor helper must not mistake the tipeui addon for the TiPE engine addon" >&2
    exit 1
fi
doctor_missing_data_output=$(XDG_DATA_HOME="$tmp_dir/doctor-missing-data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$doctor_default_model_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_missing_data_output" != *$'skip\tuser-dictionary\t'*"/doctor-missing-data/tipe/user-dictionary.tsv (optional user dictionary)"* ||
    "$doctor_missing_data_output" != *$'skip\tuser-dictionary\tnot found; validation skipped'* ||
    "$doctor_missing_data_output" == *$'ok\tuser-dictionary\t'* ||
    "$doctor_missing_data_output" != *$'skip\tpreferences\t'*"/doctor-missing-data/tipe/candidate-preferences.tsv (created after learning)"* ||
    "$doctor_missing_data_output" != *$'skip\tpreferences\tnot found; validation skipped'* ||
    "$doctor_missing_data_output" == *$'ok\tpreferences\t'* ]]; then
    echo "doctor helper should report missing optional data files as skipped, not missing or validated ok" >&2
    exit 1
fi
doctor_history_cache="$tmp_dir/doctor-history-cache"
mkdir -p "$doctor_history_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t101\tprogram\tTerm\tpreedit\t\tcandidates\t0\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\t'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t2\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t202\tprogram\tEditor\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t5\tcorrection_events\t1'
} >"$doctor_history_cache/tipe/supervision-history.tsv"
{
    printf '%s\n' $'---\tunix_ms\t202\tprogram\tEditor\tpreedit\tnihao\tcandidates\t2\texpanded\t1\tterminal\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t5\tcorrection_events\t1'
    printf '%s\n' $'events\tletter:n\tletter:i\tcandidate-selected:你好'
} >"$doctor_history_cache/tipe/supervision-training-history.tsv"
doctor_history_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_history_cache" \
    TIPE_MODEL_CONFIG="$doctor_default_model_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_history_output" != *$'supervision-history\tsize-bytes\t'* ||
    "$doctor_history_output" != *$'supervision-history\ttrim-limit-bytes\t262144'* ||
    "$doctor_history_output" != *$'supervision-history\trecords\t2'* ||
    "$doctor_history_output" != *$'supervision-history\tvalid-requests\t2'* ||
    "$doctor_history_output" != *$'supervision-history\tactive-records\t1'* ||
    "$doctor_history_output" != *$'supervision-history\tpass-through-records\t1'* ||
    "$doctor_history_output" != *$'supervision-history\tlatest-unix-ms\t202'* ||
    "$doctor_history_output" != *$'supervision-history\tlatest-program\tEditor'* ||
    "$doctor_history_output" != *$'supervision-history\tlatest-preedit\tnihao'* ||
    "$doctor_history_output" != *$'supervision-history\tlatest-candidates\t2'* ||
    "$doctor_history_output" != *$'supervision-history\tlatest-expanded\t1'* ||
    "$doctor_history_output" != *$'supervision-training-history\ttrim-limit-bytes\t1048576'* ||
    "$doctor_history_output" != *$'supervision-training-history\trecords\t1'* ||
    "$doctor_history_output" != *$'supervision-training-history\tvalid-requests\t1'* ||
    "$doctor_history_output" != *$'supervision-training-history\tactive-records\t1'* ||
    "$doctor_history_output" != *$'supervision-training-history\tpass-through-records\t0'* ||
    "$doctor_history_output" != *$'supervision-training-history\tlatest-preedit\tnihao'* ]]; then
    echo "doctor helper should summarize bounded supervision history health" >&2
    exit 1
fi
doctor_empty_latest_history_cache="$tmp_dir/doctor-empty-latest-history-cache"
mkdir -p "$doctor_empty_latest_history_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t101\tprogram\tEditor\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t5\tcorrection_events\t1'
    printf '%s\n' $'---\tunix_ms\t202\tprogram\tTerm\tpreedit\t\tcandidates\t0\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\t'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t2\tcorrection_events\t0'
} >"$doctor_empty_latest_history_cache/tipe/supervision-history.tsv"
doctor_empty_latest_history_output=$(XDG_DATA_HOME="$doctor_default_data" \
    XDG_CACHE_HOME="$doctor_empty_latest_history_cache" TIPE_MODEL_CONFIG="$doctor_default_model_config" \
    "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_empty_latest_history_output" != *$'supervision-history\tlatest-unix-ms\t202'* ||
    "$doctor_empty_latest_history_output" != *$'supervision-history\tlatest-program\tTerm'* ||
    "$doctor_empty_latest_history_output" != *$'supervision-history\tlatest-preedit\t\n'* ||
    "$doctor_empty_latest_history_output" != *$'supervision-history\tlatest-candidates\t0'* ||
    "$doctor_empty_latest_history_output" != *$'supervision-history\tlatest-expanded\t0'* ]]; then
    echo "doctor helper should preserve empty latest preedit fields in supervision history headers" >&2
    exit 1
fi
doctor_bad_history_cache="$tmp_dir/doctor-bad-history-cache"
mkdir -p "$doctor_bad_history_cache/tipe"
printf '%s\n' $'protocol\t1' $'preedit\torphan' >"$doctor_bad_history_cache/tipe/supervision-history.tsv"
doctor_bad_history_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_bad_history_cache" \
    TIPE_MODEL_CONFIG="$doctor_default_model_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_bad_history_output" != *$'supervision-history\trecords\t0'* ||
    "$doctor_bad_history_output" != *$'supervision-history\tvalid-requests\t0'* ||
    "$doctor_bad_history_output" != *$'warn\tsupervision-history\tfirst non-empty row is not a history record header'* ]]; then
    echo "doctor helper should warn when supervision history is malformed or partially trimmed" >&2
    exit 1
fi
doctor_learning_data="$tmp_dir/doctor-learning-data"
mkdir -p "$doctor_learning_data/tipe"
{
    printf '%s\n' $'nihao\t你号\t4'
    printf '%s\n' $'__raw_token__\tto\t3'
    printf '%s\n' $'__correction__\tihao\tnihao\t3'
    printf '%s\n' $'__segment_chain__\twoc\two\t我\tc\twocao\t我操\t2'
} >"$doctor_learning_data/tipe/candidate-preferences.tsv"
doctor_learning_output=$(XDG_DATA_HOME="$doctor_learning_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$doctor_default_model_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_learning_output" != *$'section\tlearning\t'* ||
    "$doctor_learning_output" != *$'learning\tpreferences\trows=1 total=4'* ||
    "$doctor_learning_output" != *$'learning\tpreference-evidence\tactive=1 inactive=0'* ||
    "$doctor_learning_output" != *$'learning\tsupervised-raw-tokens\trows=1 total=3'* ||
    "$doctor_learning_output" != *$'learning\tcorrections\trows=1 total=3'* ||
    "$doctor_learning_output" != *$'learning\tsegment-chains\trows=1 total=2'* ||
    "$doctor_learning_output" != *$'learning\ttop-preference-1\tnihao -> 你号 count=4'* ||
    "$doctor_learning_output" != *$'learning\ttop-supervised-raw-token-1\tto count=3'* ||
    "$doctor_learning_output" != *$'learning\ttop-correction-1\tihao -> nihao count=3'* ||
    "$doctor_learning_output" != *$'learning\ttop-segment-chain-1\twoc -> 我操 count=2'* ]]; then
    echo "doctor helper should summarize learned TiPE preferences for debugging" >&2
    exit 1
fi
doctor_fake_bin="$tmp_dir/doctor-fake-bin"
mkdir -p "$doctor_fake_bin"
doctor_runtime_model_config="$tmp_dir/doctor-runtime-model-env"
doctor_runtime_data_home="$tmp_dir/doctor-runtime-data"
doctor_runtime_cache_home="$tmp_dir/doctor-runtime-cache"
mkdir -p "$doctor_runtime_data_home" "$doctor_runtime_cache_home"
start_doctor_runtime "$HOME/.local/bin/tipe-model-current" "$doctor_runtime_model_config" custom heuristic
doctor_runtime_pid="$doctor_started_pid"
cat >"$doctor_fake_bin/pgrep" <<'PGREP'
#!/usr/bin/env bash
if [[ "$*" == "-x fcitx5" ]]; then
    printf '%s\n' "$TIPE_DOCTOR_FAKE_FCITX5_PID"
    exit 0
fi
exit 1
PGREP
cat >"$doctor_fake_bin/ps" <<'PS'
#!/usr/bin/env bash
printf '  %s fcitx5 fcitx5 -r --ui tipeui\n' "${@: -1}"
PS
cat >"$doctor_fake_bin/fcitx5-remote" <<'REMOTE'
#!/usr/bin/env bash
if [[ "${1:-}" == "-n" ]]; then
    printf 'tipe\n'
    exit 0
fi
exit 1
REMOTE
cat >"$doctor_fake_bin/systemctl" <<'SYSTEMCTL'
#!/usr/bin/env bash
if [[ "$*" == "--user is-active --quiet fcitx5.service" ]]; then
    exit 0
fi
exit 1
SYSTEMCTL
cat >"$doctor_fake_bin/journalctl" <<'JOURNALCTL'
#!/usr/bin/env bash
printf '%s\n' 'Loaded addon tipe' 'Loaded addon tipeui'
JOURNALCTL
chmod +x "$doctor_fake_bin/pgrep" "$doctor_fake_bin/ps" "$doctor_fake_bin/fcitx5-remote" \
    "$doctor_fake_bin/systemctl" "$doctor_fake_bin/journalctl"
doctor_remote_called="$tmp_dir/doctor-remote-called"
cat >"$doctor_fake_bin/busctl" <<'BUSCTL'
#!/usr/bin/env bash
printf 'fake session bus unavailable\n' >&2
exit 23
BUSCTL
cat >"$doctor_fake_bin/fcitx5-remote" <<'REMOTE'
#!/usr/bin/env bash
printf 'called\n' >"$TIPE_DOCTOR_REMOTE_CALLED"
exit 99
REMOTE
chmod +x "$doctor_fake_bin/busctl" "$doctor_fake_bin/fcitx5-remote"
doctor_unreachable_dbus_output=$(
    PATH="$doctor_fake_bin:$PATH" XDG_DATA_HOME="$doctor_runtime_data_home" \
        XDG_CACHE_HOME="$doctor_runtime_cache_home" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/fake-bus \
        TIPE_MODEL_CONFIG="$doctor_runtime_model_config" TIPE_DOCTOR_REMOTE_CALLED="$doctor_remote_called" \
        "$ROOT/scripts/doctor.sh"
)
if [[ -e "$doctor_remote_called" ||
    "$doctor_unreachable_dbus_output" != *$'skip\tcurrent-input-method\tfcitx5-remote -n failed: session D-Bus or fcitx5 service is not reachable: fake session bus unavailable'* ]]; then
    echo "doctor helper should preflight D-Bus without launching an abort-prone fcitx5-remote" >&2
    exit 1
fi
cat >"$doctor_fake_bin/fcitx5-remote" <<'REMOTE'
#!/usr/bin/env bash
if [[ "${1:-}" == "-n" ]]; then
    printf 'tipe\n'
    exit 0
fi
exit 1
REMOTE
chmod +x "$doctor_fake_bin/fcitx5-remote"
doctor_runtime_output=$(
    PATH="$doctor_fake_bin:$PATH" XDG_DATA_HOME="$doctor_runtime_data_home" \
        XDG_CACHE_HOME="$doctor_runtime_cache_home" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/fake-bus \
        TIPE_MODEL_CONFIG="$doctor_runtime_model_config" \
        TIPE_DOCTOR_FAKE_FCITX5_PID="$doctor_runtime_pid" "$ROOT/scripts/doctor.sh"
)
if [[ "$doctor_runtime_output" != *$'section\truntime\t'* ||
    "$doctor_runtime_output" != *$'runtime\tfcitx5-log-source\tuser-journal'* ||
    "$doctor_runtime_output" != *$'ok\tfcitx5-journal\tLoaded addon tipe found'* ||
    "$doctor_runtime_output" != *$'ok\tfcitx5-journal\tLoaded addon tipeui found'* ||
    "$doctor_runtime_output" != *$'runtime\tcurrent-input-method\ttipe'* ||
    "$doctor_runtime_output" != *$'ok\truntime\tTiPE is the current input method'* ||
    "$doctor_runtime_output" != *$'runtime\tmodel-command\t'*":$HOME/.local/bin/tipe-model-current"* ||
    "$doctor_runtime_output" != *$'runtime\tmodel-config\t'*":$doctor_runtime_model_config"* ||
    "$doctor_runtime_output" != *$'runtime\tmodel-mode\t'*":custom"* ||
    "$doctor_runtime_output" != *$'runtime\tmodel-backend\t'*":heuristic"* ||
    "$doctor_runtime_output" != *$'runtime\tmodel-config-active\t'*":1"* ||
    "$doctor_runtime_output" != *$'runtime\tmodel-config-path-active\t'*":1"* ]]; then
    echo "doctor helper should report runtime fcitx5 model environment when process checks are available" >&2
    exit 1
fi
cat >"$doctor_fake_bin/fcitx5-remote" <<'REMOTE'
#!/usr/bin/env bash
if [[ "${1:-}" == "-n" ]]; then
    printf 'pinyin\n'
    exit 0
fi
exit 1
REMOTE
chmod +x "$doctor_fake_bin/fcitx5-remote"
doctor_runtime_pinyin_output=$(
    PATH="$doctor_fake_bin:$PATH" XDG_DATA_HOME="$doctor_runtime_data_home" \
        XDG_CACHE_HOME="$doctor_runtime_cache_home" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/fake-bus \
        TIPE_MODEL_CONFIG="$doctor_runtime_model_config" \
        TIPE_DOCTOR_FAKE_FCITX5_PID="$doctor_runtime_pid" "$ROOT/scripts/doctor.sh"
)
if [[ "$doctor_runtime_pinyin_output" != *$'runtime\tcurrent-input-method\tpinyin'* ||
    "$doctor_runtime_pinyin_output" != *$'warn\truntime\tcurrent input method is pinyin, not tipe'* ]]; then
    echo "doctor helper should warn when the current input method is not TiPE" >&2
    exit 1
fi
cat >"$doctor_fake_bin/fcitx5-remote" <<'REMOTE'
#!/usr/bin/env bash
if [[ "${1:-}" == "-n" ]]; then
    printf 'fake dbus unavailable\n' >&2
    exit 7
fi
exit 1
REMOTE
chmod +x "$doctor_fake_bin/fcitx5-remote"
doctor_runtime_remote_error_output=$(
    PATH="$doctor_fake_bin:$PATH" XDG_DATA_HOME="$doctor_runtime_data_home" \
        XDG_CACHE_HOME="$doctor_runtime_cache_home" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/fake-bus \
        TIPE_MODEL_CONFIG="$doctor_runtime_model_config" \
        TIPE_DOCTOR_FAKE_FCITX5_PID="$doctor_runtime_pid" "$ROOT/scripts/doctor.sh"
)
if [[ "$doctor_runtime_remote_error_output" != *$'skip\tcurrent-input-method\tfcitx5-remote -n failed: fake dbus unavailable'* ]]; then
    echo "doctor helper should report the fcitx5-remote failure reason" >&2
    exit 1
fi
cat >"$doctor_fake_bin/fcitx5-remote" <<'REMOTE'
#!/usr/bin/env bash
if [[ "${1:-}" == "-n" ]]; then
    printf 'tipe\n'
    exit 0
fi
exit 1
REMOTE
chmod +x "$doctor_fake_bin/fcitx5-remote"
stop_doctor_runtime "$doctor_runtime_pid"
start_doctor_runtime "$ROOT/scripts/model-current.sh" "$doctor_runtime_model_config" custom heuristic
doctor_source_runtime_pid="$doctor_started_pid"
doctor_source_runtime_output=$(
    PATH="$doctor_fake_bin:$PATH" XDG_DATA_HOME="$doctor_runtime_data_home" \
        XDG_CACHE_HOME="$doctor_runtime_cache_home" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/fake-bus \
        TIPE_MODEL_CONFIG="$doctor_runtime_model_config" \
        TIPE_DOCTOR_FAKE_FCITX5_PID="$doctor_source_runtime_pid" "$ROOT/scripts/doctor.sh"
)
if [[ "$doctor_source_runtime_output" != *$'runtime\tmodel-command\t'*":$ROOT/scripts/model-current.sh"* ||
    "$doctor_source_runtime_output" != *$'runtime\tmodel-config-active\t'*":1"* ||
    "$doctor_source_runtime_output" != *$'runtime\tmodel-config-path-active\t'*":1"* ]]; then
    echo "doctor helper should recognize source-tree model-current runtime commands" >&2
    exit 1
fi
stop_doctor_runtime "$doctor_source_runtime_pid"
doctor_mismatch_model_config="$tmp_dir/doctor-runtime-other-model-env"
start_doctor_runtime "$HOME/.local/bin/tipe-model-current" "$doctor_mismatch_model_config" heuristic heuristic
doctor_mismatch_runtime_pid="$doctor_started_pid"
doctor_mismatch_runtime_output=$(
    PATH="$doctor_fake_bin:$PATH" XDG_DATA_HOME="$doctor_runtime_data_home" \
        XDG_CACHE_HOME="$doctor_runtime_cache_home" DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/fake-bus \
        TIPE_MODEL_CONFIG="$doctor_runtime_model_config" \
        TIPE_DOCTOR_FAKE_FCITX5_PID="$doctor_mismatch_runtime_pid" "$ROOT/scripts/doctor.sh"
)
if [[ "$doctor_mismatch_runtime_output" != *$'runtime\tmodel-config-active\t'*":1"* ||
    "$doctor_mismatch_runtime_output" != *$'runtime\tmodel-config-path-active\t'*":0:$doctor_mismatch_model_config"* ]]; then
    echo "doctor helper should distinguish model-current from the exact runtime model config path" >&2
    exit 1
fi
kill "$doctor_mismatch_runtime_pid" 2>/dev/null || true

restart_dry_run=$("$ROOT/scripts/restart-fcitx5.sh" --dry-run --model-example --debug)
if [[ "$restart_dry_run" != *"Dry run only; fcitx5 will not be restarted"* ||
    "$restart_dry_run" != *"TIPE_MODEL_COMMAND="* ||
    "$restart_dry_run" != *"TIPE_DEBUG=1"* ||
    "$restart_dry_run" != *"TIPE_WAYLAND_POPUP_EDGE_FALLBACK=0"* ||
    "$restart_dry_run" != *"TIPE_STATUS_EDGE_FALLBACK=0"* ||
    "$restart_dry_run" != *"direct fallback fcitx5 log:"*"/fcitx5.log"* ||
    "$restart_dry_run" != *"user service fcitx5 log: journalctl --user -u fcitx5.service"* ||
    "$restart_dry_run" != *"live supervision reset:"*"/supervision-current.tsv"* ||
    "$restart_dry_run" != *"direct launch preflight:"*"D-Bus"*"Wayland"* ||
    "$restart_dry_run" != *"fcitx5 -r --verbose default=5"* ]]; then
    echo "restart helper dry-run should show planned command without changing fcitx5" >&2
    exit 1
fi
if [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    if [[ "$restart_dry_run" != *"planned switch: fcitx5-remote -o; fcitx5-remote -s tipe; verify with fcitx5-remote -n, retry, settle-check, then final fcitx5-remote -s tipe"* ]]; then
        echo "restart helper dry-run should show planned fcitx5-remote switch when DBus is available" >&2
        exit 1
    fi
else
    if [[ "$restart_dry_run" != *"planned switch: skipped because DBUS_SESSION_BUS_ADDRESS is not set"* ]]; then
        echo "restart helper dry-run should explain skipped fcitx5-remote switch without DBus" >&2
        exit 1
    fi
fi
restart_ui_dry_run=$("$ROOT/scripts/restart-fcitx5.sh" --dry-run --ui tipeui)
if [[ "$restart_ui_dry_run" != *"--ui tipeui"* ||
    "$restart_ui_dry_run" != *"TIPE_MODEL_COMMAND="*"tipe-model-current"* ||
    "$restart_ui_dry_run" != *"TIPE_MODEL_CONFIG="*"/tipe/model-env"* ]]; then
    echo "restart helper should load the current model by default and include the requested UI addon" >&2
    exit 1
fi
restart_current_dry_run=$("$ROOT/scripts/restart-fcitx5.sh" --dry-run --model-current)
if [[ "$restart_current_dry_run" != *"TIPE_MODEL_COMMAND="*"tipe-model-current"* ||
    "$restart_current_dry_run" != *"TIPE_MODEL_CONFIG="*"/tipe/model-env"* ||
    "$restart_current_dry_run" != *"model config:"*"/tipe/model-env"* ||
    "$restart_current_dry_run" != *"model config status:"* ||
    "$restart_current_dry_run" != *"preferred launch:"*"systemd"* ||
    "$restart_current_dry_run" != *"planned verification: tipe-doctor"* ]]; then
    echo "restart helper dry-run should support the reusable model-current wrapper" >&2
    exit 1
fi
restart_no_model_dry_run=$("$ROOT/scripts/restart-fcitx5.sh" --dry-run --no-model)
if [[ "$restart_no_model_dry_run" == *"TIPE_MODEL_COMMAND="* ||
    "$restart_no_model_dry_run" == *"TIPE_MODEL_CONFIG="* ]]; then
    echo "restart helper --no-model should explicitly omit the model environment" >&2
    exit 1
fi
if "$ROOT/scripts/restart-fcitx5.sh" --dry-run --ui '../bad' >/dev/null 2>&1; then
    echo "restart helper should reject unsafe UI addon names" >&2
    exit 1
fi
if TIPE_WAYLAND_POPUP_EDGE_FALLBACK=invalid "$ROOT/scripts/restart-fcitx5.sh" --dry-run >/dev/null 2>&1; then
    echo "restart helper should reject an invalid candidate edge fallback setting" >&2
    exit 1
fi
if "$ROOT/scripts/restart-fcitx5.sh" --dry-run --model-example --model-adapter >/dev/null 2>&1; then
    echo "restart helper should reject conflicting model wrapper options" >&2
    exit 1
fi
if "$ROOT/scripts/restart-fcitx5.sh" --dry-run --model-current --model-adapter >/dev/null 2>&1; then
    echo "restart helper should reject conflicting current and adapter model wrapper options" >&2
    exit 1
fi
if "$ROOT/scripts/restart-fcitx5.sh" --dry-run --no-model --model-current >/dev/null 2>&1; then
    echo "restart helper should reject conflicting disabled and current model options" >&2
    exit 1
fi
if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    no_dbus_restart_error=$(mktemp)
    if "$ROOT/scripts/restart-fcitx5.sh" >/dev/null 2>"$no_dbus_restart_error"; then
        echo "restart helper should refuse to restart fcitx5 without a session DBus" >&2
        exit 1
    fi
    if ! grep -qF "DBUS_SESSION_BUS_ADDRESS is not set; refusing to restart fcitx5" "$no_dbus_restart_error"; then
        echo "restart helper should explain why it refused to restart without DBus" >&2
        exit 1
    fi
    rm -f "$no_dbus_restart_error"
fi

uninstall_dry_run=$("$ROOT/scripts/uninstall.sh" --dry-run)
if [[ "$uninstall_dry_run" != *$'present\t'"$HOME/.local/lib64/fcitx5/libtipe.so"* ||
    "$uninstall_dry_run" != *$'present\t'"$HOME/.local/lib64/fcitx5/libtipeui.so"* ||
    "$uninstall_dry_run" != *$'present\t'"$HOME/.local/bin/tipe-state-probe"* ||
    "$uninstall_dry_run" != *$'\t'"$HOME/.local/share/applications/tipe-supervision.desktop"* ||
    "$uninstall_dry_run" != *$'\t'"$HOME/.local/share/icons/hicolor/scalable/apps/tipe.svg"* ||
    "$uninstall_dry_run" != *$'\t'"$HOME/.local/share/icons/hicolor/scalable/apps/dev.tipe.LearningPanel.svg"* ||
    "$uninstall_dry_run" != *$'\t'"$HOME/.local/share/tipe/support/wechat.png"* ||
    "$uninstall_dry_run" != *$'\t'"$HOME/.local/share/tipe/support/alipay.png"* ||
    "$uninstall_dry_run" != *"Dry run only; no TiPE user install files were removed."* ]]; then
    echo "uninstall dry-run output did not include expected TiPE install files" >&2
    exit 1
fi
uninstall_niri_keybinds="$tmp_dir/uninstall-niri-keybinds.kdl"
printf '%s\n' 'Mod+Space { spawn-sh "$HOME/.local/bin/tipe-toggle"; }' >"$uninstall_niri_keybinds"
uninstall_niri_output=$(TIPE_NIRI_KEYBINDS="$uninstall_niri_keybinds" "$ROOT/scripts/uninstall.sh" --dry-run)
if [[ "$uninstall_niri_output" != *$'warning\t'*"still calls tipe-toggle"*"stop working"* ]]; then
    echo "uninstall dry-run should warn about a niri shortcut that will call a removed helper" >&2
    exit 1
fi
fake_home="$tmp_dir/fake-home"
mkdir -p "$fake_home"
fake_home_uninstall_dry_run=$(HOME="$fake_home" "$ROOT/scripts/uninstall.sh" --dry-run)
if [[ "$fake_home_uninstall_dry_run" != *$'missing\t'"$fake_home/.local/lib64/fcitx5/libtipe.so"* ||
    "$fake_home_uninstall_dry_run" == *$HOME/.local* ]]; then
    echo "uninstall dry-run should stay scoped to the selected HOME/.local prefix" >&2
    exit 1
fi
install_dry_home="$tmp_dir/install-dry-home"
mkdir -p "$install_dry_home"
install_dry_run=$(HOME="$install_dry_home" "$ROOT/scripts/install.sh" --dry-run)
if [[ "$install_dry_run" != *$'dry-run\tprefix\t'"$install_dry_home/.local"* ||
    "$install_dry_run" != *$'would-install\t'"$install_dry_home/.local/bin/tipe-personal-model"* ||
    "$install_dry_run" != *$'would-install\t'"$install_dry_home/.local/bin/tipe-toggle"* ||
    "$install_dry_run" != *$'would-install\t'"$install_dry_home/.local/share/fcitx5/inputmethod/tipe.conf"* ||
    "$install_dry_run" != *$'would-install\t'"$install_dry_home/.local/share/applications/tipe-supervision.desktop"* ||
    "$install_dry_run" != *$'would-install\t'"$install_dry_home/.local/share/icons/hicolor/scalable/apps/tipe.svg"* ||
    "$install_dry_run" != *$'would-install\t'"$install_dry_home/.local/share/icons/hicolor/scalable/apps/dev.tipe.LearningPanel.svg"* ||
    "$install_dry_run" != *$'would-install\t'"$install_dry_home/.local/share/tipe/support/wechat.png"* ||
    "$install_dry_run" != *$'would-install\t'"$install_dry_home/.local/share/tipe/support/alipay.png"* ||
    "$install_dry_run" != *"Dry run only; no TiPE files were installed and fcitx5 was not restarted."* ||
    "$install_dry_run" == *$HOME/.local* ||
    -n "$(find "$install_dry_home" -mindepth 1 -print -quit)" ]]; then
    echo "install dry-run should validate and list the selected HOME without writing files" >&2
    exit 1
fi
if "$ROOT/scripts/install.sh" --unknown >/dev/null 2>&1; then
    echo "install helper should reject unknown arguments" >&2
    exit 1
fi

install_failure_home="$tmp_dir/install-failure-home"
install_failure_tmp="$tmp_dir/install-failure-tmp"
install_failure_bin="$tmp_dir/install-failure-bin"
install_failure_target="$install_failure_home/.local/bin/tipe-state-probe"
install_failure_missing="$install_failure_home/.local/bin/tipe-missing-stage"
mkdir -p "$(dirname -- "$install_failure_target")" "$install_failure_tmp" "$install_failure_bin"
printf '%s\n' 'existing-live-file' >"$install_failure_target"
install_failure_inode=$(stat -c '%i' "$install_failure_target")
cat >"$install_failure_bin/cmake" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[[ "${1:-}" == "--install" ]]
mkdir -p -- "$(dirname -- "$DESTDIR$TIPE_TEST_INSTALL_TARGET")"
printf '%s\n' 'valid-staged-file' >"$DESTDIR$TIPE_TEST_INSTALL_TARGET"
printf '%s\n' "$TIPE_TEST_INSTALL_TARGET" "$TIPE_TEST_INSTALL_MISSING" >"$TIPE_TEST_INSTALL_MANIFEST"
EOF
chmod +x "$install_failure_bin/cmake"
install_failure_error="$tmp_dir/install-failure.err"
if HOME="$install_failure_home" TMPDIR="$install_failure_tmp" PATH="$install_failure_bin:$PATH" \
    TIPE_TEST_INSTALL_TARGET="$install_failure_target" \
    TIPE_TEST_INSTALL_MISSING="$install_failure_missing" \
    TIPE_TEST_INSTALL_MANIFEST="$ROOT/build/install_manifest.txt" \
    "$ROOT/scripts/install.sh" >/dev/null 2>"$install_failure_error"; then
    echo "install helper should reject an incomplete staged CMake install" >&2
    exit 1
fi
if ! grep -qF "Staged CMake install file is missing:" "$install_failure_error" ||
    [[ "$(cat "$install_failure_target")" != "existing-live-file" ]] ||
    [[ "$(stat -c '%i' "$install_failure_target")" != "$install_failure_inode" ]] ||
    [[ -n "$(find "$install_failure_home" -name '.*.tmp.*' -print -quit)" ]] ||
    [[ -n "$(find "$install_failure_tmp" -mindepth 1 -print -quit)" ]]; then
    echo "staged-install validation failure should leave live files and temporary paths untouched" >&2
    exit 1
fi

install_real_home="$tmp_dir/install-real-home"
install_real_tmp="$tmp_dir/install-real-tmp"
install_real_target="$install_real_home/.local/share/fcitx5/addon/libtipe.so"
mkdir -p "$(dirname -- "$install_real_target")" "$install_real_tmp"
printf '%s\n' 'old-shared-library' >"$install_real_target"
install_real_old_inode=$(stat -c '%i' "$install_real_target")
HOME="$install_real_home" TMPDIR="$install_real_tmp" "$ROOT/scripts/install.sh" >/dev/null
if ! cmp -s "$ROOT/build/libtipe.so" "$install_real_target" ||
    [[ "$(stat -c '%a' "$install_real_target")" != "755" ]] ||
    [[ "$(stat -c '%i' "$install_real_target")" == "$install_real_old_inode" ]]; then
    echo "real user install should atomically replace an existing shared library with mode 0755" >&2
    exit 1
fi
while IFS= read -r installed_file; do
    if [[ ! -f "$installed_file" ]]; then
        echo "real fake-home install is missing a managed file: $installed_file" >&2
        exit 1
    fi
done < <(HOME="$install_real_home" TIPE_SOURCE_ROOT="$ROOT" tipe_installed_files)
if [[ "$(stat -c '%a' "$install_real_home/.local/bin/tipe-model-current")" != "755" ]] ||
    [[ "$(stat -c '%a' "$install_real_home/.local/share/applications/tipe-supervision.desktop")" != "644" ]] ||
    [[ "$(stat -c '%a' "$install_real_home/.local/share/tipe/support/wechat.png")" != "644" ]] ||
    [[ "$(stat -c '%a' "$install_real_home/.local/share/tipe/support/alipay.png")" != "644" ]] ||
    ! cmp -s "$ROOT/data/support/wechat.png" "$install_real_home/.local/share/tipe/support/wechat.png" ||
    ! cmp -s "$ROOT/data/support/alipay.png" "$install_real_home/.local/share/tipe/support/alipay.png" ||
    ! grep -qxF "Library=$install_real_home/.local/share/fcitx5/addon/libtipe" \
        "$install_real_home/.local/share/fcitx5/addon/tipe.conf" ||
    ! grep -qxF "Library=$install_real_home/.local/share/fcitx5/addon/libtipeui" \
        "$install_real_home/.local/share/fcitx5/addon/tipeui.conf" ||
    [[ -n "$(find "$install_real_home" -name '.*.tmp.*' -print -quit)" ]] ||
    [[ -n "$(find "$install_real_tmp" -mindepth 1 -print -quit)" ]]; then
    echo "real user install should preserve modes, rewrite metadata, and clean all temporary paths" >&2
    exit 1
fi

if ! grep -qx 'Library=libtipe' "$ROOT/addon/tipe.conf"; then
    echo "addon metadata should use Library=libtipe instead of an absolute path" >&2
    exit 1
fi
if ! grep -qx 'Library=libtipeui' "$ROOT/addon/tipeui.conf"; then
    echo "ui addon metadata should use Library=libtipeui instead of an absolute path" >&2
    exit 1
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" ]]; then
if [[ -e "$HOME/.local/share/fcitx5/addon/tipe.conf" ]] &&
    ! grep -qx "Library=$HOME/.local/share/fcitx5/addon/libtipe" "$HOME/.local/share/fcitx5/addon/tipe.conf"; then
    echo "installed addon metadata should use the addon-directory libtipe path without .so; rerun ./scripts/install.sh" >&2
    exit 1
fi
if [[ -e "$HOME/.local/share/fcitx5/addon/tipeui.conf" ]] &&
    ! grep -qx "Library=$HOME/.local/share/fcitx5/addon/libtipeui" "$HOME/.local/share/fcitx5/addon/tipeui.conf"; then
    echo "installed ui addon metadata should use the addon-directory libtipeui path without .so; rerun ./scripts/install.sh" >&2
    exit 1
fi
if [[ -e "$HOME/.local/share/fcitx5/inputmethod/tipe.conf" ]] &&
    ! grep -qx 'Addon=tipe' "$HOME/.local/share/fcitx5/inputmethod/tipe.conf"; then
    echo "installed input method metadata should point to Addon=tipe" >&2
    exit 1
fi
if [[ -e "$HOME/.local/lib64/fcitx5/libtipe.so" ]]; then
    if [[ ! -e "$HOME/.local/lib/fcitx5/libtipe.so" ]]; then
        echo "compatibility copy is missing: $HOME/.local/lib/fcitx5/libtipe.so" >&2
        exit 1
    fi
    if ! cmp -s "$HOME/.local/lib64/fcitx5/libtipe.so" "$HOME/.local/lib/fcitx5/libtipe.so"; then
        echo "compatibility libtipe.so copy differs from lib64 install" >&2
        exit 1
    fi
    if [[ ! -e "$HOME/.local/share/fcitx5/addon/libtipe.so" ]]; then
        echo "addon-directory compatibility copy is missing: $HOME/.local/share/fcitx5/addon/libtipe.so" >&2
        exit 1
    fi
    if ! cmp -s "$HOME/.local/lib64/fcitx5/libtipe.so" "$HOME/.local/share/fcitx5/addon/libtipe.so"; then
        echo "addon-directory libtipe.so copy differs from lib64 install" >&2
        exit 1
    fi
fi
if [[ -e "$HOME/.local/lib64/fcitx5/libtipeui.so" ]]; then
    if [[ ! -e "$HOME/.local/lib/fcitx5/libtipeui.so" ]]; then
        echo "compatibility copy is missing: $HOME/.local/lib/fcitx5/libtipeui.so" >&2
        exit 1
    fi
    if ! cmp -s "$HOME/.local/lib64/fcitx5/libtipeui.so" "$HOME/.local/lib/fcitx5/libtipeui.so"; then
        echo "compatibility libtipeui.so copy differs from lib64 install" >&2
        exit 1
    fi
    if [[ ! -e "$HOME/.local/share/fcitx5/addon/libtipeui.so" ]]; then
        echo "addon-directory compatibility copy is missing: $HOME/.local/share/fcitx5/addon/libtipeui.so" >&2
        exit 1
    fi
    if ! cmp -s "$HOME/.local/lib64/fcitx5/libtipeui.so" "$HOME/.local/share/fcitx5/addon/libtipeui.so"; then
        echo "addon-directory libtipeui.so copy differs from lib64 install" >&2
        exit 1
    fi
fi
for helper_pair in \
    "$ROOT/scripts/model-protocol-example.sh:$HOME/.local/bin/tipe-model-protocol-example" \
    "$ROOT/scripts/model-adapter.sh:$HOME/.local/bin/tipe-model-adapter" \
    "$ROOT/scripts/model-dump.sh:$HOME/.local/bin/tipe-model-dump" \
    "$ROOT/scripts/model-explain.sh:$HOME/.local/bin/tipe-model-explain" \
    "$ROOT/scripts/learning-panel.sh:$HOME/.local/bin/tipe-learning-panel" \
    "$ROOT/scripts/supervision-window.sh:$HOME/.local/bin/tipe-supervision-window" \
    "$ROOT/scripts/analyze-window.sh:$HOME/.local/bin/tipe-analyze-window" \
    "$ROOT/scripts/model-replay.sh:$HOME/.local/bin/tipe-model-replay" \
    "$ROOT/scripts/model-current.sh:$HOME/.local/bin/tipe-model-current" \
    "$ROOT/scripts/model-config.sh:$HOME/.local/bin/tipe-model-config" \
    "$ROOT/scripts/model-self-test.sh:$HOME/.local/bin/tipe-model-self-test" \
    "$ROOT/scripts/model-wrapper-new.sh:$HOME/.local/bin/tipe-model-wrapper-new" \
    "$ROOT/scripts/model-wrapper-check.sh:$HOME/.local/bin/tipe-model-wrapper-check" \
    "$ROOT/scripts/training-export.py:$HOME/.local/bin/tipe-training-export" \
    "$ROOT/scripts/personal-model.py:$HOME/.local/bin/tipe-personal-model" \
    "$ROOT/scripts/personal-model-train.sh:$HOME/.local/bin/tipe-personal-model-train" \
    "$ROOT/scripts/check-user-dictionary.sh:$HOME/.local/bin/tipe-check-user-dictionary" \
    "$ROOT/scripts/check-preferences.sh:$HOME/.local/bin/tipe-check-preferences" \
    "$ROOT/scripts/doctor.sh:$HOME/.local/bin/tipe-doctor" \
    "$ROOT/scripts/restart-fcitx5.sh:$HOME/.local/bin/tipe-restart-fcitx5" \
    "$ROOT/scripts/toggle.sh:$HOME/.local/bin/tipe-toggle"; do
    source_script="${helper_pair%%:*}"
    installed_script="${helper_pair#*:}"
    if [[ -e "$HOME/.local/share/fcitx5/addon/tipe.conf" && ! -e "$installed_script" ]]; then
        echo "installed helper script is missing: $installed_script" >&2
        exit 1
    fi
    if [[ -e "$installed_script" && ! -x "$installed_script" ]]; then
        echo "installed helper script is not executable: $installed_script" >&2
        exit 1
    fi
    if [[ -e "$installed_script" ]] && ! cmp -s "$source_script" "$installed_script"; then
        echo "installed helper script differs from source; rerun ./scripts/install.sh: $installed_script" >&2
        exit 1
    fi
done

while IFS= read -r tracked_file; do
    if [[ -e "$HOME/.local/share/fcitx5/addon/tipe.conf" && ! -e "$tracked_file" ]]; then
        echo "tracked installed file is missing: $tracked_file" >&2
        exit 1
    fi
done < <(tipe_installed_files)
fi

for legacy_binary in "$HOME/.local/bin/tipe-ctl" "$HOME/.local/bin/tipe-engine" "$HOME/.local/bin/tipe-gui" "$HOME/.local/bin/tipe-rerank-test"; do
    if [[ -e "$legacy_binary" ]]; then
        echo "notice: unmanaged legacy TiPE binary is present: $legacy_binary"
    fi
done

dictionary_sample="$tmp_dir/user-dictionary.tsv"
printf 'nihao\t你好\t你号\r\n# comment\r\nzidingyi\t自定义\r\n' >"$dictionary_sample"
"$ROOT/scripts/check-user-dictionary.sh" "$dictionary_sample" >/dev/null
"$ROOT/scripts/check-user-dictionary.sh" "$ROOT/examples/user-dictionary.tsv" >/dev/null
dictionary_explain=$("$ROOT/scripts/check-user-dictionary.sh" --explain "$dictionary_sample")
if [[ "$dictionary_explain" != *$'entry\t1\tnihao\t1:你好\t2:你号'* ||
    "$dictionary_explain" != *$'entry\t3\tzidingyi\t1:自定义'* ||
    "$dictionary_explain" != *"TiPE user dictionary ok: $dictionary_sample"* ]]; then
    echo "user dictionary checker should explain candidate order" >&2
    exit 1
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" && -x "$HOME/.local/bin/tipe-check-user-dictionary" ]]; then
    "$HOME/.local/bin/tipe-check-user-dictionary" "$dictionary_sample" >/dev/null
    installed_dictionary_explain=$("$HOME/.local/bin/tipe-check-user-dictionary" --explain "$dictionary_sample")
    if [[ "$installed_dictionary_explain" != *$'entry\t1\tnihao\t1:你好\t2:你号'* ]]; then
        echo "installed user dictionary checker should explain candidate order" >&2
        exit 1
    fi
fi
dictionary_add_sample="$tmp_dir/add-user-dictionary.tsv"
"$ROOT/scripts/check-user-dictionary.sh" --add nihao 你好 你号 --path "$dictionary_add_sample" >/dev/null
"$ROOT/scripts/check-user-dictionary.sh" --add nihao 你好啊 你号 --path "$dictionary_add_sample" >/dev/null
"$ROOT/scripts/check-user-dictionary.sh" --add zidingyi 自定义 --path "$dictionary_add_sample" >/dev/null
"$ROOT/scripts/check-user-dictionary.sh" --add nihao 你号 --first --path "$dictionary_add_sample" >/dev/null
"$ROOT/scripts/check-user-dictionary.sh" --add nihao 你好呀 你好 --first --path "$dictionary_add_sample" >/dev/null
dictionary_add_explain=$("$ROOT/scripts/check-user-dictionary.sh" --explain "$dictionary_add_sample")
if [[ "$dictionary_add_explain" != *$'entry\t1\tnihao\t1:你好呀\t2:你好\t3:你号\t4:你好啊'* ||
    "$dictionary_add_explain" != *$'entry\t2\tzidingyi\t1:自定义'* ]]; then
    echo "user dictionary checker should safely add and merge dictionary rows" >&2
    exit 1
fi
if [[ "$(stat -c '%a' "$dictionary_add_sample")" != "600" ||
    "$(stat -c '%a' "$dictionary_add_sample.lock")" != "600" ]] ||
    compgen -G "$dictionary_add_sample.tmp.*" >/dev/null; then
    echo "user dictionary updates should be private and leave no temporary files" >&2
    exit 1
fi

concurrent_dictionary_sample="$tmp_dir/concurrent-user-dictionary.tsv"
exec {concurrent_dictionary_lock_fd}>"$concurrent_dictionary_sample.lock"
flock -x "$concurrent_dictionary_lock_fd"
"$ROOT/scripts/check-user-dictionary.sh" --add bingfa 并发 --path "$concurrent_dictionary_sample" \
    >"$tmp_dir/concurrent-dictionary-a.out" 2>"$tmp_dir/concurrent-dictionary-a.err" &
concurrent_dictionary_a_pid=$!
"$ROOT/scripts/check-user-dictionary.sh" --add yuanzi 原子 --path "$concurrent_dictionary_sample" \
    >"$tmp_dir/concurrent-dictionary-b.out" 2>"$tmp_dir/concurrent-dictionary-b.err" &
concurrent_dictionary_b_pid=$!
sleep 0.1
flock -u "$concurrent_dictionary_lock_fd"
exec {concurrent_dictionary_lock_fd}>&-
concurrent_dictionary_a_status=0
concurrent_dictionary_b_status=0
wait "$concurrent_dictionary_a_pid" || concurrent_dictionary_a_status=$?
wait "$concurrent_dictionary_b_pid" || concurrent_dictionary_b_status=$?
if (( concurrent_dictionary_a_status != 0 || concurrent_dictionary_b_status != 0 )); then
    echo "concurrent user dictionary updates should both succeed" >&2
    exit 1
fi
if ! rg -q $'^bingfa\t并发$' "$concurrent_dictionary_sample" ||
    ! rg -q $'^yuanzi\t原子$' "$concurrent_dictionary_sample" ||
    compgen -G "$concurrent_dictionary_sample.tmp.*" >/dev/null; then
    echo "concurrent user dictionary updates should preserve both rows atomically" >&2
    exit 1
fi
if "$ROOT/scripts/check-user-dictionary.sh" --first "$dictionary_add_sample" >/dev/null 2>&1; then
    echo "user dictionary checker should reject --first without --add" >&2
    exit 1
fi
if "$ROOT/scripts/check-user-dictionary.sh" --add nihao --first --path "$dictionary_add_sample" >/dev/null 2>&1; then
    echo "user dictionary checker should reject --add without candidates before --first" >&2
    exit 1
fi
if "$ROOT/scripts/check-user-dictionary.sh" --add NiHao 你好 --path "$dictionary_add_sample" >/dev/null 2>&1; then
    echo "user dictionary checker should reject invalid pinyin in --add" >&2
    exit 1
fi
if "$ROOT/scripts/check-user-dictionary.sh" --add nihao $'坏\t候选' --path "$dictionary_add_sample" >/dev/null 2>&1; then
    echo "user dictionary checker should reject invalid candidates in --add" >&2
    exit 1
fi
oversized_dictionary_pinyin=$(printf 'a%.0s' {1..129})
oversized_dictionary_candidate=$(printf '词%.0s' {1..86})
if "$ROOT/scripts/check-user-dictionary.sh" --add "$oversized_dictionary_pinyin" 过长 --path "$dictionary_add_sample" \
        >/dev/null 2>&1 ||
    "$ROOT/scripts/check-user-dictionary.sh" --add nihao "$oversized_dictionary_candidate" \
        --path "$dictionary_add_sample" >/dev/null 2>&1; then
    echo "user dictionary checker should reject oversized pinyin and candidates" >&2
    exit 1
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" && -x "$HOME/.local/bin/tipe-check-user-dictionary" ]]; then
    installed_dictionary_add_sample="$tmp_dir/installed-add-user-dictionary.tsv"
    "$HOME/.local/bin/tipe-check-user-dictionary" --add ceshi 测试 --path "$installed_dictionary_add_sample" >/dev/null
    "$HOME/.local/bin/tipe-check-user-dictionary" --add ceshi 侧试 --first --path "$installed_dictionary_add_sample" >/dev/null
    installed_dictionary_add_explain=$("$HOME/.local/bin/tipe-check-user-dictionary" --explain "$installed_dictionary_add_sample")
    if [[ "$installed_dictionary_add_explain" != *$'entry\t1\tceshi\t1:侧试\t2:测试'* ]]; then
        echo "installed user dictionary checker should add dictionary rows" >&2
        exit 1
    fi
fi
probe_dictionary_output=$("$ROOT/build/tipe-state-probe" nihao --dictionary "$dictionary_sample")
if [[ "$probe_dictionary_output" != *$'candidate\t0\t你好'* || "$probe_dictionary_output" != *$'你号'* ]]; then
    echo "state probe should load an explicit user dictionary" >&2
    exit 1
fi
probe_dictionary_first_output=$("$ROOT/build/tipe-state-probe" nihao --dictionary "$dictionary_add_sample")
if [[ "$probe_dictionary_first_output" != *$'candidate\t0\t你好呀'* ||
    "$probe_dictionary_first_output" != *$'candidate\t1\t你好'* ||
    "$probe_dictionary_first_output" != *$'candidate\t2\t你号'* ]]; then
    echo "state probe should honor user dictionary --first candidate order" >&2
    exit 1
fi
probe_dictionary_new_phrase_output=$("$ROOT/build/tipe-state-probe" zidingyi --dictionary "$dictionary_sample")
if [[ "$probe_dictionary_new_phrase_output" != *$'candidate\t0\t自定义'* ]]; then
    echo "state probe should expose explicit user dictionary new phrases" >&2
    exit 1
fi
duplicate_dictionary_sample="$tmp_dir/duplicate-user-dictionary.tsv"
printf 'nihao\t你好\t你好\n' >"$duplicate_dictionary_sample"
if "$ROOT/scripts/check-user-dictionary.sh" "$duplicate_dictionary_sample" >/dev/null 2>&1; then
    echo "user dictionary checker should reject duplicate candidates" >&2
    exit 1
fi
bad_pinyin_dictionary_sample="$tmp_dir/bad-pinyin-user-dictionary.tsv"
printf 'NiHao\t你好\n' >"$bad_pinyin_dictionary_sample"
bad_pinyin_error="$tmp_dir/bad-pinyin.err"
if "$ROOT/scripts/check-user-dictionary.sh" "$bad_pinyin_dictionary_sample" >/dev/null 2>"$bad_pinyin_error"; then
    echo "user dictionary checker should reject uppercase pinyin rows" >&2
    exit 1
fi
if [[ $(wc -l <"$bad_pinyin_error") -ne 1 ]] ||
    ! grep -qF "$bad_pinyin_dictionary_sample:1: pinyin should use lowercase a-z letters only: NiHao" "$bad_pinyin_error"; then
    echo "user dictionary checker should print one path-qualified pinyin error" >&2
    exit 1
fi
empty_dictionary_sample="$tmp_dir/empty-user-dictionary.tsv"
printf 'nihao\t你好\t\n' >"$empty_dictionary_sample"
if "$ROOT/scripts/check-user-dictionary.sh" "$empty_dictionary_sample" >/dev/null 2>&1; then
    echo "user dictionary checker should reject empty trailing candidates" >&2
    exit 1
fi
printf 'nihao\t\t你好\n' >"$empty_dictionary_sample"
if "$ROOT/scripts/check-user-dictionary.sh" "$empty_dictionary_sample" >/dev/null 2>&1; then
    echo "user dictionary checker should reject empty middle candidates" >&2
    exit 1
fi

preferences_sample="$tmp_dir/candidate-preferences.tsv"
printf 'nihao\t你号\t3\nlegacy-key\t4\n__correction__\tihao\tnihao\t2\nwoc\t我操\t8\n__correction__\twoc\twocao\t7\n__correction__\tong\tnong\t3\n__segment_chain__\twoc\two\t我\tc\twocao\t我操\t6\n__correction_pattern__\tmissing\t\tn\t0\t0\t2\n__key_habit__\tmissing\t\tn\t6\n' >"$preferences_sample"
"$ROOT/scripts/check-preferences.sh" "$preferences_sample" >/dev/null
preferences_crlf_sample="$tmp_dir/candidate-preferences-crlf.tsv"
printf 'nihao\t你号\t3\r\nlegacy-key\t4\r\n__correction__\tihao\tnihao\t2\r\n__segment_chain__\twoc\two\t我\tc\twocao\t我操\t6\r\n' >"$preferences_crlf_sample"
"$ROOT/scripts/check-preferences.sh" "$preferences_crlf_sample" >/dev/null
preferences_overflow_sample="$tmp_dir/candidate-preferences-overflow.tsv"
printf 'nihao\t你号\t18446744073709551615\n' >"$preferences_overflow_sample"
preferences_overflow_error="$tmp_dir/candidate-preferences-overflow.err"
if "$ROOT/scripts/check-preferences.sh" "$preferences_overflow_sample" >/dev/null 2>"$preferences_overflow_error"; then
    echo "preference checker should reject counts above the bounded learning limit" >&2
    exit 1
fi
if ! grep -qF "$preferences_overflow_sample:1: count exceeds TiPE learning limit 1000000: 18446744073709551615" \
    "$preferences_overflow_error"; then
    echo "preference checker should explain out-of-range learning counts" >&2
    exit 1
fi
preferences_explain=$("$ROOT/scripts/check-preferences.sh" --explain "$preferences_sample")
if [[ "$preferences_explain" != *$'preference\t1\tnihao\t你号\t3'* ||
    "$preferences_explain" != *$'legacy-preference\t2\tlegacy-key\t4'* ||
    "$preferences_explain" != *$'correction\t3\tihao\tnihao\t2'* ||
    "$preferences_explain" != *$'preference\t4\twoc\t我操\t8'* ||
    "$preferences_explain" != *$'correction\t5\twoc\twocao\t7'* ||
    "$preferences_explain" != *$'correction\t6\tong\tnong\t3'* ||
    "$preferences_explain" != *$'segment-chain\t7\twoc\two\t我\tc\twocao\t我操\t6'* ||
    "$preferences_explain" != *$'runtime-correction-pattern\t8\tmissing\t\tn\t0\t0\t2'* ||
    "$preferences_explain" != *$'runtime-key-habit\t9\tmissing\t\tn\t6'* ||
    "$preferences_explain" != *"TiPE preferences ok: $preferences_sample"* ]]; then
    echo "preference checker should explain candidate preference, correction, and segment-chain rows" >&2
    exit 1
fi
preferences_summary=$("$ROOT/scripts/check-preferences.sh" --summary --top 1 "$preferences_sample")
if [[ "$preferences_summary" != *$'summary\trows\t9'* ||
    "$preferences_summary" != *$'summary\tpreferences\t2\t11'* ||
    "$preferences_summary" != *$'summary\tlegacy-preferences\t1\t4'* ||
    "$preferences_summary" != *$'summary\tcorrections\t3\t12'* ||
    "$preferences_summary" != *$'summary\tcorrection-patterns\t1\t5'* ||
    "$preferences_summary" != *$'summary\truntime-correction-patterns\t1\t2'* ||
    "$preferences_summary" != *$'summary\truntime-key-habits\t1\t6'* ||
    "$preferences_summary" != *$'summary\tsegment-chains\t1\t6'* ||
    "$preferences_summary" != *$'top-preference\t1\t8\twoc\t我操'* ||
    "$preferences_summary" != *$'top-legacy-preference\t1\t4\tlegacy-key'* ||
    "$preferences_summary" != *$'top-correction\t1\t7\twoc\twocao'* ||
    "$preferences_summary" != *$'top-correction-pattern\t1\t5\tmissing\tn\t0'* ||
    "$preferences_summary" != *$'top-runtime-correction-pattern\t1\t2\tmissing\t\tn\t0\t0'* ||
    "$preferences_summary" != *$'top-runtime-key-habit\t1\t6\tmissing\t\tn'* ||
    "$preferences_summary" != *$'top-segment-chain\t1\t6\twoc\t我操'* ]]; then
    echo "preference checker should summarize strongest learned preference, correction, and segment-chain rows" >&2
    exit 1
fi
threshold_preferences_sample="$tmp_dir/threshold-candidate-preferences.tsv"
printf '__correction_pattern__\textra\tx\t\t1\t0\t2\n__correction_pattern__\treplace\tx\ty\t1\t0\t4\n__correction_pattern__\ttranspose\tab\tba\t1\t0\t2\n__key_habit__\tmissing\t\tn\t4\n__key_habit__\textra\tx\t\t5\n__key_habit__\ttranspose\tab\tba\t3\n' >"$threshold_preferences_sample"
threshold_preferences_explain=$("$ROOT/scripts/check-preferences.sh" --explain --summary --top 10 \
    "$threshold_preferences_sample")
if [[ "$threshold_preferences_explain" != *$'runtime-correction-pattern\t1\textra\tx\t\t1\t0\t2\tinactive-evidence\trequires\t3'* ||
    "$threshold_preferences_explain" != *$'runtime-correction-pattern\t2\treplace\tx\ty\t1\t0\t4\tactive\trequires\t4'* ||
    "$threshold_preferences_explain" != *$'runtime-correction-pattern\t3\ttranspose\tab\tba\t1\t0\t2\tactive\trequires\t2'* ||
    "$threshold_preferences_explain" != *$'runtime-key-habit\t4\tmissing\t\tn\t4\tinactive-evidence\trequires\t5'* ||
    "$threshold_preferences_explain" != *$'runtime-key-habit\t5\textra\tx\t\t5\tinactive-evidence\trequires\t6'* ||
    "$threshold_preferences_explain" != *$'runtime-key-habit\t6\ttranspose\tab\tba\t3\tactive\trequires\t3'* ||
    "$threshold_preferences_explain" != *$'summary\truntime-correction-patterns\t3\t8\tactive\t2'* ||
    "$threshold_preferences_explain" != *$'summary\truntime-key-habits\t3\t12\tactive\t1'* ]]; then
    echo "preference checker should use the runtime's per-edit activation thresholds" >&2
    exit 1
fi
large_preferences_sample="$tmp_dir/large-candidate-preferences.tsv"
large_preferences_error="$tmp_dir/large-candidate-preferences.error"
: >"$large_preferences_sample"
large_preference_index=1
while (( large_preference_index <= 2500 )); do
    printf 'token%04d\t候选%04d\t%d\n' "$large_preference_index" "$large_preference_index" \
        "$((2501 - large_preference_index))" >>"$large_preferences_sample"
    large_preference_index=$((large_preference_index + 1))
done
large_preferences_summary=$(
    "$ROOT/scripts/check-preferences.sh" --summary --top 5 "$large_preferences_sample" \
        2>"$large_preferences_error"
)
if [[ "$large_preferences_summary" != *$'summary\trows\t2500'* ||
    "$large_preferences_summary" != *$'top-preference\t5\t2496\ttoken0005\t候选0005'* ||
    -s "$large_preferences_error" ]]; then
    echo "preference summary should bound displayed rows without breaking the sort pipe" >&2
    cat "$large_preferences_error" >&2
    exit 1
fi
preferences_query_woc=$("$ROOT/scripts/check-preferences.sh" --preedit woc "$preferences_sample")
if [[ "$preferences_query_woc" != *$'query-preference\t4\twoc\t我操\t8'* ||
    "$preferences_query_woc" != *$'query-effect\trank-preference\t4\twoc\t我操\t8'* ||
    "$preferences_query_woc" != *$'query-correction\t5\ttypo\twoc\twocao\t7'* ||
    "$preferences_query_woc" != *$'query-effect\tcorrection-borrow\t5\twoc\twocao\t7'* ||
    "$preferences_query_woc" != *$'query-segment-chain\t7\twoc\two\t我\tc\twocao\t我操\t6'* ||
    "$preferences_query_woc" != *$'query-effect\tsegment-chain-full\t7\twoc\two\t我\tc\twocao\t我操\t6'* ||
    "$preferences_query_woc" != *$'query-summary\twoc\tpreferences\t1\tlegacy-preferences\t0\tcorrections\t1\tsegment-chains\t1'* ]]; then
    echo "preference checker should explain rows that affect one preedit" >&2
    exit 1
fi
preferences_query_nihao=$("$ROOT/scripts/check-preferences.sh" --preedit nihao "$preferences_sample")
if [[ "$preferences_query_nihao" != *$'query-preference\t1\tnihao\t你号\t3'* ||
    "$preferences_query_nihao" != *$'query-effect\trank-preference\t1\tnihao\t你号\t3'* ||
    "$preferences_query_nihao" != *$'query-correction\t3\tcorrected\tihao\tnihao\t2'* ||
    "$preferences_query_nihao" != *$'query-effect\tcorrection-target\t3\tihao\tnihao\t2'* ||
    "$preferences_query_nihao" != *$'query-summary\tnihao\tpreferences\t1\tlegacy-preferences\t0\tcorrections\t1\tsegment-chains\t0'* ]]; then
    echo "preference checker should show corrections where the queried preedit is the corrected target" >&2
    exit 1
fi
preferences_query_nihao_fast=$("$ROOT/scripts/check-preferences.sh" --query-only --preedit nihao "$preferences_sample")
if [[ "$preferences_query_nihao_fast" != "$preferences_query_nihao" ]]; then
    echo "preference checker query-only mode should preserve current-preedit query results" >&2
    exit 1
fi
if "$ROOT/scripts/check-preferences.sh" --query-only "$preferences_sample" >/dev/null 2>&1; then
    echo "preference checker query-only mode should require a preedit" >&2
    exit 1
fi
raw_preference_classification_sample="$tmp_dir/raw-preference-classification.tsv"
printf 'nihao\tnihao\t9\nstarted\tstarted\t3\n' >"$raw_preference_classification_sample"
raw_preference_classification_summary=$(
    "$ROOT/scripts/check-preferences.sh" --summary --top 2 "$raw_preference_classification_sample"
)
if [[ "$raw_preference_classification_summary" != *$'summary\tpreference-evidence\tactive\t1\tinactive\t1'* ||
    "$raw_preference_classification_summary" != *$'top-preference\t1\t3\tstarted\tstarted'* ||
    "$raw_preference_classification_summary" != *$'top-inactive-preference\t1\t9\tnihao\tnihao'* ]]; then
    echo "preference checker should classify raw preference rows by identifier plausibility as well as count" >&2
    exit 1
fi
raw_preference_classification_query=$(
    "$ROOT/scripts/check-preferences.sh" --preedit nihao "$raw_preference_classification_sample"
)
if [[ "$raw_preference_classification_query" != *$'query-preference\t1\tnihao\tnihao\t9\tinactive-evidence'* ||
    "$raw_preference_classification_query" != *$'query-inactive-evidence\traw-preference\t1\tnihao\tnihao\t9\trequires\tenglish-identifier'* ||
    "$raw_preference_classification_query" == *$'query-effect\traw-preference'* ]]; then
    echo "preference query should not report normal pinyin as an active raw-English effect" >&2
    exit 1
fi
generic_raw_preference_sample="$tmp_dir/generic-raw-preference-classification.tsv"
printf 'goal\tgoal\t9\nok\tok\t6\nlv\tlv\t9\n' >"$generic_raw_preference_sample"
generic_raw_preference_summary=$(
    "$ROOT/scripts/check-preferences.sh" --summary --top 3 "$generic_raw_preference_sample"
)
if [[ "$generic_raw_preference_summary" != *$'summary\tpreference-evidence\tactive\t2\tinactive\t1'* ||
    "$generic_raw_preference_summary" != *$'top-preference\t1\t9\tgoal\tgoal'* ||
    "$generic_raw_preference_summary" != *$'top-preference\t2\t6\tok\tok'* ||
    "$generic_raw_preference_summary" != *$'top-inactive-preference\t1\t9\tlv\tlv'* ]]; then
    echo "preference checker should recognize generic English terminal shapes without misclassifying v-form pinyin" >&2
    exit 1
fi
supervised_raw_token_sample="$tmp_dir/supervised-raw-token.tsv"
printf '__raw_token__\tto\t3\n__raw_token__\ttooling\t2\n' >"$supervised_raw_token_sample"
supervised_raw_token_summary=$(
    "$ROOT/scripts/check-preferences.sh" --summary --top 2 "$supervised_raw_token_sample"
)
supervised_raw_token_query=$(
    "$ROOT/scripts/check-preferences.sh" --preedit to "$supervised_raw_token_sample"
)
if [[ "$supervised_raw_token_summary" != *$'summary\tsupervised-raw-tokens\t2\t5'* ||
    "$supervised_raw_token_summary" != *$'top-supervised-raw-token\t1\t3\tto'* ||
    "$supervised_raw_token_query" != *$'query-supervised-raw-token\t1\tto\t3\tactive'* ||
    "$supervised_raw_token_query" != *$'query-effect\tsupervised-raw-token\t1\tto\t3'* ||
    "$supervised_raw_token_query" != *$'supervised-raw-tokens\t1'* ]]; then
    echo "preference checker should report distilled English-mode exact-token evidence separately" >&2
    exit 1
fi
invalid_supervised_raw_token="$tmp_dir/invalid-supervised-raw-token.tsv"
printf '__raw_token__\tNiHao\t3\n' >"$invalid_supervised_raw_token"
if "$ROOT/scripts/check-preferences.sh" "$invalid_supervised_raw_token" >/dev/null 2>&1; then
    echo "preference checker should reject malformed supervised raw-token records" >&2
    exit 1
fi
preferences_query_suffix=$("$ROOT/scripts/check-preferences.sh" --preedit c "$preferences_sample")
if [[ "$preferences_query_suffix" != *$'query-segment-chain\t7\twoc\two\t我\tc\twocao\t我操\t6'* ||
    "$preferences_query_suffix" != *$'query-effect\tsegment-chain-suffix\t7\twoc\two\t我\tc\twocao\t我操\t6'* ||
    "$preferences_query_suffix" != *$'query-summary\tc\tpreferences\t0\tlegacy-preferences\t0\tcorrections\t0\tsegment-chains\t1'* ]]; then
    echo "preference checker should explain suffix effects from learned segment chains" >&2
    exit 1
fi
preferences_query_none=$("$ROOT/scripts/check-preferences.sh" --preedit meiyou "$preferences_sample")
if [[ "$preferences_query_none" != *$'query-summary\tmeiyou\tpreferences\t0\tlegacy-preferences\t0\tcorrections\t0\tsegment-chains\t0'* ]]; then
    echo "preference checker should summarize zero matches for an unknown preedit" >&2
    exit 1
fi
inactive_preferences_sample="$tmp_dir/inactive-candidate-preferences.tsv"
printf 'nihao\t你号\t1\nstarted\tstarted\t2\nlegacy-raw\t2\n' >"$inactive_preferences_sample"
inactive_preferences_explain=$("$ROOT/scripts/check-preferences.sh" --explain "$inactive_preferences_sample")
if [[ "$inactive_preferences_explain" != *$'preference\t1\tnihao\t你号\t1\tinactive-evidence'* ||
    "$inactive_preferences_explain" != *$'preference\t2\tstarted\tstarted\t2\tinactive-evidence'* ||
    "$inactive_preferences_explain" != *$'legacy-preference\t3\tlegacy-raw\t2\tinactive-evidence'* ]]; then
    echo "preference checker should label valid rows below activation thresholds as inactive evidence" >&2
    exit 1
fi
inactive_preferences_summary=$("$ROOT/scripts/check-preferences.sh" --summary --top 1 "$inactive_preferences_sample")
if [[ "$inactive_preferences_summary" != *$'summary\tpreference-evidence\tactive\t0\tinactive\t3'* ||
    "$inactive_preferences_summary" != *$'top-inactive-preference\t1\t2\tstarted\tstarted'* ||
    "$inactive_preferences_summary" != *$'top-inactive-legacy-preference\t1\t2\tlegacy-raw'* ]]; then
    echo "preference checker should summarize inactive candidate and raw evidence separately" >&2
    exit 1
fi
inactive_preferences_query=$("$ROOT/scripts/check-preferences.sh" --preedit nihao "$inactive_preferences_sample")
if [[ "$inactive_preferences_query" != *$'query-inactive-evidence\trank-preference\t1\tnihao\t你号\t1\trequires\t2'* ||
    "$inactive_preferences_query" == *$'query-effect\trank-preference\t1\tnihao\t你号\t1'* ||
    "$inactive_preferences_query" != *$'active-preferences\t0\tinactive-preferences\t1'* ]]; then
    echo "preference checker should not describe inactive evidence as a current ranking effect" >&2
    exit 1
fi
suffix_preferences_sample="$tmp_dir/suffix-candidate-preferences.tsv"
printf '__correction__\tcesh\tceshi\t2\n' >"$suffix_preferences_sample"
suffix_preferences_summary=$("$ROOT/scripts/check-preferences.sh" --summary --top 1 "$suffix_preferences_sample")
if [[ "$suffix_preferences_summary" != *$'summary\tcorrection-patterns\t1\t2'* ||
    "$suffix_preferences_summary" != *$'top-correction-pattern\t1\t2\tmissing\ti\tend-0'* ]]; then
    echo "preference checker should summarize suffix-relative correction patterns" >&2
    exit 1
fi
transpose_preferences_sample="$tmp_dir/transpose-candidate-preferences.tsv"
printf '__correction__\tgongnegn\tgongneng\t2\n' >"$transpose_preferences_sample"
transpose_preferences_summary=$("$ROOT/scripts/check-preferences.sh" --summary --top 1 "$transpose_preferences_sample")
if [[ "$transpose_preferences_summary" != *$'summary\tcorrection-patterns\t1\t2'* ||
    "$transpose_preferences_summary" != *$'top-correction-pattern\t1\t2\ttranspose\tgn->ng\tend-0'* ]]; then
    echo "preference checker should summarize adjacent-transposition correction patterns" >&2
    exit 1
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" && -x "$HOME/.local/bin/tipe-check-preferences" ]]; then
    "$HOME/.local/bin/tipe-check-preferences" "$preferences_sample" >/dev/null
    installed_preferences_explain=$("$HOME/.local/bin/tipe-check-preferences" --explain "$preferences_sample")
    if [[ "$installed_preferences_explain" != *$'preference\t1\tnihao\t你号\t3'* ||
        "$installed_preferences_explain" != *$'legacy-preference\t2\tlegacy-key\t4'* ||
        "$installed_preferences_explain" != *$'correction\t3\tihao\tnihao\t2'* ||
        "$installed_preferences_explain" != *$'preference\t4\twoc\t我操\t8'* ||
        "$installed_preferences_explain" != *$'segment-chain\t7\twoc\two\t我\tc\twocao\t我操\t6'* ||
        "$installed_preferences_explain" != *$'runtime-correction-pattern\t8\tmissing\t\tn\t0\t0\t2'* ||
        "$installed_preferences_explain" != *$'runtime-key-habit\t9\tmissing\t\tn\t6'* ]]; then
        echo "installed preference checker should explain preference rows" >&2
        exit 1
    fi
    installed_preferences_summary=$("$HOME/.local/bin/tipe-check-preferences" --summary --top 1 "$preferences_sample")
    if [[ "$installed_preferences_summary" != *$'top-preference\t1\t8\twoc\t我操'* ||
        "$installed_preferences_summary" != *$'top-correction\t1\t7\twoc\twocao'* ||
        "$installed_preferences_summary" != *$'top-runtime-correction-pattern\t1\t2\tmissing\t\tn\t0\t0'* ||
        "$installed_preferences_summary" != *$'top-runtime-key-habit\t1\t6\tmissing\t\tn'* ||
        "$installed_preferences_summary" != *$'top-segment-chain\t1\t6\twoc\t我操'* ]]; then
        echo "installed preference checker should summarize preference rows" >&2
        exit 1
    fi
fi
bad_preferences_sample="$tmp_dir/bad-candidate-preferences.tsv"
printf 'nihao\t你号\t0\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject non-positive counts" >&2
    exit 1
fi
printf '__correction__\tihao\t2\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject malformed correction rows" >&2
    exit 1
fi
printf '__correction__\tihao\tzzzz\t2\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject implausible correction rows" >&2
    exit 1
fi
printf '__correction_pattern__\tmissing\tbad\tn\t0\t0\t2\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject malformed runtime correction patterns" >&2
    exit 1
fi
printf '__key_habit__\tmissing\t\tn\t0\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject malformed runtime key habits" >&2
    exit 1
fi
printf 'nihao\t你号\t3\textra\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject malformed preference rows" >&2
    exit 1
fi
printf 'legacy-key\t0\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject malformed legacy preference rows" >&2
    exit 1
fi
printf '__segment_chain__\tnihao\tni\t你\thao\tzzzz\t你好\t3\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject implausible segment-chain correction rows" >&2
    exit 1
fi
printf '__segment_chain__\tnihao\tn\t你\thao\tnihao\t你好\t3\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject segment-chain rows with impossible consumed/remaining shape" >&2
    exit 1
fi
printf '__segment_chain__\tnihao\tni\t你\thao\tnihao\t好你\t3\n' >"$bad_preferences_sample"
if "$ROOT/scripts/check-preferences.sh" "$bad_preferences_sample" >/dev/null 2>&1; then
    echo "preference checker should reject segment-chain rows whose combined candidate does not start with committed text" >&2
    exit 1
fi

model_output=$(printf 'protocol\t1\npreedit\tihao\ncandidates\n' | "$ROOT/scripts/model-protocol-example.sh")
if [[ -n "$model_output" ]]; then
    echo "model protocol example returned unexpected output: $model_output" >&2
    exit 1
fi
model_no_newline_output=$(printf 'protocol\t1\npreedit\tihao' | "$ROOT/scripts/model-protocol-example.sh")
if [[ -n "$model_no_newline_output" ]]; then
    echo "model protocol example should parse a final request row without a trailing newline: $model_no_newline_output" >&2
    exit 1
fi
model_partial_rewrite_output=$(printf 'protocol\t1\npreedit\tnhao\ncandidates\n' | "$ROOT/scripts/model-protocol-example.sh")
if [[ -n "$model_partial_rewrite_output" ]]; then
    echo "model protocol example returned unexpected partial-rewrite output: $model_partial_rewrite_output" >&2
    exit 1
fi
model_segment_chain_output=$(
    printf 'protocol\t1\npreedit\twoc\ncandidates\t我操\t我曹\nsegment_chain\twoc\two\t我\tc\twocao\t我操\n' |
        "$ROOT/scripts/model-protocol-example.sh"
)
if [[ "$model_segment_chain_output" != $'candidate\t我操\ncorrection\twoc\twocao' ]]; then
    echo "model protocol example should demonstrate segment-chain candidate and correction rows: $model_segment_chain_output" >&2
    exit 1
fi
model_bad_segment_chain_protocol_output=$(
    printf 'protocol\t1\npreedit\twoc\ncandidates\t我操\t我曹\nsegment_chain\twoc\two\t我\tx\twocao\t我操\n' |
        "$ROOT/scripts/model-protocol-example.sh"
)
if [[ -n "$model_bad_segment_chain_protocol_output" ]]; then
    echo "model protocol example should ignore impossible segment-chain rows: $model_bad_segment_chain_protocol_output" >&2
    exit 1
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" && -x "$HOME/.local/bin/tipe-model-protocol-example" ]]; then
    installed_model_output=$(printf 'protocol\t1\npreedit\tihao\ncandidates\n' | "$HOME/.local/bin/tipe-model-protocol-example")
    if [[ -n "$installed_model_output" ]]; then
        echo "installed model protocol example returned unexpected output: $installed_model_output" >&2
        exit 1
    fi
    installed_model_partial_rewrite_output=$(printf 'protocol\t1\npreedit\tnhao\ncandidates\n' | "$HOME/.local/bin/tipe-model-protocol-example")
    if [[ -n "$installed_model_partial_rewrite_output" ]]; then
        echo "installed model protocol example returned unexpected partial-rewrite output: $installed_model_partial_rewrite_output" >&2
        exit 1
    fi
fi

adapter_output=$(printf 'protocol\t1\npreedit\tihao\ncandidates\t你好\t你号\n' | "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_output" ]]; then
    echo "model adapter returned unexpected heuristic output: $adapter_output" >&2
    exit 1
fi
adapter_no_newline_output=$(printf 'protocol\t1\npreedit\tihao' | "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_no_newline_output" ]]; then
    echo "model adapter should parse a final request row without a trailing newline: $adapter_no_newline_output" >&2
    exit 1
fi
adapter_partial_rewrite_output=$(printf 'protocol\t1\npreedit\tnhao\ncandidates\t你好\t你号\n' | "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_partial_rewrite_output" ]]; then
    echo "model adapter returned unexpected partial-rewrite heuristic output: $adapter_partial_rewrite_output" >&2
    exit 1
fi
adapter_multi_correction_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tletter:i\tletter:h\tletter:a\tletter:o\n' |
        "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_multi_correction_output" != *$'correction\tihao\tnihao'* ||
    "$adapter_multi_correction_output" != *$'correction\tnhao\tnihao'* ]]; then
    echo "model adapter heuristic should emit all distinct safe correction signals from one supervised key trail: $adapter_multi_correction_output" >&2
    exit 1
fi
adapter_preference_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\npreference\tnihao\t你号\t5\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_preference_output" != $'candidate\t你号' ]]; then
    echo "model adapter heuristic should use learned preference rows for ranking without re-emitting them as new learning: $adapter_preference_output" >&2
    exit 1
fi
adapter_inactive_preference_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\npreference\tnihao\t你号\t1\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_inactive_preference_output" ]]; then
    echo "model adapter heuristic should ignore one-count candidate preference evidence: $adapter_inactive_preference_output" >&2
    exit 1
fi
adapter_inactive_raw_preference_output=$(printf 'protocol\t1\npreedit\tstarted\ncandidates\t三他人特定\tstarted\npreference\tstarted\tstarted\t2\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_inactive_raw_preference_output" ]]; then
    echo "model adapter heuristic should ignore raw preference evidence below count three: $adapter_inactive_raw_preference_output" >&2
    exit 1
fi
adapter_active_raw_preference_output=$(printf 'protocol\t1\npreedit\tstarted\ncandidates\t三他人特定\tstarted\npreference\tstarted\tstarted\t3\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_active_raw_preference_output" != $'candidate\tstarted' ]]; then
    echo "model adapter heuristic should activate raw preference evidence at count three: $adapter_active_raw_preference_output" >&2
    exit 1
fi
adapter_generic_raw_preference_output=$(printf 'protocol\t1\npreedit\tgoal\ncandidates\t个哦阿里\tgoal\npreference\tgoal\tgoal\t3\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_generic_raw_preference_output" != $'candidate\tgoal' ]]; then
    echo "model adapter heuristic should learn non-hardcoded terminal-shape English identifiers: $adapter_generic_raw_preference_output" >&2
    exit 1
fi
adapter_pinyin_raw_preference_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\tnihao\npreference\tnihao\tnihao\t9\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_pinyin_raw_preference_output" ]]; then
    echo "model adapter heuristic should not treat normal pinyin as learned raw English: $adapter_pinyin_raw_preference_output" >&2
    exit 1
fi
adapter_v_pinyin_raw_preference_output=$(printf 'protocol\t1\npreedit\tlv\ncandidates\t绿\tlv\npreference\tlv\tlv\t9\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_v_pinyin_raw_preference_output" ]]; then
    echo "model adapter heuristic should preserve valid v-form pinyin: $adapter_v_pinyin_raw_preference_output" >&2
    exit 1
fi
adapter_selected_candidate_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nselected_candidate\t1\t你号\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_selected_candidate_output" != $'candidate\t你号\npreference\tnihao\t你号\t2' ]]; then
    echo "model adapter heuristic should turn a non-leading selected candidate into immediate ranking and learning actions: $adapter_selected_candidate_output" >&2
    exit 1
fi
adapter_selected_prefix_output=$(printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\nselected_candidate\t1\t继续\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_selected_prefix_output" ]]; then
    echo "model adapter heuristic should not learn prefix-only selected candidates as full-preedit preferences: $adapter_selected_prefix_output" >&2
    exit 1
fi
adapter_selected_short_partial_output=$(printf 'protocol\t1\npreedit\tshdfjshdkjfa\ncandidates\t是的福建省雕刻技法\t水电费\nselected_candidate\t1\t水电费\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_selected_short_partial_output" ]]; then
    echo "model adapter heuristic should not learn short partial selected candidates as full-preedit preferences: $adapter_selected_short_partial_output" >&2
    exit 1
fi
adapter_selected_full_consumed_output=$(printf 'protocol\t1\npreedit\tpinyinjiu\ncandidates\t品饮酒\t拼音就\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t9\tsource\tprefix-continuation\tscore\t999999\nselected_candidate\t1\t拼音就\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_selected_full_consumed_output" != $'candidate\t拼音就\npreference\tpinyinjiu\t拼音就\t2' ]]; then
    echo "model adapter heuristic should immediately rank and learn full-consuming consumed-prefix candidates as full-preedit preferences: $adapter_selected_full_consumed_output" >&2
    exit 1
fi
adapter_selected_top_candidate_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nselected_candidate\t0\t你好\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_selected_top_candidate_output" ]]; then
    echo "model adapter heuristic should not self-learn selected top candidates: $adapter_selected_top_candidate_output" >&2
    exit 1
fi
adapter_known_correction_output=$(printf 'protocol\t1\npreedit\tihao\ncandidates\t你好\t你号\ncorrection\tihao\tnihao\t3\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_known_correction_output" != $'correction\tihao\tnihao' ]]; then
    echo "model adapter heuristic should use learned correction rows: $adapter_known_correction_output" >&2
    exit 1
fi
adapter_pattern_correction_output=$(printf 'protocol\t1\npreedit\tong\ncandidates\t同\t通\ncorrection\tihao\tnihao\t2\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_pattern_correction_output" != $'correction\tong\tnong' ]]; then
    echo "model adapter heuristic should generalize repeated missing-letter correction patterns: $adapter_pattern_correction_output" >&2
    exit 1
fi
adapter_ambiguous_pattern_output=$(printf 'protocol\t1\npreedit\tong\ncandidates\t同\t通\ncorrection\tihao\tnihao\t2\ncorrection\teli\tgeli\t2\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_ambiguous_pattern_output" ]]; then
    echo "model adapter heuristic should suppress tied generalized correction guesses: $adapter_ambiguous_pattern_output" >&2
    exit 1
fi
adapter_strong_pattern_output=$(printf 'protocol\t1\npreedit\tong\ncandidates\t同\t通\ncorrection\tihao\tnihao\t3\ncorrection\teli\tgeli\t2\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_strong_pattern_output" != $'correction\tong\tnong' ]]; then
    echo "model adapter heuristic should emit only a uniquely strongest generalized correction: $adapter_strong_pattern_output" >&2
    exit 1
fi
adapter_suffix_pattern_correction_output=$(printf 'protocol\t1\npreedit\txuesh\ncandidates\t学生\t学士\ncorrection\tcesh\tceshi\t2\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_suffix_pattern_correction_output" != $'correction\txuesh\txueshi' ]]; then
    echo "model adapter heuristic should generalize suffix-relative correction patterns without bad absolute variants: $adapter_suffix_pattern_correction_output" >&2
    exit 1
fi
adapter_weak_pattern_correction_output=$(printf 'protocol\t1\npreedit\tong\ncandidates\t同\t通\ncorrection\tihao\tnihao\t1\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_weak_pattern_correction_output" ]]; then
    echo "model adapter heuristic should not generalize weak correction patterns: $adapter_weak_pattern_correction_output" >&2
    exit 1
fi
adapter_aggregated_pattern_output=$(printf 'protocol\t1\npreedit\tiren\ncandidates\t艺人\t一人\ncorrection\tihao\tnihao\t1\ncorrection\timen\tnimen\t1\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_aggregated_pattern_output" != $'correction\tiren\tniren' ]]; then
    echo "model adapter heuristic should aggregate separate observations of one correction pattern: $adapter_aggregated_pattern_output" >&2
    exit 1
fi
adapter_transpose_pattern_output=$(printf 'protocol\t1\npreedit\tjibengongnegn\ncandidates\t基本功呢功能\t基本功能\ncorrection\tgongnegn\tgongneng\t2\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_transpose_pattern_output" != $'correction\tjibengongnegn\tjibengongneng' ]]; then
    echo "model adapter heuristic should generalize adjacent-transposition correction patterns: $adapter_transpose_pattern_output" >&2
    exit 1
fi
adapter_transpose_english_output=$(printf 'protocol\t1\npreedit\tstringn\ncandidates\tstringn\ncorrection\tgongnegn\tgongneng\t2\n' |
    TIPE_SUPERVISION_HISTORY="$tmp_dir/no-adapter-history.tsv" "$ROOT/scripts/model-adapter.sh")
if [[ -n "$adapter_transpose_english_output" ]]; then
    echo "model adapter heuristic should not apply learned transpose patterns to English identifiers" >&2
    exit 1
fi
adapter_trail_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_trail_output" != $'correction\tihao\tnihao' ]]; then
    echo "model adapter did not infer correction from correction_events: $adapter_trail_output" >&2
    exit 1
fi
adapter_digit_trail_output=$(printf 'protocol\t1\npreedit\tqwen3\ncandidates\tqwen3\t全文\ncorrection_events\tletter:q\tletter:w\tletter:e\tletter:n\tdigit:2\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:q\tletter:w\tletter:e\tletter:n\tdigit:3\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_digit_trail_output" != $'correction\tqwen2\tqwen3\ncandidate\tqwen3' ]]; then
    echo "model adapter did not infer digit-aware correction trail: $adapter_digit_trail_output" >&2
    exit 1
fi
adapter_cursor_insert_trail_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncorrection_events\tletter:n\tletter:h\tletter:a\tletter:o\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tletter:i\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_cursor_insert_trail_output" != $'correction\tnhao\tnihao' ]]; then
    echo "model adapter did not infer cursor-insert correction trail: $adapter_cursor_insert_trail_output" >&2
    exit 1
fi
adapter_delete_trail_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncorrection_events\tletter:n\tletter:i\tletter:y\tletter:h\tletter:a\tletter:o\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tdelete:\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_delete_trail_output" != $'correction\tniyhao\tnihao' ]]; then
    echo "model adapter did not infer delete-key correction trail: $adapter_delete_trail_output" >&2
    exit 1
fi
adapter_delete_erase_trail_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tdelete:\tdelete:\tdelete:\tdelete:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\n' |
    "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_delete_erase_trail_output" != $'correction\tihao\tnihao' ]]; then
    echo "model adapter did not infer Delete full-erase correction trail: $adapter_delete_erase_trail_output" >&2
    exit 1
fi
if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" && -x "$HOME/.local/bin/tipe-model-adapter" ]]; then
    installed_adapter_output=$(printf 'protocol\t1\npreedit\tihao\ncandidates\t你好\t你号\n' | "$HOME/.local/bin/tipe-model-adapter")
    if [[ -n "$installed_adapter_output" ]]; then
        echo "installed model adapter returned unexpected heuristic output: $installed_adapter_output" >&2
        exit 1
    fi
    installed_adapter_partial_rewrite_output=$(printf 'protocol\t1\npreedit\tnhao\ncandidates\t你好\t你号\n' | "$HOME/.local/bin/tipe-model-adapter")
    if [[ -n "$installed_adapter_partial_rewrite_output" ]]; then
        echo "installed model adapter returned unexpected partial-rewrite heuristic output: $installed_adapter_partial_rewrite_output" >&2
        exit 1
    fi
    installed_adapter_trail_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\n' |
        "$HOME/.local/bin/tipe-model-adapter")
    if [[ "$installed_adapter_trail_output" != $'correction\tihao\tnihao' ]]; then
        echo "installed model adapter did not infer correction from correction_events: $installed_adapter_trail_output" >&2
        exit 1
    fi
fi

adapter_english_output=$(printf 'protocol\t1\npreedit\tgithub\ncandidates\tgithub\t计特哈布\n' | "$ROOT/scripts/model-adapter.sh")
if [[ "$adapter_english_output" != $'candidate\tgithub' ]]; then
    echo "model adapter returned unexpected english output: $adapter_english_output" >&2
    exit 1
fi
for adapter_english_token in chatgpt javascript niri ollama git npm node rust cargo cmake build cmakebuild qwen2 gpt4 ipv6; do
    adapter_english_output=$(printf 'protocol\t1\npreedit\t%s\ncandidates\t awkward\t%s\n' \
        "$adapter_english_token" "$adapter_english_token" | "$ROOT/scripts/model-adapter.sh")
    if [[ "$adapter_english_output" != $'candidate\t'"$adapter_english_token" ]]; then
        echo "model adapter returned unexpected extended english output for $adapter_english_token: $adapter_english_output" >&2
        exit 1
    fi
    if [[ "${TIPE_SMOKE_CHECK_INSTALLED:-0}" == "1" &&
        -x "$HOME/.local/bin/tipe-model-adapter" &&
        "$adapter_english_token" != "qwen2" &&
        "$adapter_english_token" != "gpt4" &&
        "$adapter_english_token" != "ipv6" ]]; then
        installed_adapter_english_output=$(printf 'protocol\t1\npreedit\t%s\ncandidates\t awkward\t%s\n' \
            "$adapter_english_token" "$adapter_english_token" | "$HOME/.local/bin/tipe-model-adapter")
        if [[ "$installed_adapter_english_output" != $'candidate\t'"$adapter_english_token" ]]; then
            echo "installed model adapter returned unexpected extended english output for $adapter_english_token: $installed_adapter_english_output" >&2
            exit 1
        fi
    fi
done
model_dump_path="$tmp_dir/model-request.tsv"
model_dump_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        TIPE_MODEL_DUMP_PATH="$model_dump_path" TIPE_MODEL_DUMP_RESPONSE=$'candidate\t你号\n' "$ROOT/scripts/model-dump.sh"
)
if [[ "$model_dump_output" != $'candidate\t你号' ]] ||
    ! cmp -s "$model_dump_path" <(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n'); then
    echo "model dump helper should capture request payload and emit optional response" >&2
    exit 1
fi
model_explain_sample="$tmp_dir/model-explain-request.tsv"
printf 'protocol\t1\npreedit\tnihao\napplication\tAlacritty\nsurrounding_before\t刚才\\tPath\\\\Name\nsurrounding_after\t后面\\nText\ncandidates\t你好\t你号\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nruntime_state\tcontinuous\t1\nselected_candidate\t1\t你号\nvisible_candidates\t0:你好\t1:你号\nnumbered_candidates\t1:1:你号\nevents\tletter:n\tcursor-move:Down\nevent_counts\tletter:1\tcursor-move:1\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\ncorrection_event_counts\tletter:9\tbackspace:4\ncontext\t刚才\nsegment_chain\tnihao\tni\t你\thao\tnihao\t你好\npreference\tnihao\t你号\t3\npreference\tnihao\t你好\t8\ncorrection\tihao\tnihao\t2\ncorrection\tnhao\tnihao\t5\n' >"$model_explain_sample"
model_explain_history_cache="$tmp_dir/model-explain-history-cache"
mkdir -p "$model_explain_history_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\told\tcandidates\t1\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\told'
    printf '%s\n' $'event_counts\tobserved:1'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t1\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\t\tcandidates\t0\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\t'
    printf '%s\n' $'event_counts\tspace:2\tdelete:1\tcursor-move:1'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t4\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t3\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'event_counts\tletter:1\tcursor-move:1'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t2\tcorrection_events\t0'
} >"$model_explain_history_cache/tipe/supervision-history.tsv"
model_explain_output=$(XDG_CACHE_HOME="$model_explain_history_cache" "$ROOT/scripts/model-explain.sh" "$model_explain_sample")
if [[ "$model_explain_output" != *$'preedit\tnihao'* ||
    "$model_explain_output" != *$'candidate_count\t2'* ||
    "$model_explain_output" != *$'selected_candidate\t1\t你号'* ||
    "$model_explain_output" != *$'runtime_state\tcontinuous\t1'* ||
    "$model_explain_output" != *$'surrounding_before\t刚才\\tPath\\\\Name'* ||
    "$model_explain_output" != *$'surrounding_after\t后面\\nText'* ||
    "$model_explain_output" != *$'visible_candidate\t0\t你好'* ||
    "$model_explain_output" != *$'numbered_candidate\t1\t1\t你号'* ||
    "$model_explain_output" != *$'events_count\t2'* ||
    "$model_explain_output" != *$'event_counts_count\t2'* ||
    "$model_explain_output" != *$'event_counts_kind_count\tletter\t1'* ||
    "$model_explain_output" != *$'event_counts_kind_count\tcursor-move\t1'* ||
    "$model_explain_output" != *$'events_kind_count\tletter\t1'* ||
    "$model_explain_output" != *$'events_kind_count\tcursor-move\t1'* ||
    "$model_explain_output" != *$'correction_events_count\t13'* ||
    "$model_explain_output" != *$'correction_event_counts_count\t2'* ||
    "$model_explain_output" != *$'correction_event_counts_kind_count\tletter\t9'* ||
    "$model_explain_output" != *$'correction_event_counts_kind_count\tbackspace\t4'* ||
    "$model_explain_output" != *$'correction_events_kind_count\tletter\t9'* ||
    "$model_explain_output" != *$'correction_events_kind_count\tbackspace\t4'* ||
    "$model_explain_output" != *$'behavior_recent_event_count\tletter\t1'* ||
    "$model_explain_output" != *$'behavior_correction_event_count\tbackspace\t4'* ||
    "$model_explain_output" != *$'behavior_history_summary\trecords\t3\tactive\t2\tpass-through\t1\tpath\t'"$model_explain_history_cache/tipe/supervision-history.tsv"* ||
    "$model_explain_output" != *$'behavior_history_event_count\tspace\t2'* ||
    "$model_explain_output" != *$'behavior_history_active_event_count\tletter\t1'* ||
    "$model_explain_output" != *$'behavior_history_pass_through_event_count\tspace\t2'* ||
    "$model_explain_output" != *$'behavior_history_pass_through_event_count\tdelete\t1'* ||
    "$model_explain_output" != *$'behavior_raw_english_hint\t0\tnone'* ||
    "$model_explain_output" != *$'behavior_preedit_leading_context\tactive\t0\tevents\t0'* ||
    "$model_explain_output" != *$'behavior_possible_correction\tfull-delete-retype\tihao\tnihao'* ||
    "$model_explain_output" != *$'behavior_realtime_correction\t1\tskipped\talready-present\tmissing\ti\t1\t5'* ||
    "$model_explain_output" != *$'behavior_realtime_correction\t2\tskipped\talready-present\tmissing\tn\t0\t2'* ||
    "$model_explain_output" != *$'learning_preference_count\t2'* ||
    "$model_explain_output" != *$'learning_preference_total\t11'* ||
    "$model_explain_output" != *$'learning_correction_count\t2'* ||
    "$model_explain_output" != *$'learning_correction_total\t7'* ||
    "$model_explain_output" != *$'learning_top_preference\t1\t8\tnihao\t你好'* ||
    "$model_explain_output" != *$'learning_top_correction\t1\t5\tnhao\tnihao'* ||
    "$model_explain_output" != *$'learning_selected_candidate_signal\tnihao\t1\t你号\twould-learn-preference'* ||
    "$model_explain_output" != *$'learning_segment_chain_signal\t1\tnihao\tni\t你\thao\tnihao\t你好\tcontinuation'* ||
    "$model_explain_output" != *$'learning_correction_pattern\t1\t5\tmissing\ti\t1'* ||
    "$model_explain_output" != *$'learning_correction_pattern\t2\t2\tmissing\tn\t0'* ||
    "$model_explain_output" != *$'context_item\t0\t刚才'* ||
    "$model_explain_output" != *$'segment_chain\tnihao\tni\t你\thao\tnihao\t你好'* ]]; then
    echo "model explain helper should summarize model request TSV" >&2
    exit 1
fi
model_explain_transpose_output=$(printf 'protocol\t1\npreedit\tjibengongnegn\ncandidates\t基本功呢功能\t基本功能\ncorrection\tgongnegn\tgongneng\t2\n' |
    "$ROOT/scripts/model-explain.sh")
if [[ "$model_explain_transpose_output" != *$'learning_correction_pattern\t1\t2\ttranspose\tgn->ng\tend-0'* ||
    "$model_explain_transpose_output" != *$'behavior_realtime_correction\t1\tapplied\tok\ttranspose\tgn->ng\tend-0\t2\tjibengongneng'* ]]; then
    echo "model explain helper should expose applicable adjacent-transposition patterns" >&2
    exit 1
fi
model_explain_bad_chain_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nsegment_chain\tnihao\tn\t你\thao\tnihao\t你好\n' |
        "$ROOT/scripts/model-explain.sh"
)
if [[ "$model_explain_bad_chain_output" != *$'invalid_segment_chain\t4\tnihao\tn\t你\thao\tnihao\t你好'* ||
    "$model_explain_bad_chain_output" == *$'learning_segment_chain_signal'* ]]; then
    echo "model explain helper should mark malformed segment-chain rows invalid instead of learning them" >&2
    exit 1
fi
supervision_mode_history_cache="$tmp_dir/supervision-mode-history-cache"
mkdir -p "$supervision_mode_history_cache/tipe"
supervision_mode_request="$supervision_mode_history_cache/request.tsv"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\t错\n' >"$supervision_mode_request"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t3\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号\t错'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你号'
    printf '%s\n' $'event_counts\tletter:5\tcandidate-selected:1'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t6\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t3\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号\t错'
    printf '%s\n' $'selected_candidate\t1\t错'
    printf '%s\n' $'event_counts\tletter:9\tspace:1\tcursor-move:1'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t3\tcorrection_events\t0'
} >"$supervision_mode_history_cache/tipe/supervision-history.tsv"
supervision_mode_explain_output=$(
    XDG_CACHE_HOME="$supervision_mode_history_cache" "$ROOT/scripts/model-explain.sh" "$supervision_mode_request"
)
if [[ "$supervision_mode_explain_output" != *$'behavior_history_summary\trecords\t2\tactive\t1\tpass-through\t1\tpath\t'"$supervision_mode_history_cache/tipe/supervision-history.tsv"* ||
    "$supervision_mode_explain_output" != *$'behavior_history_active_event_count\tletter\t5'* ||
    "$supervision_mode_explain_output" == *$'behavior_history_active_event_count\tspace\t1'* ||
    "$supervision_mode_explain_output" != *$'behavior_history_pass_through_event_count\tletter\t9'* ||
    "$supervision_mode_explain_output" != *$'behavior_history_pass_through_event_count\tspace\t1'* ]]; then
    echo "model explain history summary should classify records by supervision_state mode" >&2
    exit 1
fi
supervision_mode_panel_output=$(
    XDG_CACHE_HOME="$supervision_mode_history_cache" "$ROOT/scripts/learning-panel.sh" --raw-panel "$supervision_mode_request"
)
if [[ "$supervision_mode_panel_output" != *$'panel\thistory\tsummary\trecords\t2\tactive\t1\tpass-through\t1'* ||
    "$supervision_mode_panel_output" != *$'panel\thistory\tpreedit-selected\t1\tnihao\t你号\t1'* ||
    "$supervision_mode_panel_output" == *$'panel\thistory\tpreedit-selected\t'*$'\tnihao\t错\t'* ]]; then
    echo "learning panel history rows should not learn stale pass-through preedit selections" >&2
    exit 1
fi
model_explain_panel_output=$("$ROOT/scripts/model-explain.sh" --panel "$model_explain_sample")
model_explain_panel_stdin_output=$(cat "$model_explain_sample" | "$ROOT/scripts/model-explain.sh" --panel -)
if [[ "$model_explain_panel_stdin_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$model_explain_panel_stdin_output" != *$'panel\tstate\tsurrounding-before\t刚才\\tPath\\\\Name'* ||
    "$model_explain_panel_stdin_output" != *$'panel\tstate\tsurrounding-after\t后面\\nText'* ||
    "$model_explain_panel_stdin_output" != *$'panel\tcandidates\tselected\t1\t你号'* ]]; then
    echo "model explain helper should accept - as stdin" >&2
    exit 1
fi
if [[ "$model_explain_panel_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$model_explain_panel_output" != *$'panel\tstate\tapplication\tAlacritty'* ||
    "$model_explain_panel_output" != *$'panel\tstate\tsurrounding-before\t刚才\\tPath\\\\Name'* ||
    "$model_explain_panel_output" != *$'panel\tstate\tsurrounding-after\t后面\\nText'* ||
    "$model_explain_panel_output" != *$'panel\tcandidates\ttotal\t2\tvisible\t2\tnumbered\t1'* ||
    "$model_explain_panel_output" != *$'panel\tcandidates\tfirst\t0\t你好'* ||
    "$model_explain_panel_output" != *$'panel\tcandidates\tselected\t1\t你号'* ||
    "$model_explain_panel_output" != *$'panel\tcandidates\tvisible\t1\t0\t你好'* ||
    "$model_explain_panel_output" != *$'panel\tcandidates\tnumbered\t1\t1\t1\t你号'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\trecent-events\t2\tcorrection-events\t13\tcontext\t1\tsegment-chains\t1\tpending-segments\t0'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tmodel-input\tpreedit\tnihao\tcandidates\t2\tvisible\t2\tnumbered\t1\tcontext\t1\tsegment-chains\t1\tpending-segments\t0'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tmode\tactive-preedit'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\truntime-state\tcontinuous\t1'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tevent-trail\trecent\t2\tlimit\t64\tpurpose\tui-and-short-action-order'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tcorrection-trail\trecent\t13\tlimit\t256\tpurpose\tdelete-retype-and-middle-edit-learning'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tevent-count\tletter\t1'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tevent-count\tcursor-move\t1'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tcorrection-event-count\tletter\t9'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tcorrection-event-count\tbackspace\t4'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tevent-item\t1\tletter\tn'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tevent-item\t2\tcursor-move\tDown'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tcorrection-event-item\t1\tletter\th'* ||
    "$model_explain_panel_output" != *$'panel\tsupervision\tcorrection-event-item\t12\tletter\to'* ||
    "$model_explain_panel_output" != *$'panel\tsegment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好'* ||
    "$model_explain_panel_output" != *$'panel\tlearning\tpreferences\t2\ttotal\t11\tcorrections\t2\ttotal\t7'* ||
    "$model_explain_panel_output" != *$'panel\tlearning\tselected-candidate-signal\tnihao\t1\t你号\twould-learn-preference'* ||
    "$model_explain_panel_output" != *$'panel\tlearning\tsegment-chain-signal\t1\tnihao\tni\t你\thao\tnihao\t你好\tcontinuation'* ||
    "$model_explain_panel_output" != *$'panel\tlearning\tcorrection-signal\t1\tfull-delete-retype\tihao\tnihao'* ||
    "$model_explain_panel_output" != *$'panel\ttop-preference\t1\t8\tnihao\t你好'* ||
    "$model_explain_panel_output" != *$'panel\ttop-correction\t1\t5\tnhao\tnihao'* ||
    "$model_explain_panel_output" != *$'panel\ttop-correction-pattern\t1\t5\tmissing\ti\t1'* ||
    "$model_explain_panel_output" != *$'panel\ttop-correction-pattern\t2\t2\tmissing\tn\t0'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\traw-english-hint\t0\tsource\tnone'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\tpreedit-leading-context\tactive\t0\tevents\t0'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\tedit-summary\tcurrent\tnihao\tcursor\t5\ttyped-tail\tnihao\tlast-erased\tihao\tlast-edited\tihao\tmiddle-edit\t'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\trecent-event\tletter\t1'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\tcorrection-event\tbackspace\t4'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\tpossible-correction\t1\tfull-delete-retype\tihao\tnihao'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\tcorrection-pattern\t1\tmissing\ti\t1\t5'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\tcorrection-pattern\t2\tmissing\tn\t0\t2'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\trealtime-correction\t1\tskipped\talready-present\tmissing\ti\t1\t5'* ||
    "$model_explain_panel_output" != *$'panel\tbehavior\trealtime-correction\t2\tskipped\talready-present\tmissing\tn\t0\t2'* ]]; then
    echo "model explain helper should print stable learning panel rows" >&2
    exit 1
fi
preedit_leading_output=$(
    printf 'protocol\t1\npreedit\tni\ncandidates\t你\nevents\tobserved:WindowSwitch\tcursor-move:Left\tletter:n\tletter:i\nevent_counts\tobserved:1\tcursor-move:1\tletter:2\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$preedit_leading_output" != *$'panel\tbehavior\tpreedit-leading-context\tactive\t1\tevents\t2'* ||
    "$preedit_leading_output" != *$'panel\tbehavior\tpreedit-leading-event\t1\tobserved\tWindowSwitch'* ||
    "$preedit_leading_output" != *$'panel\tbehavior\tpreedit-leading-event\t2\tcursor-move\tLeft'* ]]; then
    echo "model explain helper should expose key context before the first preedit letter" >&2
    exit 1
fi
learning_panel_data_home="$tmp_dir/learning-panel-data"
mkdir -p "$learning_panel_data_home/tipe"
cp "$preferences_sample" "$learning_panel_data_home/tipe/candidate-preferences.tsv"
learning_panel_model_config="$tmp_dir/learning-panel-model-env"
printf 'export TIPE_MODEL_COMMAND=%s\nexport TIPE_MODEL_MODE=personal\nexport TIPE_MODEL_BACKEND=personal\nexport TIPE_PERSONAL_MODEL_PATH=%s\n' \
    "$ROOT/scripts/model-current.sh" "$tmp_dir/untrained-personal-model.json" >"$learning_panel_model_config"
learning_panel_output=$(TIPE_MODEL_CONFIG="$learning_panel_model_config" \
    XDG_DATA_HOME="$learning_panel_data_home" "$ROOT/scripts/learning-panel.sh" "$model_explain_sample")
if [[ "$learning_panel_output" != *$'TiPE 学习面板'* ||
    "$learning_panel_output" != *$'state\tpreedit\tnihao'* ||
    "$learning_panel_output" != *$'state\tsurrounding-before\t刚才\\tPath\\\\Name'* ||
    "$learning_panel_output" != *$'state\tsurrounding-after\t后面\\nText'* ||
    "$learning_panel_output" != *$'候选\ttotal\t2\tvisible\t2\tnumbered\t1'* ||
    "$learning_panel_output" != *$'候选\tfirst\t0\t你好'* ||
    "$learning_panel_output" != *$'候选\tselected\t1\t你号'* ||
    "$learning_panel_output" != *$'候选\tvisible\t1\t0\t你好'* ||
    "$learning_panel_output" != *$'候选\tnumbered\t1\t1\t1\t你号'* ||
    "$learning_panel_output" != *$'supervision\trecent-events\t2\tcorrection-events\t13\tcontext\t1\tsegment-chains\t1\tpending-segments\t0'* ||
    "$learning_panel_output" != *$'supervision\tmodel-input\tpreedit\tnihao\tcandidates\t2\tvisible\t2\tnumbered\t1\tcontext\t1\tsegment-chains\t1\tpending-segments\t0'* ||
    "$learning_panel_output" != *$'supervision\tmode\tactive-preedit'* ||
    "$learning_panel_output" != *$'supervision\truntime-state\tcontinuous\t1'* ||
    "$learning_panel_output" != *$'supervision\tevent-trail\trecent\t2\tlimit\t64\tpurpose\tui-and-short-action-order'* ||
    "$learning_panel_output" != *$'supervision\tcorrection-trail\trecent\t13\tlimit\t256\tpurpose\tdelete-retype-and-middle-edit-learning'* ||
    "$learning_panel_output" != *$'supervision\tevent-item\t1\tletter\tn'* ||
    "$learning_panel_output" != *$'supervision\tcorrection-event-item\t12\tletter\to'* ||
    "$learning_panel_output" != *$'segment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好'* ||
    "$learning_panel_output" != *$'learning\tpreferences\t2\ttotal\t11\tcorrections\t2\ttotal\t7'* ||
    "$learning_panel_output" != *$'learning\tselected-candidate-signal\tnihao\t1\t你号\twould-learn-preference'* ||
    "$learning_panel_output" != *$'learning\tsegment-chain-signal\t1\tnihao\tni\t你\thao\tnihao\t你好\tcontinuation'* ||
    "$learning_panel_output" != *$'learning\tcorrection-signal\t1\tfull-delete-retype\tihao\tnihao'* ||
    "$learning_panel_output" != *$'learning\tevidence-summary\tnihao\tpreferences\t1\tlegacy-preferences\t0\tcorrections\t1\tsegment-chains\t0'* ||
    "$learning_panel_output" != *$'learning\tevidence-preference\t1\tnihao\t你号\t3'* ||
    "$learning_panel_output" != *$'learning\tevidence-correction\t3\tcorrected\tihao\tnihao\t2'* ||
    "$learning_panel_output" != *$'learning\tevidence-effect\trank-preference\t1\tnihao\t你号\t3'* ||
    "$learning_panel_output" != *$'learning\tevidence-effect\tcorrection-target\t3\tihao\tnihao\t2'* ||
    "$learning_panel_output" != *$'top-correction-pattern\t1\t5\tmissing\ti\t1'* ||
    "$learning_panel_output" != *$'model-config\tkind\t'* ||
    "$learning_panel_output" != *$'model-config\tprocess-command-active-scope\tcurrent-shell-only-not-fcitx5-runtime'* ||
    "$learning_panel_output" != *$'model-config\truntime-verification\ttipe-doctor'* ||
    "$learning_panel_output" != *$'model-config\tpersonal-model\t'* ||
    "$learning_panel_output" != *$'model-config\tpersonal-model-status\t'* ||
    "$learning_panel_output" != *$'model-config\tdry-run-test-command\t'*"tipe-model-self-test --current --config"* ||
    "$learning_panel_output" != *$'behavior\tedit-summary\tcurrent\tnihao\tcursor\t5\ttyped-tail\tnihao\tlast-erased\tihao\tlast-edited\tihao\tmiddle-edit'* ||
    "$learning_panel_output" != *$'behavior\tcorrection-pattern\t1\tmissing\ti\t1\t5'* ||
    "$learning_panel_output" != *$'behavior\trealtime-correction\t1\tskipped\talready-present\tmissing\ti\t1\t5'* ||
    "$learning_panel_output" != *$'behavior\tpossible-correction\t1\tfull-delete-retype\tihao\tnihao'* ]]; then
    echo "learning panel helper should render a compact read-only panel from model requests" >&2
    exit 1
fi
learning_panel_raw_output=$(TIPE_DISABLE_LIBIME_LEARNING=0 TIPE_MODEL_CONFIG="$learning_panel_model_config" \
    XDG_DATA_HOME="$learning_panel_data_home" "$ROOT/scripts/learning-panel.sh" --raw-panel "$model_explain_sample")
if [[ "$learning_panel_raw_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$learning_panel_raw_output" != *$'panel\tstate\trequest-source\trequest-file\t'"$model_explain_sample"$'\n'* ||
    "$learning_panel_raw_output" != *$'panel\tstate\trequest-mtime\t'* ||
    "$learning_panel_raw_output" != *$'panel\tstate\tsurrounding-before\t刚才\\tPath\\\\Name'* ||
    "$learning_panel_raw_output" != *$'panel\tstate\tsurrounding-after\t后面\\nText'* ||
    "$learning_panel_raw_output" != *$'panel\tsupervision\tmodel-input\tpreedit\tnihao\tcandidates\t2\tvisible\t2\tnumbered\t1\tcontext\t1\tsegment-chains\t1\tpending-segments\t0'* ||
    "$learning_panel_raw_output" != *$'panel\tsupervision\tmode\tactive-preedit'* ||
    "$learning_panel_raw_output" != *$'panel\tsupervision\truntime-state\tcontinuous\t1'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tcorrection-signal\t1\tfull-delete-retype\tihao\tnihao'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tevidence-summary\tnihao\tpreferences\t1\tlegacy-preferences\t0\tcorrections\t1\tsegment-chains\t0'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tevidence-preference\t1\tnihao\t你号\t3'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tevidence-correction\t3\tcorrected\tihao\tnihao\t2'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tevidence-effect\trank-preference\t1\tnihao\t你号\t3'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tevidence-effect\tcorrection-target\t3\tihao\tnihao\t2'* ||
    "$learning_panel_raw_output" != *$'panel\tsegment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tstatus\tready-to-learn\tnext-step\tprefer-suggested-protocols'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tstatus-suggested-protocol\t1\tpreference\tnihao\t你号\t2'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tstatus-signal-count\tselected_candidate\t1'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tdictionary-history\tstatus\twaiting\tbytes\t0\tpath\t'"$learning_panel_data_home/tipe/libime/user.history"$'\treason\tfirst-selection'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tselected-candidate-signal\tnihao\t1\t你号\twould-learn-preference'* ||
    "$learning_panel_raw_output" != *$'panel\tlearning\tsegment-chain-signal\t1\tnihao\tni\t你\thao\tnihao\t你好\tcontinuation'* ||
    "$learning_panel_raw_output" != *$'panel\tmodel-config\tkind\t'* ||
    "$learning_panel_raw_output" != *$'panel\tmodel-config\tprocess-command-active-scope\tcurrent-shell-only-not-fcitx5-runtime'* ||
    "$learning_panel_raw_output" != *$'panel\tmodel-config\truntime-verification\ttipe-doctor'* ||
    "$learning_panel_raw_output" != *$'panel\tmodel-config\tpersonal-model\t'* ||
    "$learning_panel_raw_output" != *$'panel\tmodel-config\tpersonal-model-status\t'* ||
    "$learning_panel_raw_output" != *$'panel\tmodel-config\tdry-run-test-command\t'*"tipe-model-self-test --current --config"* ||
    "$learning_panel_raw_output" != *$'panel\ttop-correction-pattern\t1\t5\tmissing\ti\t1'* ||
    "$learning_panel_raw_output" != *$'panel\tbehavior\trealtime-correction\t1\tskipped\talready-present\tmissing\ti\t1\t5'* ||
    "$learning_panel_raw_output" != *$'panel\tbehavior\tpossible-correction\t1\tfull-delete-retype\tihao\tnihao'* ]]; then
    echo "learning panel helper should expose raw panel rows for future UI consumers" >&2
    exit 1
fi
mkdir -p "$learning_panel_data_home/tipe/libime"
printf 'history' >"$learning_panel_data_home/tipe/libime/user.history"
chmod 600 "$learning_panel_data_home/tipe/libime/user.history"
learning_panel_dictionary_ready=$(TIPE_DISABLE_LIBIME_LEARNING=0 TIPE_MODEL_CONFIG="$learning_panel_model_config" \
    XDG_DATA_HOME="$learning_panel_data_home" "$ROOT/scripts/learning-panel.sh" --raw-panel "$model_explain_sample")
learning_panel_dictionary_disabled=$(TIPE_DISABLE_LIBIME_LEARNING=0 TIPE_MODEL_CONFIG="$learning_panel_model_config" \
    XDG_DATA_HOME="$learning_panel_data_home" TIPE_LIBIME_USER_HISTORY= \
    "$ROOT/scripts/learning-panel.sh" --raw-panel "$model_explain_sample")
if [[ "$learning_panel_dictionary_ready" != *$'panel\tlearning\tdictionary-history\tstatus\tready\tbytes\t7\tpath\t'"$learning_panel_data_home/tipe/libime/user.history"$'\treason\thealthy'* ||
    "$learning_panel_dictionary_disabled" != *$'panel\tlearning\tdictionary-history\tstatus\tdisabled\tbytes\t0\tpath\t\treason\tenvironment'* ]]; then
    echo "learning panel should distinguish automatic dictionary history readiness from disabled learning" >&2
    exit 1
fi
learning_panel_history_correction_cache="$tmp_dir/learning-panel-history-correction-cache"
mkdir -p "$learning_panel_history_correction_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0\tterminal\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你好'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0\tterminal\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你好'
} >"$learning_panel_history_correction_cache/tipe/supervision-history.tsv"
learning_panel_history_correction_raw_output=$(
    TIPE_MODEL_CONFIG="$learning_panel_model_config" XDG_CACHE_HOME="$learning_panel_history_correction_cache" \
        XDG_DATA_HOME="$learning_panel_data_home" \
        "$ROOT/scripts/learning-panel.sh" --raw-panel "$model_explain_sample"
)
if [[ "$learning_panel_history_correction_raw_output" != *$'panel\thistory\tlearnable-correction\t1\tihao\tnihao\t2\tcorrection\tihao\tnihao'* ||
    "$learning_panel_history_correction_raw_output" != *$'panel\tlearning\tstatus-suggested-protocol\t1\tcorrection\tihao\tnihao'* ||
    "$learning_panel_history_correction_raw_output" != *$'panel\tlearning\tstatus-signal-count\thistory_correction\t1'* ]]; then
    echo "learning panel helper should expose current related history corrections as learnable status rows" >&2
    exit 1
fi
learning_panel_stdin_raw_output=$(cat "$model_explain_sample" | TIPE_MODEL_CONFIG="$learning_panel_model_config" \
    XDG_DATA_HOME="$learning_panel_data_home" "$ROOT/scripts/learning-panel.sh" --raw-panel -)
if [[ "$learning_panel_stdin_raw_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$learning_panel_stdin_raw_output" != *$'panel\tstate\trequest-source\trequest-file\t'* ||
    "$learning_panel_stdin_raw_output" != *$'panel\tmodel-config\tconfigured-command'* ||
    "$learning_panel_stdin_raw_output" != *$'panel\tmodel-config\tprocess-command-active'* ]]; then
    echo "learning panel helper should accept '-' as stdin and expose model config rows" >&2
    exit 1
fi
pass_through_panel_output=$(
    printf 'protocol\t1\npreedit\t\ncandidates\nevents\tspace:\tdelete:\tcursor-move:Down\tobserved:Tab\nevent_counts\tspace:1\tdelete:1\tcursor-move:1\tobserved:1\ncorrection_events\tspace:\tdelete:\tcursor-move:Down\tobserved:Tab\ncorrection_event_counts\tspace:1\tdelete:1\tcursor-move:1\tobserved:1\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$pass_through_panel_output" != *$'panel\tsupervision\tmode\tpass-through-only'* ||
    "$pass_through_panel_output" != *$'panel\tsupervision\trecent-events\t4\tcorrection-events\t4\tcontext\t0\tsegment-chains\t0\tpending-segments\t0'* ||
    "$pass_through_panel_output" != *$'panel\tlearning\tstatus\tkeyboard-context-only\tnext-step\twait-for-active-preedit'* ]]; then
    echo "model explain helper should mark empty-preedit requests as pass-through supervision" >&2
    exit 1
fi
middle_edit_panel_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncorrection_events\tletter:n\tletter:h\tletter:a\tletter:o\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tletter:i\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$middle_edit_panel_output" != *$'panel\tbehavior\tedit-summary\tcurrent\tnihao\tcursor\t2\ttyped-tail\ti\tlast-erased\t\tlast-edited\t\tmiddle-edit\tnhao'* ||
    "$middle_edit_panel_output" != *$'panel\tbehavior\tpossible-correction\t1\tmiddle-edit\tnhao\tnihao'* ]]; then
    echo "model explain helper should summarize cursor insertion typo repairs" >&2
    exit 1
fi
suffix_pattern_panel_output=$(
    printf 'protocol\t1\npreedit\txuesh\ncandidates\t学生\t学士\ncorrection\tcesh\tceshi\t2\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$suffix_pattern_panel_output" != *$'panel\ttop-correction-pattern\t1\t2\tmissing\ti\tend-0'* ||
    "$suffix_pattern_panel_output" != *$'panel\tbehavior\tcorrection-pattern\t1\tmissing\ti\tend-0\t2'* ||
    "$suffix_pattern_panel_output" != *$'panel\tbehavior\trealtime-correction\t1\tapplied\tok\tmissing\ti\tend-0\t2\txueshi'* ]]; then
    echo "model explain helper should mark suffix-relative correction patterns" >&2
    exit 1
fi
selected_top_signal_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nselected_candidate\t0\t你好\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$selected_top_signal_output" != *$'panel\tlearning\tselected-candidate-signal\tnihao\t0\t你好\talready-top'* ]]; then
    echo "model explain helper should mark top selected candidates as already learned by ordering" >&2
    exit 1
fi
selected_prefix_signal_output=$(
    printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\nselected_candidate\t1\t继续\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$selected_prefix_signal_output" != *$'panel\tlearning\tselected-candidate-signal\tjixuzuo\t1\t继续\tprefix-only-no-preference'* ]]; then
    echo "model explain helper should mark prefix-only selected candidates as non-preference learning signals" >&2
    exit 1
fi
if [[ "$selected_prefix_signal_output" != *$'panel\tlearning\tstatus\tawaiting-suffix-confirmation\tnext-step\twait-for-selected-suffix'* ||
    "$selected_prefix_signal_output" != *$'panel\tlearning\tstatus-signal-count\tselected_candidate_prefix\t1'* ]]; then
    echo "model explain helper should expose prefix-only selections as waiting for suffix confirmation" >&2
    exit 1
fi
pending_segment_wait_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\ncontext\t我\npending_segment\twoc\two\t我\tc\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$pending_segment_wait_output" != *$'panel\tlearning\tstatus\tawaiting-suffix-confirmation\tnext-step\twait-for-selected-suffix'* ||
    "$pending_segment_wait_output" != *$'panel\tlearning\tstatus-awaiting-suffix\t1\twoc\two\t我\tc'* ]]; then
    echo "model explain helper should expose awaiting-suffix learning status" >&2
    exit 1
fi
selected_full_consumed_signal_output=$(
    printf 'protocol\t1\npreedit\tpinyinjiu\ncandidates\t品饮酒\t拼音就\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t9\tsource\tprefix-continuation\tscore\t999999\nselected_candidate\t1\t拼音就\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$selected_full_consumed_signal_output" != *$'panel\tlearning\tselected-candidate-signal\tpinyinjiu\t1\t拼音就\twould-learn-preference'* ]]; then
    echo "model explain helper should mark full-consuming consumed-prefix candidates as learnable preferences" >&2
    exit 1
fi
segment_chain_signal_output=$(
    printf 'protocol\t1\npreedit\twoc\ncandidates\t我操\t我曹\nsegment_chain\twoc\two\t我\tc\twocao\t我操\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$segment_chain_signal_output" != *$'panel\tlearning\tsegment-chain-signal\t1\twoc\two\t我\tc\twocao\t我操\tcorrection-chain'* ]]; then
    echo "model explain helper should mark segment chains that imply typo corrections" >&2
    exit 1
fi
segment_chain_suffix_signal_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t从\t操\t曹\ncontext\t我\nsegment_chain\twoc\two\t我\tc\twocao\t我操\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$segment_chain_suffix_signal_output" != *$'panel\tlearning\tsegment-chain-signal\t1\twoc\two\t我\tc\twocao\t我操\tsuffix-correction-chain'* ]]; then
    echo "model explain helper should mark segment chains that apply to the remaining suffix preedit" >&2
    exit 1
fi
pending_segment_confirm_signal_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t0\t操\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$pending_segment_confirm_signal_output" != *$'panel\tlearning\tpending-segment-signal\t1\twoc\two\t我\tc\tawaiting-suffix-confirmation'* ||
    "$pending_segment_confirm_signal_output" != *$'panel\tlearning\tstatus\tawaiting-suffix-confirmation\tnext-step\twait-for-selected-suffix'* ]]; then
    echo "model explain helper should not treat the passive top suffix candidate as confirmed" >&2
    exit 1
fi
pending_segment_explicit_confirm_signal_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t1\t从\n' |
        "$ROOT/scripts/model-explain.sh" --panel -
)
if [[ "$pending_segment_explicit_confirm_signal_output" != *$'panel\tlearning\tpending-segment-signal\t1\twoc\two\t我\tc\tconfirmed-suffix\t从\twoc\t我从'* ]]; then
    echo "model explain helper should mark pending segment rows whose non-top suffix candidate is selected" >&2
    exit 1
fi
learning_panel_window_mock="$tmp_dir/learning-panel-window-mock.sh"
cat >"$learning_panel_window_mock" <<'WINMOCK'
#!/usr/bin/env bash
set -euo pipefail
panel_path=""
title=""
model_config_command=""
personal_train_command=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --title)
            title="${2:-}"
            shift 2
            ;;
        --model-config-command)
            model_config_command="${2:-}"
            shift 2
            ;;
        --personal-train-command)
            personal_train_command="${2:-}"
            shift 2
            ;;
        *)
            if [[ -f "$1" ]] && awk -F '\t' '$1 == "panel" {found=1; exit} END {exit found ? 0 : 1}' "$1"; then
                panel_path="$1"
            fi
            shift
            ;;
    esac
done
test -n "$title"
printf 'window-title\t%s\n' "$title"
printf 'window-path\t%s\n' "$panel_path"
printf 'model-config-command\t%s\n' "$model_config_command"
printf 'personal-train-command\t%s\n' "$personal_train_command"
test -n "$panel_path"
test -x "$model_config_command"
test -x "$personal_train_command"
awk -F '\t' '$1 == "panel" {print; seen=1} END {exit seen ? 0 : 1}' "$panel_path"
WINMOCK
chmod +x "$learning_panel_window_mock"
learning_panel_window_output=$(
    TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_mock" TIPE_LEARNING_PANEL_REFRESH_SECONDS=999 \
        "$ROOT/scripts/learning-panel.sh" --window "$model_explain_sample"
)
if [[ "$learning_panel_window_output" != *$'window-title\tTiPE'* ||
    "$learning_panel_window_output" != *$'window-path\t'* ||
    "$learning_panel_window_output" != *$'model-config-command\t'*"model-config.sh"* ||
    "$learning_panel_window_output" != *$'personal-train-command\t'*"personal-model-train.sh"* ||
    "$learning_panel_window_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$learning_panel_window_output" != *$'panel\tsupervision\tevent-trail\trecent\t2\tlimit\t64\tpurpose\tui-and-short-action-order'* ||
    "$learning_panel_window_output" != *$'panel\tmodel-config\tkind\t'* ||
    "$learning_panel_window_output" != *$'panel\tlearning\tcorrection-signal\t1\tfull-delete-retype\tihao\tnihao'* ||
    "$learning_panel_window_output" != *$'panel\tbehavior\tpossible-correction\t1\tfull-delete-retype\tihao\tnihao'* ]]; then
    echo "learning panel --window should pass a generated panel file to the window process" >&2
    exit 1
fi
supervision_window_output=$(
    TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_mock" TIPE_LEARNING_PANEL_REFRESH_SECONDS=999 \
        "$ROOT/scripts/supervision-window.sh" "$model_explain_sample"
)
if [[ "$supervision_window_output" != *$'window-title\tTiPE'* ||
    "$supervision_window_output" != *$'window-path\t'* ||
    "$supervision_window_output" != *$'panel\tsupervision\tmodel-input\tpreedit\tnihao\tcandidates\t2\tvisible\t2\tnumbered\t1\tcontext\t1\tsegment-chains\t1\tpending-segments\t0'* ||
    "$supervision_window_output" != *$'panel\tmodel-config\tkind\t'* ||
    "$supervision_window_output" != *$'panel\tmodel-replay\tmode\tmanual\tlearn-output\t1\ttrigger\tAnalyze'* ||
    "$supervision_window_output" == *$'panel\tmodel-replay\tstatus\t'* ]]; then
    echo "supervision window helper should open manual Analyze mode without running model replay on open" >&2
    exit 1
fi
live_supervision_cache="$tmp_dir/live-supervision-cache"
mkdir -p "$live_supervision_cache/tipe"
cp "$model_explain_sample" "$live_supervision_cache/tipe/supervision-current.tsv"
supervision_window_live_output=$(
    XDG_CACHE_HOME="$live_supervision_cache" TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_mock" \
        TIPE_LEARNING_PANEL_REFRESH_SECONDS=999 "$ROOT/scripts/supervision-window.sh"
)
if [[ "$supervision_window_live_output" != *$'window-title\tTiPE'* ||
    "$supervision_window_live_output" != *$'window-path\t'* ||
    "$supervision_window_live_output" != *$'panel\tstate\trequest-source\tlive-supervision\t'"$live_supervision_cache/tipe/supervision-current.tsv"$'\n'* ||
    "$supervision_window_live_output" != *$'panel\tstate\tpreedit\tnihao'* ]]; then
    echo "supervision window helper should prefer the live supervision snapshot when no request path is passed" >&2
    exit 1
fi
missing_live_supervision_cache="$tmp_dir/missing-live-supervision-cache"
mkdir -p "$missing_live_supervision_cache/tipe"
supervision_window_wait_output=$(
    XDG_CACHE_HOME="$missing_live_supervision_cache" TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_mock" \
        TIPE_LEARNING_PANEL_REFRESH_SECONDS=999 "$ROOT/scripts/supervision-window.sh"
)
if [[ "$supervision_window_wait_output" != *$'window-title\tTiPE'* ||
    "$supervision_window_wait_output" != *$'window-path\t'* ||
    "$supervision_window_wait_output" != *$'panel\tstate\tstatus\twaiting-for-live-supervision'* ||
    "$supervision_window_wait_output" != *$'panel\tstate\trequest-source\twaiting-for-live-supervision\t'"$missing_live_supervision_cache/tipe/supervision-current.tsv"$'\n'* ||
    "$supervision_window_wait_output" != *$'panel\tsupervision\tmodel-input\tpreedit\t\tcandidates\t0\tvisible\t0\tnumbered\t0\tcontext\t0\tsegment-chains\t0\tpending-segments\t0'* ]]; then
    echo "supervision window helper should open and wait when no live supervision snapshot exists yet" >&2
    exit 1
fi
last_supervision_cache="$tmp_dir/last-supervision-cache"
mkdir -p "$last_supervision_cache/tipe"
cp "$model_explain_sample" "$last_supervision_cache/tipe/supervision-last.tsv"
supervision_window_last_output=$(
    XDG_CACHE_HOME="$last_supervision_cache" TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_mock" \
        TIPE_LEARNING_PANEL_REFRESH_SECONDS=999 "$ROOT/scripts/supervision-window.sh"
)
if [[ "$supervision_window_last_output" != *$'window-title\tTiPE'* ||
    "$supervision_window_last_output" != *$'window-path\t'* ||
    "$supervision_window_last_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$supervision_window_last_output" != *$'panel\tstate\tstatus\tshowing-last-supervision'* ||
    "$supervision_window_last_output" != *$'panel\tstate\trequest-source\tlast-supervision\t'"$last_supervision_cache/tipe/supervision-last.tsv"$'\n'* ]]; then
    echo "supervision window helper should show the last supervision snapshot while waiting for live input" >&2
    exit 1
fi
history_supervision_cache="$tmp_dir/history-supervision-cache"
mkdir -p "$history_supervision_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\told\tcandidates\t1\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\told'
    printf '%s\n' $'candidates\t旧'
    printf '%s\n' $'selected_candidate\t0\t旧'
    printf '%s\n' $'event_counts\tobserved:1'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t1\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\t\tcandidates\t0\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\t'
    printf '%s\n' $'candidates'
    printf '%s\n' $'event_counts\tspace:2\tdelete:1\tcursor-move:1'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t4\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t3\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0\tterminal\t1'
    sed -e $'s/^events\t.*/events\tletter:n\tcursor-move:Down\tcandidate-selected:你号/' \
        -e $'s/^correction_events\t.*/&\tcandidate-selected:你号/' "$model_explain_sample"
} >"$history_supervision_cache/tipe/supervision-history.tsv"
supervision_window_history_output=$(
    XDG_CACHE_HOME="$history_supervision_cache" TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_mock" \
        TIPE_LEARNING_PANEL_REFRESH_SECONDS=999 "$ROOT/scripts/supervision-window.sh"
)
if [[ "$supervision_window_history_output" != *$'window-title\tTiPE'* ||
    "$supervision_window_history_output" != *$'window-path\t'* ||
    "$supervision_window_history_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$supervision_window_history_output" != *$'panel\tstate\tstatus\tshowing-history-supervision'* ||
    "$supervision_window_history_output" != *$'panel\tstate\trequest-source\thistory-supervision\t'* ||
    "$supervision_window_history_output" != *$'panel\thistory\tsummary\trecords\t3\tactive\t2\tpass-through\t1'* ||
    "$supervision_window_history_output" != *$'panel\thistory\tpreedit\t1\tnihao\t1'* ||
    "$supervision_window_history_output" != *$'panel\thistory\tselected-candidate\t1\t你号\t1'* ||
    "$supervision_window_history_output" != *$'panel\thistory\tpreedit-selected\t1\tnihao\t你号\t1'* ||
    "$supervision_window_history_output" != *$'panel\thistory\tactive-event-count\t'*$'\tletter\t1'* ||
    "$supervision_window_history_output" != *$'panel\thistory\tpass-through-event-count\t1\tspace\t2'* ||
    "$supervision_window_history_output" != *$'panel\thistory\tpass-through-event-count\t2\tdelete\t1'* ||
    "$supervision_window_history_output" != *$'panel\thistory\tcorrection\t1\tihao\tnihao\t1'* ||
    "$supervision_window_history_output" != *$'panel\ttraining\tsummary\trecords\t3\tlearnable\t1\tchoices\t1\tobservations\t0'* ]]; then
    echo "supervision window helper should fall back to the latest bounded supervision history request" >&2
    exit 1
fi
if [[ "$supervision_window_history_output" == *$'panel\thistory\tselected-candidate\t'*$'\t旧\t'* ||
    "$supervision_window_history_output" == *$'panel\thistory\tpreedit-selected\t'*$'\told\t旧\t'* ]]; then
    echo "supervision window history panel should not treat passive default selected_candidate rows as learned selections" >&2
    exit 1
fi
if [[ -x "$ROOT/build/tipe-learning-panel-window" ]]; then
    history_window_parse=$(printf '%s\n' "$supervision_window_history_output" |
        sed -n '/^panel\t/p' | "$ROOT/build/tipe-learning-panel-window" --parse-panel -)
    if [[ "$history_window_parse" != *$'history\tsummary\trecords\t3\tactive\t2\tpass-through\t1'* ||
        "$history_window_parse" != *$'history\tselected-candidate\t1\t你号\t1'* ||
        "$history_window_parse" != *$'history\tpreedit-selected\t1\tnihao\t你号\t1'* ||
        "$history_window_parse" != *$'history\tactive-event-count\t'*$'\tletter\t1'* ||
        "$history_window_parse" != *$'history\tpass-through-event-count\t1\tspace\t2'* ||
        "$history_window_parse" != *$'history\tcorrection\t1\tihao\tnihao\t1'* ]]; then
        echo "learning panel window parser should consume bounded supervision history rows" >&2
        exit 1
    fi
    if [[ "$history_window_parse" == *$'history\tselected-candidate\t'*$'\t旧\t'* ||
        "$history_window_parse" == *$'history\tpreedit-selected\t'*$'\told\t旧\t'* ]]; then
        echo "learning panel window parser should not show passive default selected_candidate rows as learned selections" >&2
        exit 1
    fi
fi
history_preference_panel_cache="$tmp_dir/history-preference-panel-cache"
mkdir -p "$history_preference_panel_cache/tipe"
history_preference_request="$history_preference_panel_cache/request.tsv"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' >"$history_preference_request"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcandidate-selected:你号'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcandidate-selected:你号'
} >"$history_preference_panel_cache/tipe/supervision-history.tsv"
history_preference_panel_output=$(
    XDG_CACHE_HOME="$history_preference_panel_cache" "$ROOT/scripts/learning-panel.sh" --raw-panel "$history_preference_request"
)
if [[ "$history_preference_panel_output" != *$'panel\thistory\tlearnable-preference\t1\tnihao\t你号\t2\tpreference\tnihao\t你号\t2'* ]]; then
    echo "learning panel should expose repeated history pairs as learnable preferences" >&2
    exit 1
fi
history_highlight_only_cache="$tmp_dir/history-highlight-only-cache"
mkdir -p "$history_highlight_only_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcursor-move:Down'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcursor-move:Down'
} >"$history_highlight_only_cache/tipe/supervision-history.tsv"
history_highlight_panel_output=$(
    XDG_CACHE_HOME="$history_highlight_only_cache" "$ROOT/scripts/learning-panel.sh" --raw-panel "$history_preference_request"
)
if [[ "$history_highlight_panel_output" == *$'panel\thistory\tlearnable-preference\t'* ]]; then
    echo "learning panel should not treat repeated candidate highlights as confirmed selections" >&2
    exit 1
fi
history_highlight_adapter_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        XDG_CACHE_HOME="$history_highlight_only_cache" TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ -n "$history_highlight_adapter_output" ]]; then
    echo "model adapter should not learn from candidate highlights that were never committed" >&2
    exit 1
fi
history_prefix_only_panel_cache="$tmp_dir/history-prefix-only-panel-cache"
mkdir -p "$history_prefix_only_panel_cache/tipe"
history_prefix_only_request="$history_prefix_only_panel_cache/request.tsv"
printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\n' >"$history_prefix_only_request"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tjixuzuo\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tjixuzuo'
    printf '%s\n' $'candidates\t继续做\t继续'
    printf '%s\n' $'candidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000'
    printf '%s\n' $'candidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999'
    printf '%s\n' $'selected_candidate\t1\t继续'
    printf '%s\n' $'events\tcandidate-selected:继续'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tjixuzuo\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tjixuzuo'
    printf '%s\n' $'candidates\t继续做\t继续'
    printf '%s\n' $'candidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000'
    printf '%s\n' $'candidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999'
    printf '%s\n' $'selected_candidate\t1\t继续'
    printf '%s\n' $'events\tcandidate-selected:继续'
} >"$history_prefix_only_panel_cache/tipe/supervision-history.tsv"
history_prefix_only_panel_output=$(
    XDG_CACHE_HOME="$history_prefix_only_panel_cache" "$ROOT/scripts/learning-panel.sh" --raw-panel "$history_prefix_only_request"
)
if [[ "$history_prefix_only_panel_output" == *$'panel\thistory\tlearnable-preference\t1\tjixuzuo\t继续'* ]]; then
    echo "learning panel should not expose repeated prefix-only history pairs as full-preedit preferences" >&2
    exit 1
fi
if [[ -x "$ROOT/build/tipe-learning-panel-window" ]]; then
    history_preference_window_parse=$(printf '%s\n' "$history_preference_panel_output" |
        sed -n '/^panel\t/p' | "$ROOT/build/tipe-learning-panel-window" --parse-panel -)
    if [[ "$history_preference_window_parse" != *$'history\tlearnable-preference\t1\tnihao\t你号\t2\tpreference\tnihao\t你号\t2'* ]]; then
        echo "learning panel window parser should consume history learnable-preference rows" >&2
        exit 1
    fi
fi
history_model_replay_output=$(
    XDG_CACHE_HOME="$history_supervision_cache" "$ROOT/scripts/model-replay.sh" --command "$ROOT/scripts/model-adapter.sh"
)
if [[ "$history_model_replay_output" != *$'candidate\t你号'* ||
    "$history_model_replay_output" != *$'preference\tnihao\t你号\t2'* ||
    "$history_model_replay_output" == *$'preference\told\t'* ]]; then
    echo "model replay should use the latest bounded supervision history request when no live request exists" >&2
    exit 1
fi
fallback_replay_request="$tmp_dir/fallback-replay-request.tsv"
fallback_replay_preferences="$tmp_dir/fallback-replay-preferences.tsv"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nselected_candidate\t0\t你好\n' >"$fallback_replay_request"
fallback_replay_cache="$tmp_dir/fallback-replay-cache"
mkdir -p "$fallback_replay_cache/tipe"
cp "$fallback_replay_request" "$fallback_replay_cache/tipe/supervision-last.tsv"
fallback_replay_output=$(
    XDG_CACHE_HOME="$fallback_replay_cache" "$ROOT/scripts/learning-panel.sh" --raw-panel --wait-missing \
        --fallback-request "$fallback_replay_cache/tipe/supervision-last.tsv" --replay --explain-output \
        --learn-output --preferences "$fallback_replay_preferences" --command "$ROOT/scripts/model-adapter.sh" \
        "$fallback_replay_cache/tipe/supervision-current.tsv"
)
if [[ "$fallback_replay_output" != *$'panel\tstate\tstatus\tshowing-last-supervision'* ||
    "$fallback_replay_output" != *$'panel\tmodel-replay\tstatus\tok'* ||
    "$fallback_replay_output" != *$'panel\tmodel-output\tlearned\tpreferences\t0\tcorrections\t0\tsegment-chains\t0\tpath\t'"$fallback_replay_preferences"* ||
    "$fallback_replay_output" != *$'panel\tmodel-output\tnote\tselected-candidate-already-top\tnihao\t0\t你好'* ||
    -s "$fallback_replay_preferences" ]]; then
    echo "learning panel replay should analyze fallback supervision without self-learning the default top candidate" >&2
    exit 1
fi
top_echo_request="$tmp_dir/top-echo-request.tsv"
top_echo_preferences="$tmp_dir/top-echo-preferences.tsv"
top_echo_stderr="$tmp_dir/top-echo.err"
top_echo_model="$tmp_dir/top-echo-model.sh"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' >"$top_echo_request"
cat >"$top_echo_model" <<'TOP'
#!/usr/bin/env bash
printf '%s\n' $'candidate\t你好'
TOP
chmod +x "$top_echo_model"
top_echo_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$top_echo_request" --command "$top_echo_model" \
        --learn-output --preferences "$top_echo_preferences" 2>"$top_echo_stderr"
)
if [[ "$top_echo_output" != $'candidate\t你好' ||
    "$(cat "$top_echo_stderr")" != *$'model-output-learned\tpreferences\t0\tcorrections\t0\tsegment-chains\t0\tpath\t'"$top_echo_preferences"* ||
    -s "$top_echo_preferences" ]]; then
    echo "model replay should not persist a candidate row that only echoes the current top candidate" >&2
    exit 1
fi
self_amplify_request="$tmp_dir/self-amplify-request.tsv"
self_amplify_preferences="$tmp_dir/self-amplify-preferences.tsv"
self_amplify_stderr="$tmp_dir/self-amplify.err"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\npreference\tnihao\t你号\t5\n' >"$self_amplify_request"
self_amplify_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$self_amplify_request" --command "$ROOT/scripts/model-adapter.sh" \
        --learn-output --preferences "$self_amplify_preferences" 2>"$self_amplify_stderr"
)
if [[ "$self_amplify_output" != $'candidate\t你号' ||
    "$(cat "$self_amplify_stderr")" != *$'model-output-learned\tpreferences\t0\tcorrections\t0\tsegment-chains\t0\tpath\t'"$self_amplify_preferences"* ||
    -s "$self_amplify_preferences" ]]; then
    echo "model replay should not re-learn preference rows that were already present in the request" >&2
    exit 1
fi
explicit_echo_request="$tmp_dir/explicit-echo-request.tsv"
explicit_echo_preferences="$tmp_dir/explicit-echo-preferences.tsv"
explicit_echo_stderr="$tmp_dir/explicit-echo.err"
explicit_echo_model="$tmp_dir/explicit-echo-model.sh"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nselected_candidate\t1\t你号\npreference\tnihao\t你号\t5\ncorrection\tihao\tnihao\t2\nsegment_chain\tnihao\tni\t你\thao\tnihao\t你号\n' >"$explicit_echo_request"
cat >"$explicit_echo_model" <<'ECHO'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' \
    $'preference\tnihao\t你号\t5' \
    $'correction\tihao\tnihao' \
    $'segment_chain\tnihao\tni\t你\thao\tnihao\t你号\t5'
ECHO
chmod +x "$explicit_echo_model"
explicit_echo_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$explicit_echo_request" --command "$explicit_echo_model" \
        --learn-output --preferences "$explicit_echo_preferences" 2>"$explicit_echo_stderr"
)
if [[ "$explicit_echo_output" != $'preference\tnihao\t你号\t5\ncorrection\tihao\tnihao\nsegment_chain\tnihao\tni\t你\thao\tnihao\t你号\t5' ||
    "$(cat "$explicit_echo_stderr")" != *$'model-output-learned\tpreferences\t0\tcorrections\t0\tsegment-chains\t0\tpath\t'"$explicit_echo_preferences"* ||
    -s "$explicit_echo_preferences" ]]; then
    echo "model replay should not persist explicit model rows that only echo request learning evidence" >&2
    exit 1
fi
existing_only_echo_request="$tmp_dir/existing-only-echo-request.tsv"
existing_only_echo_preferences="$tmp_dir/existing-only-echo-preferences.tsv"
existing_only_echo_stderr="$tmp_dir/existing-only-echo.err"
existing_only_echo_model="$tmp_dir/existing-only-echo-model.sh"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' >"$existing_only_echo_request"
printf 'nihao\t你号\t4\n__correction__\tihao\tnihao\t2\n__segment_chain__\tnihao\tni\t你\thao\tnihao\t你号\t3\n' >"$existing_only_echo_preferences"
cat >"$existing_only_echo_model" <<'ECHO'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' \
    $'preference\tnihao\t你号\t5' \
    $'correction\tihao\tnihao' \
    $'segment_chain\tnihao\tni\t你\thao\tnihao\t你号\t5'
ECHO
chmod +x "$existing_only_echo_model"
existing_only_echo_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$existing_only_echo_request" --command "$existing_only_echo_model" \
        --learn-output --preferences "$existing_only_echo_preferences" 2>"$existing_only_echo_stderr"
)
existing_only_echo_after=$(cat "$existing_only_echo_preferences")
if [[ "$existing_only_echo_output" != $'preference\tnihao\t你号\t5\ncorrection\tihao\tnihao\nsegment_chain\tnihao\tni\t你\thao\tnihao\t你号\t5' ||
    "$(cat "$existing_only_echo_stderr")" != *$'model-output-learned\tpreferences\t0\tcorrections\t0\tsegment-chains\t0\tpath\t'"$existing_only_echo_preferences"* ||
    "$existing_only_echo_after" != $'nihao\t你号\t4\n__correction__\tihao\tnihao\t2\n__segment_chain__\tnihao\tni\t你\thao\tnihao\t你号\t3' ]]; then
    echo "model replay should not amplify explicit model rows that echo existing preference-file evidence omitted from the request" >&2
    exit 1
fi
prefix_preference_request="$tmp_dir/prefix-preference-request.tsv"
prefix_preference_model="$tmp_dir/prefix-preference-model.sh"
prefix_preference_preferences="$tmp_dir/prefix-preference-preferences.tsv"
prefix_preference_stderr="$tmp_dir/prefix-preference.err"
printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\n' >"$prefix_preference_request"
cat >"$prefix_preference_model" <<'MODEL'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'preference\tjixuzuo\t继续\t4'
MODEL
chmod +x "$prefix_preference_model"
if "$ROOT/scripts/model-replay.sh" --request "$prefix_preference_request" --command "$prefix_preference_model" \
    --learn-output --preferences "$prefix_preference_preferences" >"$tmp_dir/prefix-preference.out" 2>"$prefix_preference_stderr"; then
    echo "model replay should reject prefix-only candidate preferences before learning" >&2
    exit 1
fi
if [[ "$(cat "$prefix_preference_stderr")" != *'preference candidate is prefix-only'* ||
    ( -e "$prefix_preference_preferences" && -s "$prefix_preference_preferences" ) ]]; then
    echo "model replay should not persist rejected prefix-only candidate preferences" >&2
    exit 1
fi
full_consumed_preference_request="$tmp_dir/full-consumed-preference-request.tsv"
full_consumed_preference_model="$tmp_dir/full-consumed-preference-model.sh"
full_consumed_preference_preferences="$tmp_dir/full-consumed-preference-preferences.tsv"
full_consumed_preference_stderr="$tmp_dir/full-consumed-preference.err"
printf 'protocol\t1\npreedit\tpinyinjiu\ncandidates\t品饮酒\t拼音就\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t9\tsource\tprefix-continuation\tscore\t999999\n' >"$full_consumed_preference_request"
cat >"$full_consumed_preference_model" <<'MODEL'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'preference\tpinyinjiu\t拼音就\t4'
MODEL
chmod +x "$full_consumed_preference_model"
full_consumed_preference_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$full_consumed_preference_request" \
        --command "$full_consumed_preference_model" --learn-output \
        --preferences "$full_consumed_preference_preferences" 2>"$full_consumed_preference_stderr"
)
if [[ "$full_consumed_preference_output" != $'preference\tpinyinjiu\t拼音就\t4' ||
    "$(cat "$full_consumed_preference_stderr")" != *$'model-output-learned\tpreferences\t1\tcorrections\t0\tsegment-chains\t0\tpath\t'"$full_consumed_preference_preferences"* ||
    "$(cat "$full_consumed_preference_preferences")" != $'pinyinjiu\t拼音就\t4' ]]; then
    echo "model replay should accept full-consuming consumed-prefix preferences: output=$full_consumed_preference_output stderr=$(cat "$full_consumed_preference_stderr") prefs=$(cat "$full_consumed_preference_preferences" 2>/dev/null)" >&2
    exit 1
fi
full_text_prefix_request="$tmp_dir/full-text-prefix-request.tsv"
full_text_prefix_model="$tmp_dir/full-text-prefix-model.sh"
full_text_prefix_preferences="$tmp_dir/full-text-prefix-preferences.tsv"
full_text_prefix_stderr="$tmp_dir/full-text-prefix.err"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你好啊\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t0\tsource\tfull\tscore\t999999\n' >"$full_text_prefix_request"
cat >"$full_text_prefix_model" <<'MODEL'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'preference\tnihao\t你好\t4'
MODEL
chmod +x "$full_text_prefix_model"
full_text_prefix_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$full_text_prefix_request" \
        --command "$full_text_prefix_model" --learn-output \
        --preferences "$full_text_prefix_preferences" 2>"$full_text_prefix_stderr"
)
if [[ "$full_text_prefix_output" != $'preference\tnihao\t你好\t4' ||
    "$(cat "$full_text_prefix_stderr")" != *$'model-output-learned\tpreferences\t1\tcorrections\t0\tsegment-chains\t0\tpath\t'"$full_text_prefix_preferences"* ||
    "$(cat "$full_text_prefix_preferences")" != $'nihao\t你好\t4' ]]; then
    echo "model replay should trust full-consumption metadata when another candidate has the same text prefix" >&2
    exit 1
fi
concurrent_preferences="$tmp_dir/concurrent-preferences.tsv"
concurrent_engine_output="$tmp_dir/concurrent-engine.out"
concurrent_engine_stderr="$tmp_dir/concurrent-engine.err"
concurrent_model_output="$tmp_dir/concurrent-model.out"
concurrent_model_stderr="$tmp_dir/concurrent-model.err"
exec {concurrent_lock_fd}>"$concurrent_preferences.lock"
flock -x "$concurrent_lock_fd"
"$ROOT/build/tipe-state-probe" nihao --preferences "$concurrent_preferences" --select 你好 \
    >"$concurrent_engine_output" 2>"$concurrent_engine_stderr" &
concurrent_engine_pid=$!
"$ROOT/scripts/model-replay.sh" --request "$full_consumed_preference_request" \
    --command "$full_consumed_preference_model" --learn-output --preferences "$concurrent_preferences" \
    >"$concurrent_model_output" 2>"$concurrent_model_stderr" &
concurrent_model_pid=$!
sleep 0.2
flock -u "$concurrent_lock_fd"
exec {concurrent_lock_fd}>&-
if ! wait "$concurrent_engine_pid"; then
    echo "state probe failed during concurrent preference write: $(cat "$concurrent_engine_stderr")" >&2
    exit 1
fi
if ! wait "$concurrent_model_pid"; then
    echo "model replay failed during concurrent preference write: $(cat "$concurrent_model_stderr")" >&2
    exit 1
fi
if ! rg -q $'^nihao\t你好\t[1-9][0-9]*$' "$concurrent_preferences" ||
    ! rg -q $'^pinyinjiu\t拼音就\t4$' "$concurrent_preferences"; then
    echo "engine and model replay should preserve both writes to a shared preference file" >&2
    exit 1
fi
if compgen -G "$concurrent_preferences.tmp.*" >/dev/null; then
    echo "concurrent preference writes should not leave temporary files behind" >&2
    exit 1
fi
known_correction_request="$tmp_dir/known-correction-request.tsv"
known_correction_preferences="$tmp_dir/known-correction-preferences.tsv"
known_correction_stderr="$tmp_dir/known-correction.err"
printf 'protocol\t1\npreedit\tihao\ncandidates\t你好\t你号\ncorrection\tihao\tnihao\t3\n' >"$known_correction_request"
known_correction_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$known_correction_request" --command "$ROOT/scripts/model-adapter.sh" \
        --learn-output --preferences "$known_correction_preferences" 2>"$known_correction_stderr"
)
if [[ "$known_correction_output" != $'correction\tihao\tnihao' ||
    "$(cat "$known_correction_stderr")" != *$'model-output-learned\tpreferences\t0\tcorrections\t0\tsegment-chains\t0\tpath\t'"$known_correction_preferences"* ||
    -s "$known_correction_preferences" ]]; then
    echo "model replay should not re-learn correction rows that were already present in the request" >&2
    exit 1
fi
learning_panel_live_output=$(
    XDG_CACHE_HOME="$live_supervision_cache" "$ROOT/scripts/learning-panel.sh"
)
if [[ "$learning_panel_live_output" != *$'state\tpreedit\tnihao'* ||
    "$learning_panel_live_output" != *$'supervision\tmodel-input\tpreedit\tnihao\tcandidates\t2\tvisible\t2\tnumbered\t1\tcontext\t1\tsegment-chains\t1\tpending-segments\t0'* ]]; then
    echo "learning panel helper should prefer the live supervision snapshot when no request path is passed" >&2
    exit 1
fi
if [[ -x "$ROOT/build/tipe-learning-panel-window" ]]; then
    learning_panel_window_parse=$(printf '%s\n' "$learning_panel_raw_output" |
        "$ROOT/build/tipe-learning-panel-window" --parse-panel -)
    if [[ "$learning_panel_window_parse" != *$'state\tpreedit\tnihao'* ||
        "$learning_panel_window_parse" != *$'segment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好'* ||
        "$learning_panel_window_parse" != *$'learning\tcorrection-signal\t1\tfull-delete-retype\tihao\tnihao'* ||
        "$learning_panel_window_parse" != *$'behavior\tpossible-correction\t1\tfull-delete-retype\tihao\tnihao'* ]]; then
        echo "learning panel raw rows should be consumable by the GTK window parser" >&2
        exit 1
    fi
fi
learning_panel_model="$tmp_dir/learning-panel-model.sh"
cat >"$learning_panel_model" <<'LEARNMODEL'
#!/usr/bin/env bash
cat >/dev/null
printf 'candidate	你号\n'
LEARNMODEL
chmod +x "$learning_panel_model"
learning_panel_replay_output=$(
    "$ROOT/scripts/learning-panel.sh" --replay --check --command "$learning_panel_model" "$model_explain_sample"
)
if [[ "$learning_panel_replay_output" != *$'模型回放'* ||
    "$learning_panel_replay_output" != *$'replay-check\twrapper-ok\t'"$learning_panel_model"$'\trows\t1'* ||
    "$learning_panel_replay_output" != *$'replay-output\tcandidate\t你号'* ]]; then
    echo "learning panel helper should replay and check a selected model command without changing fcitx5" >&2
    exit 1
fi
model_explain_raw_hint_output=$(
    printf 'protocol\t1\npreedit\tstarted\nsurrounding_before\tconst taskStatus = \n' |
        "$ROOT/scripts/model-explain.sh"
)
if [[ "$model_explain_raw_hint_output" != *$'behavior_raw_english_hint\t0\tnone'* ]]; then
    echo "model explain helper should not report app/surrounding raw English hints" >&2
    exit 1
fi
model_explain_learned_raw_hint_output=$(
    printf 'protocol\t1\npreedit\tstarted\ncandidates\t三他人特定\tstarted\npreference\tstarted\tstarted\t3\n' |
        "$ROOT/scripts/model-explain.sh"
)
if [[ "$model_explain_learned_raw_hint_output" != *$'behavior_raw_english_hint\t1\tlearned-raw-preference\tcount\t3'* ]]; then
    echo "model explain helper should report learned raw-English preference evidence" >&2
    exit 1
fi
model_explain_cursor_insert_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncorrection_events\tletter:n\tletter:h\tletter:a\tletter:o\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tletter:i\n' |
        "$ROOT/scripts/model-explain.sh"
)
if [[ "$model_explain_cursor_insert_output" != *$'behavior_possible_correction\tmiddle-edit\tnhao\tnihao'* ]]; then
    echo "model explain helper should summarize cursor-insert correction behavior" >&2
    exit 1
fi
model_explain_delete_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncorrection_events\tletter:n\tletter:i\tletter:y\tletter:h\tletter:a\tletter:o\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tdelete:\n' |
        "$ROOT/scripts/model-explain.sh"
)
if [[ "$model_explain_delete_output" != *$'behavior_possible_correction\tmiddle-edit\tniyhao\tnihao'* ]]; then
    echo "model explain helper should summarize delete-key correction behavior" >&2
    exit 1
fi
model_explain_delete_erase_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tcursor-move:Left\tdelete:\tdelete:\tdelete:\tdelete:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\n' |
        "$ROOT/scripts/model-explain.sh"
)
if [[ "$model_explain_delete_erase_output" != *$'behavior_possible_correction\tfull-delete-retype\tihao\tnihao'* ]]; then
    echo "model explain helper should summarize Delete full-erase correction behavior" >&2
    exit 1
fi
rm -f /tmp/tipe/model-request.tsv
printf 'protocol\t1\npreedit\tnihao\n' |
    env -u HOME -u XDG_CACHE_HOME -u TIPE_MODEL_DUMP_PATH "$ROOT/scripts/model-dump.sh" >/dev/null
if ! cmp -s /tmp/tipe/model-request.tsv <(printf 'protocol\t1\npreedit\tnihao\n') ||
    [[ "$(stat -c '%a' /tmp/tipe/model-request.tsv)" != "600" ]]; then
    echo "model dump helper should use a private atomic fallback when HOME and XDG_CACHE_HOME are unset" >&2
    exit 1
fi
model_config_path="$tmp_dir/model-env"
model_config_show=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --show)
if [[ "$model_config_show" != *"config"$'\t'"$model_config_path"* ||
    "$model_config_show" != *"status"$'\t'"missing"* ||
    "$model_config_show" != *$'model-status\tconfigured-mode\theuristic'* ||
    "$model_config_show" != *$'model-status\tkind\toffline-heuristic'* ||
    "$model_config_show" != *$'model-status\ttimeout\t2'* ||
    "$model_config_show" != *$'model-status\thttp-timeout\t8'* ||
    "$model_config_show" != *$'model-status\ttemperature\t0'* ||
    "$model_config_show" != *$'model-status\tmax-tokens\t128'* ||
    "$model_config_show" != *$'model-status\tclick-trigger\tF9'* ||
    "$model_config_show" != *$'model-status\tcontinuous-mode\tlocal-light-rerank'* ||
    "$model_config_show" != *$'model-status\tcontinuous-default\t0'* ||
    "$model_config_show" != *$'model-status\ttraining-context\t0'* ||
    "$model_config_show" != *$'model-status\ttraining-surrounding\t0'* ||
    "$model_config_show" != *$'model-status\tcontinuous-toggle\tShift+F9'* ||
    "$model_config_show" != *$'model-status\tanalyze-window\t'*"tipe-analyze-window"* ||
    "$model_config_show" != *$'model-status\tsupervision-window\t'*"tipe-supervision-window"* ||
    "$model_config_show" != *$'model-status\tanalyze-learn\t'*"tipe-analyze-window --learn-output"* ||
    "$model_config_show" != *$'model-status\tself-test-command\t'*"tipe-model-self-test --current --config $model_config_path"* ||
    "$model_config_show" != *$'model-status\tdry-run-test-command\t'*"tipe-model-self-test --current --config $model_config_path --adapter-dry-run"* ||
    "$model_config_show" != *$'model-status\tconfigured-command\tunset'* ||
    "$model_config_show" != *$'model-status\tconfigured-command-valid\t0'* ||
    "$model_config_show" != *$'model-status\tprocess-command\tunset'* ||
    "$model_config_show" != *$'model-status\tprocess-command-active-scope\tcurrent-shell-only-not-fcitx5-runtime'* ||
    "$model_config_show" != *$'model-status\truntime-verification\ttipe-doctor'* ||
    "$model_config_show" != *$'model-status\tprocess-command-active\t0'* ||
    "$model_config_show" != *$'model-status\tactivation-hint\ttipe-restart-fcitx5'* ||
    "$model_config_show" != *"restart-env"$'\t'"TIPE_MODEL_COMMAND="* ]]; then
    echo "model config helper should show a missing config without writing it" >&2
    exit 1
fi
model_current_help=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-current.sh" --help)
model_current_show=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-current.sh" --show)
model_current_env=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-current.sh" --print-env)
if [[ "$model_current_help" != *'Usage:'* ||
    "$model_current_show" != *"config"$'\t'"$model_config_path"* ||
    "$model_current_show" != *$'status\tmissing'* ||
    "$model_current_show" != *$'mode\theuristic'* ||
    "$model_current_show" != *$'kind\toffline-heuristic'* ||
    "$model_current_show" != *$'configured-command\tunset'* ||
    "$model_current_show" != *$'configured-command-valid\t0'* ||
    "$model_current_show" != *$'process-command\tunset'* ||
    "$model_current_show" != *$'process-command-scope\tcurrent-shell-environment'* ||
    "$model_current_show" != *$'process-command-active-scope\tcurrent-shell-only-not-fcitx5-runtime'* ||
    "$model_current_show" != *$'runtime-verification\ttipe-doctor'* ||
    "$model_current_show" != *$'process-command-active\t0'* ||
    "$model_current_show" != *$'activation-hint\ttipe-restart-fcitx5'* ||
    "$model_current_show" != *$'click-trigger\tF9'* ||
    "$model_current_show" != *$'continuous-mode\tlocal-light-rerank'* ||
    "$model_current_show" != *$'continuous-toggle\tShift+F9'* ||
    "$model_current_show" != *$'timeout\t2'* ||
    "$model_current_show" != *$'http-timeout\t8'* ||
    "$model_current_show" != *$'temperature\t0'* ||
    "$model_current_show" != *$'max-tokens\t128'* ||
    "$model_current_show" != *$'training-context\t0'* ||
    "$model_current_show" != *$'training-surrounding\t0'* ||
    "$model_current_env" != *'export TIPE_MODEL_COMMAND='*'tipe-model-current'* ||
    "$model_current_env" != *"export TIPE_MODEL_CONFIG=$model_config_path"* ||
    "$model_current_env" != *'export TIPE_MODEL_MODE=heuristic'* ]]; then
    echo "model current helper should expose useful status and environment diagnostics" >&2
    exit 1
fi
model_config_source_active_show=$(
    TIPE_MODEL_COMMAND="$ROOT/scripts/model-current.sh" TIPE_MODEL_CONFIG="$model_config_path" \
        "$ROOT/scripts/model-config.sh" --show
)
if [[ "$model_config_source_active_show" != *$'model-status\tprocess-command\t'*"model-current.sh"* ||
    "$model_config_source_active_show" != *$'model-status\tprocess-command-active\t1'* ]]; then
    echo "model config helper should recognize source-tree model-current as active" >&2
    exit 1
fi
model_config_env_active_show=$(
    TIPE_MODEL_COMMAND="TIPE_MODEL_BACKEND=heuristic $HOME/.local/bin/tipe-model-current" \
        TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --show
)
if [[ "$model_config_env_active_show" != *$'model-status\tprocess-command-active\t1'* ]]; then
    echo "model config helper should recognize env-prefixed installed model-current as active" >&2
    exit 1
fi
model_config_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write ollama \
    --base-url http://127.0.0.1:11434/v1 --model qwen-test --timeout 3 --http-timeout 4 --dry-run)
if [[ "$model_config_dry_run" != *'export TIPE_MODEL_MODE=ollama'* ||
    "$model_config_dry_run" != *'export TIPE_MODEL_BACKEND=ollama'* ||
    "$model_config_dry_run" != *'export TIPE_MODEL_NAME=qwen-test'* ||
    "$model_config_dry_run" != *'export TIPE_MODEL_TIMEOUT_SECONDS=3'* ||
    -e "$model_config_path" ]]; then
    echo "model config helper dry-run should print ollama config without writing" >&2
    exit 1
fi
llama_model_file="$tmp_dir/qwen-test.gguf"
printf '%s\n' 'fake-gguf' >"$llama_model_file"
llama_command_file="$tmp_dir/llama-cli"
cat >"$llama_command_file" <<'LLAMACLI'
#!/usr/bin/env bash
set -euo pipefail
model=""
prompt=""
threads=""
context=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -m) model="$2"; shift 2 ;;
        -f) prompt="$2"; shift 2 ;;
        -t) threads="$2"; shift 2 ;;
        -c) context="$2"; shift 2 ;;
        *) shift ;;
    esac
done
[[ "$model" == "$TIPE_TEST_LLAMA_MODEL" ]]
[[ "$threads" == "4" && "$context" == "4096" ]]
grep -q '"supervision_mode": "active-preedit"' "$prompt"
printf 'candidate\t你号\ncorrection\tihao\tnihao\ncandidate\t伪造\n'
LLAMACLI
chmod +x "$llama_command_file"
llama_config="$tmp_dir/llama-model-env"
llama_config_dry_run=$(TIPE_MODEL_CONFIG="$llama_config" "$ROOT/scripts/model-config.sh" \
    --write llama-cpp --model "$llama_model_file" --llama-command "$llama_command_file" \
    --llama-threads 4 --llama-context 4096 --dry-run --test-dry-run)
if [[ "$llama_config_dry_run" != *'export TIPE_MODEL_MODE=llama-cpp'* ||
    "$llama_config_dry_run" != *"export TIPE_MODEL_NAME=$llama_model_file"* ||
    "$llama_config_dry_run" != *"export TIPE_LLAMA_CPP_COMMAND=$llama_command_file"* ||
    "$llama_config_dry_run" != *'export TIPE_LLAMA_CPP_THREADS=4'* ||
    "$llama_config_dry_run" != *'export TIPE_LLAMA_CPP_CONTEXT=4096'* ||
    "$llama_config_dry_run" != *'export TIPE_MODEL_TIMEOUT_SECONDS=30'* ||
    "$llama_config_dry_run" != *$'model-dry-run-request\tllama-cpp:'"$llama_model_file"* ||
    -e "$llama_config" ]]; then
    echo "llama.cpp config dry-run should validate one-shot local model settings without loading a model" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$llama_config" "$ROOT/scripts/model-config.sh" --write llama-cpp \
    --model "$llama_model_file" --llama-command "$llama_command_file" \
    --llama-threads 4 --llama-context 4096 >/dev/null
llama_config_show=$(TIPE_MODEL_CONFIG="$llama_config" "$ROOT/scripts/model-config.sh" --show)
llama_current_show=$(TIPE_MODEL_CONFIG="$llama_config" "$ROOT/scripts/model-current.sh" --show)
llama_doctor_show=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$llama_config" "$ROOT/scripts/doctor.sh" --no-runtime)
llama_current_output=$(printf 'protocol\t1\npreedit\tihao\ncandidates\t你好\t你号\n' |
    TIPE_TEST_LLAMA_MODEL="$llama_model_file" TIPE_MODEL_CONFIG="$llama_config" \
        "$ROOT/scripts/model-current.sh")
llama_restart_dry_run=$(TIPE_MODEL_CONFIG="$llama_config" \
    "$ROOT/scripts/restart-fcitx5.sh" --model-current --dry-run)
if [[ "$llama_config_show" != *$'model-status\tkind\tlocal-llama-cpp'* ||
    "$llama_config_show" != *$'model-status\ttimeout\t30'* ||
    "$llama_config_show" != *$'model-status\tinvocation\ton-demand-single-process'* ||
    "$llama_config_show" != *$'model-status\tllama-command-valid\t1'* ||
    "$llama_config_show" != *$'model-status\tllama-model-readable\t1'* ||
    "$llama_config_show" != *$'model-status\tllama-threads\t4'* ||
    "$llama_config_show" != *$'model-status\tllama-context\t4096'* ||
    "$llama_config_show" != *$'model-status\tdry-run-test-supported\t1'* ||
    "$llama_current_show" != *$'kind\tlocal-llama-cpp'* ||
    "$llama_current_show" != *$'invocation\ton-demand-single-process'* ||
    "$llama_current_show" != *$'llama-threads\t4'* ||
    "$llama_current_show" != *$'llama-context\t4096'* ||
    "$llama_doctor_show" != *$'model\tkind\tlocal-llama-cpp'* ||
    "$llama_doctor_show" != *$'model\ttimeout\t30'* ||
    "$llama_doctor_show" != *$'model\tllama-command-valid\t1'* ||
    "$llama_doctor_show" != *$'model\tllama-model-readable\t1'* ||
    "$llama_current_output" != $'candidate\t你号\ncorrection\tihao\tnihao' ||
    "$llama_restart_dry_run" != *'TIPE_MODEL_TIMEOUT_SECONDS=30'* ]]; then
    echo "llama.cpp mode should configure, diagnose, invoke, filter, and preserve the engine timeout" >&2
    exit 1
fi
personal_model_file="$tmp_dir/personal-reranker.json"
printf '%s\n' '{"architecture":"hashed-pairwise-ranker+personal-edit-channel+raw-token-memory+raw-offer-profile+pinyin-prior","baseline_weight":0.05,"dimension":1024,"feature_version":3,"name":"TiP","pinyin_prior":{"nihao":12},"raw_token_evidence":{"eeeeeeeeeeeeeeeeeeeeeeee":3},"schema":"tipe.personal-reranker.v1","training":{"active_raw_token_evidence":1,"correction_only_samples":2,"pinyin_prior_sources":1,"ranking_samples":2,"raw_profile_auxiliary_positive_samples":3,"raw_token_evidence_entries":1,"samples":4,"validation_generic_excluded_direct_evidence":1,"validation_generic_excluded_seen_preedit":1,"validation_generic_excluded_raw_candidate":0,"validation_generic_non_leading_accuracy":0.5,"validation_generic_non_leading_correct":1,"validation_generic_non_leading_samples":2,"validation_strategy":"capability-isolated-temporal-v3"},"weights":{}}' \
    >"$personal_model_file"
personal_model_config="$tmp_dir/personal-model-env"
TIPE_MODEL_CONFIG="$personal_model_config" "$ROOT/scripts/model-config.sh" --write personal \
    --personal-model "$personal_model_file" >/dev/null
personal_config_show=$(TIPE_MODEL_CONFIG="$personal_model_config" "$ROOT/scripts/model-config.sh" --show)
personal_current_show=$(TIPE_MODEL_CONFIG="$personal_model_config" "$ROOT/scripts/model-current.sh" --show)
personal_current_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        TIPE_MODEL_CONFIG="$personal_model_config" "$ROOT/scripts/model-current.sh"
)
if [[ "$personal_config_show" != *$'model-status\tconfigured-mode\tpersonal'* ||
    "$personal_config_show" != *$'model-status\tbackend\tpersonal'* ||
    "$personal_config_show" != *$'model-status\tkind\tpersonal-reranker'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model\t'*"$personal_model_file"* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-status\tready'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-name\tTiP'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-architecture\thashed-pairwise-ranker+personal-edit-channel+raw-token-memory+raw-offer-profile+pinyin-prior'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-feature-version\t3'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-training-raw-profile-auxiliary-positive-samples\t3'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-raw-token-evidence\t1'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-active-raw-token-evidence\t1'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-pinyin-prior-entries\t1'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-training-pinyin-prior-sources\t1'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-training-samples\t4'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-training-ranking-samples\t2'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-training-correction-only-samples\t2'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-training-validation-strategy\tcapability-isolated-temporal-v3'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-training-validation-generic-non-leading-samples\t2'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-active-correction-patterns\t0'* ||
    "$personal_config_show" != *$'model-status\tpersonal-model-active-key-habits\t0'* ||
    "$personal_current_show" != *$'mode\tpersonal'* ||
    "$personal_current_show" != *$'kind\tpersonal-reranker'* ||
    "$personal_current_show" != *$'personal-model\t'*"$personal_model_file"* ||
    "$personal_current_show" != *$'personal-model-name\tTiP'* ||
    "$personal_current_show" != *$'personal-model-architecture\thashed-pairwise-ranker+personal-edit-channel+raw-token-memory+raw-offer-profile+pinyin-prior'* ||
    "$personal_current_show" != *$'personal-model-feature-version\t3'* ||
    "$personal_current_show" != *$'personal-model-training-raw-profile-auxiliary-positive-samples\t3'* ||
    "$personal_current_show" != *$'personal-model-raw-token-evidence\t1'* ||
    "$personal_current_show" != *$'personal-model-active-raw-token-evidence\t1'* ||
    "$personal_current_show" != *$'personal-model-pinyin-prior-entries\t1'* ||
    "$personal_current_show" != *$'personal-model-training-samples\t4'* ||
    "$personal_current_show" != *$'personal-model-training-ranking-samples\t2'* ||
    "$personal_current_show" != *$'personal-model-training-correction-only-samples\t2'* ||
    "$personal_current_show" != *$'personal-model-training-validation-strategy\tcapability-isolated-temporal-v3'* ||
    "$personal_current_show" != *$'personal-model-training-validation-generic-excluded-direct-evidence\t1'* ||
    "$personal_current_show" != *$'personal-model-active-key-habits\t0'* ||
    "$personal_current_output" != $'candidate\t你好\ncandidate\t你号' ]]; then
    echo "personal model mode should configure and invoke the safe local reranker" >&2
    exit 1
fi
personal_model_self_test=$(
    "$ROOT/scripts/model-self-test.sh" --current --config "$personal_model_config"
)
if [[ "$personal_model_self_test" != *$'self-test-command\t'*"model-current.sh"* ||
    "$personal_model_self_test" != *$'wrapper-ok\t'*"model-current.sh"* ||
    "$personal_model_self_test" != *$'pending-check\twrapper-ok\t'*"model-current.sh"* ]]; then
    echo "model self-test should validate a configured personal model" >&2
    exit 1
fi
if personal_dry_run_error=$(
    "$ROOT/scripts/model-self-test.sh" --current --config "$personal_model_config" --adapter-dry-run 2>&1
); then
    echo "personal model mode should reject adapter request dry-run" >&2
    exit 1
fi
if [[ "$personal_dry_run_error" != *'--adapter-dry-run is not supported for configured TiPE model mode: personal'* ||
    "$personal_dry_run_error" != *'Run the current model self-test without --adapter-dry-run.'* ]]; then
    echo "unsupported personal-model adapter dry-run should fail with an actionable error" >&2
    exit 1
fi
missing_personal_config="$tmp_dir/missing-personal-model-env"
TIPE_MODEL_CONFIG="$missing_personal_config" "$ROOT/scripts/model-config.sh" --write personal \
    --personal-model "$tmp_dir/not-trained.json" >/dev/null
missing_personal_show=$(TIPE_MODEL_CONFIG="$missing_personal_config" "$ROOT/scripts/model-config.sh" --show)
if [[ "$missing_personal_show" != *$'model-status\tpersonal-model-status\tuntrained'* ]]; then
    echo "personal model config should expose an untrained model path" >&2
    exit 1
fi
invalid_personal_file="$tmp_dir/invalid-personal-model.json"
printf '%s\n' '{"schema":"wrong","weights":{}}' >"$invalid_personal_file"
invalid_personal_config="$tmp_dir/invalid-personal-model-env"
TIPE_MODEL_CONFIG="$invalid_personal_config" "$ROOT/scripts/model-config.sh" --write personal \
    --personal-model "$invalid_personal_file" >/dev/null
invalid_personal_show=$(TIPE_MODEL_CONFIG="$invalid_personal_config" "$ROOT/scripts/model-config.sh" --show)
if [[ "$invalid_personal_show" != *$'model-status\tpersonal-model-status\tinvalid'* ]]; then
    echo "personal model config should reject a readable but invalid model file" >&2
    exit 1
fi
custom_model="$tmp_dir/custom-model.sh"
cat >"$custom_model" <<'CUSTOM'
#!/usr/bin/env bash
set -euo pipefail
input=$(cat)
grep -q $'^preedit\tnihao$' <<< "$input"
printf '%s\n' $'candidate\t你号'
CUSTOM
chmod +x "$custom_model"
custom_arg_model="$tmp_dir/custom-arg-model.sh"
cat >"$custom_arg_model" <<'CUSTOMARG'
#!/usr/bin/env bash
set -euo pipefail
input=$(cat)
grep -q $'^preedit\tnihao$' <<< "$input"
[[ "${TIPE_ARG_MODEL_MODE:-}" == "safe" ]]
[[ "${1:-}" == "--prefer" ]]
[[ "${2:-}" == "second" ]]
printf '%s\n' $'candidate\t你号'
CUSTOMARG
chmod +x "$custom_arg_model"
generated_model="$tmp_dir/generated-model.sh"
model_wrapper_dry_run=$("$ROOT/scripts/model-wrapper-new.sh" --path "$generated_model" --dry-run)
if [[ "$model_wrapper_dry_run" != *'Custom TiPE model wrapper template'* ||
    "$model_wrapper_dry_run" != *'supervision_mode="pass-through-only"'* ||
    "$model_wrapper_dry_run" != *'pass_through_only=1'* ||
    "$model_wrapper_dry_run" != *'candidate_metadata=()'* ||
    "$model_wrapper_dry_run" != *'input_state=""'* ||
	    "$model_wrapper_dry_run" != *'runtime_state=""'* ||
	    "$model_wrapper_dry_run" != *'continuous_mode=0'* ||
	    "$model_wrapper_dry_run" != *'input_mode="chinese"'* ||
	    "$model_wrapper_dry_run" != *'selected_candidate=""'* ||
	    "$model_wrapper_dry_run" != *'pending_segments=()'* ||
	    "$model_wrapper_dry_run" != *'visible_candidates=()'* ||
    "$model_wrapper_dry_run" != *'numbered_candidates=()'* ||
	    "$model_wrapper_dry_run" != *'event_counts=()'* ||
	    "$model_wrapper_dry_run" != *'preedit_leading_events=()'* ||
	    "$model_wrapper_dry_run" != *'preedit_leading_event_counts=()'* ||
	    "$model_wrapper_dry_run" != *'correction_event_counts=()'* ||
	    "$model_wrapper_dry_run" != *'edit_summary_current=""'* ||
	    "$model_wrapper_dry_run" != *'correction_patterns=()'* ||
	    "$model_wrapper_dry_run" != *'realtime_correction_decisions=()'* ||
	    "$model_wrapper_dry_run" != *'recent_history_records=0'* ||
	    "$model_wrapper_dry_run" != *'recent_history_preedits=()'* ||
	    "$model_wrapper_dry_run" != *'recent_history_preedit_selected_pairs=()'* ||
	    "$model_wrapper_dry_run" != *'recent_history_corrections=()'* ||
	    "$model_wrapper_dry_run" != *'compute_recent_history_summary'* ||
	    "$model_wrapper_dry_run" != *'compute_behavior_summary'* ||
	    "$model_wrapper_dry_run" != *'TIPE_WRAPPER_DEBUG_SUMMARY'* ||
	    "$model_wrapper_dry_run" != *'wrapper-debug\tpreedit-leading-context'* ||
	    "$model_wrapper_dry_run" != *'relative_to_end'* ||
	    "$model_wrapper_dry_run" != *'end-{position}'* ||
	    "$model_wrapper_dry_run" != *'emit_selected_candidate_preference'* ||
	    "$model_wrapper_dry_run" != *'emit_pending_segment_confirmation_chain'* ||
	    "$model_wrapper_dry_run" != *'pass-through-only, TiPE is showing read-only keyboard behavior context'* ||
	    -e "$generated_model" ]]; then
    echo "model wrapper generator dry-run should print template without writing" >&2
    exit 1
fi
"$ROOT/scripts/model-wrapper-new.sh" --path "$generated_model" >/dev/null
if [[ ! -x "$generated_model" ]]; then
    echo "model wrapper generator should create an executable wrapper" >&2
    exit 1
fi
if "$ROOT/scripts/model-wrapper-new.sh" --path "$generated_model" >/dev/null 2>&1; then
    echo "model wrapper generator should not overwrite without --force" >&2
    exit 1
fi
generated_model_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' | "$generated_model")
if [[ -n "$generated_model_output" ]]; then
    echo "generated custom model wrapper should not emit hardcoded candidate rows" >&2
    exit 1
fi
generated_model_behavior_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nevent_counts\tletter:1\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\ncorrection_event_counts\tletter:9\tbackspace:4\ncorrection\tihao\tnihao\t2\n' |
        "$generated_model"
)
if [[ -n "$generated_model_behavior_output" ]]; then
    echo "generated custom model wrapper should tolerate behavior-summary inputs without hardcoded rows" >&2
    exit 1
fi
generated_model_debug_output=$(
    printf 'protocol\t1\npreedit\tni\ncandidates\t你\nevents\tobserved:WindowSwitch\tcursor-move:Left\tletter:n\tletter:i\nevent_counts\tobserved:1\tcursor-move:1\tletter:2\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_debug_output" != *$'wrapper-debug\tpreedit-leading-context\tactive\t1\tevents\t2'* ||
    "$generated_model_debug_output" != *$'wrapper-debug\tpreedit-leading-event\tobserved:WindowSwitch'* ||
    "$generated_model_debug_output" != *$'wrapper-debug\tpreedit-leading-event\tcursor-move:Left'* ||
    "$generated_model_debug_output" != *$'wrapper-debug\tpreedit-leading-event-count\tobserved\t1'* ||
    "$generated_model_debug_output" != *$'wrapper-debug\tpreedit-leading-event-count\tcursor-move\t1'* ]]; then
    echo "generated custom model wrapper should expose key context before the first preedit letter" >&2
    exit 1
fi
generated_model_pass_through_debug_output=$(
    printf 'protocol\t1\npreedit\t\ncandidates\nevents\tspace:\tdelete:\tcursor-move:Down\tobserved:Tab\nevent_counts\tspace:1\tdelete:1\tcursor-move:1\tobserved:1\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_pass_through_debug_output" != *$'wrapper-debug\tpreedit-leading-context\tactive\t1\tevents\t4'* ||
    "$generated_model_pass_through_debug_output" != *$'wrapper-debug\tpreedit-leading-event\tspace:'* ||
    "$generated_model_pass_through_debug_output" != *$'wrapper-debug\tpreedit-leading-event\tobserved:Tab'* ||
    "$generated_model_pass_through_debug_output" != *$'wrapper-debug\tpreedit-leading-event-count\tspace\t1'* ||
    "$generated_model_pass_through_debug_output" != *$'wrapper-debug\tpreedit-leading-event-count\tobserved\t1'* ]]; then
    echo "generated custom model wrapper should expose pass-through key context when no preedit is active" >&2
    exit 1
fi
generated_model_generalized_debug=$(
    printf 'protocol\t1\npreedit\tong\ncandidates\t弄\t哦嗯个\ncorrection_events\tletter:o\tletter:n\tletter:g\ncorrection\tihao\tnihao\t2\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_generalized_debug" != *$'wrapper-debug\trealtime-correction\tapplied\tok\tmissing\tn\t0\t2\tnong'* ]]; then
    echo "generated custom model wrapper should expose generalized realtime correction decisions for custom models" >&2
    exit 1
fi
generated_model_transpose_debug=$(
    printf 'protocol\t1\npreedit\tjibengongnegn\ncandidates\t基本功呢功能\t基本功能\ncorrection\tgongnegn\tgongneng\t2\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_transpose_debug" != *$'wrapper-debug\tcorrection-pattern\ttranspose\tgn->ng\tend-0\t2'* ||
    "$generated_model_transpose_debug" != *$'wrapper-debug\trealtime-correction\tapplied\tok\ttranspose\tgn->ng\tend-0\t2\tjibengongneng'* ]]; then
    echo "generated custom model wrapper should expose adjacent-transposition correction decisions" >&2
    exit 1
fi
generated_wrapper_history_cache="$tmp_dir/generated-wrapper-history-cache"
mkdir -p "$generated_wrapper_history_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\told\tcandidates\t1\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\told'
    printf '%s\n' $'candidates\t旧'
    printf '%s\n' $'selected_candidate\t0\t旧'
    printf '%s\n' $'event_counts\tobserved:1'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t1\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tAlacritty\tpreedit\tnihao\tcandidates\t2\texpanded\t1\tterminal\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'application\tAlacritty'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcandidate-selected:你号'
    printf '%s\n' $'event_counts\tletter:1\tcursor-move:1'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你号'
    printf '%s\n' $'correction_event_counts\tletter:9\tbackspace:4'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t2\tcorrection_events\t13'
    printf '%s\n' $'segment_chain\tnihao\tni\t你\thao\tnihao\t你好'
    printf '%s\n' $'---\tunix_ms\t3\tprogram\tAlacritty\tpreedit\t\tcandidates\t0\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\t'
    printf '%s\n' $'candidates'
    printf '%s\n' $'event_counts\tspace:2\tdelete:1\tcursor-move:1'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你号'
    printf '%s\n' $'correction_event_counts\tletter:9\tbackspace:4\tcandidate-selected:1'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t4\tcorrection_events\t0'
} >"$generated_wrapper_history_cache/tipe/supervision-history.tsv"
generated_model_history_debug=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        XDG_CACHE_HOME="$generated_wrapper_history_cache" TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_history_debug" != *$'wrapper-debug\trecent-history\tsummary\tavailable\t1\trecords\t3\tactive\t2\tpass-through\t1\tsegment-chains\t1\tpending-segments\t0\tpath\t'"$generated_wrapper_history_cache/tipe/supervision-history.tsv"$'\n'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-preedit\tnihao\t1'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-preedit\told\t1'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-selected-candidate\t你号\t1'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-preedit-selected\tnihao\t你号\t1'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-event-count\tletter\t1'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-event-count\tspace\t2'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-active-event-count\tletter\t1'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-pass-through-event-count\tspace\t2'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-pass-through-event-count\tdelete\t1'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-correction-event-count\tbackspace\t4'* ||
    "$generated_model_history_debug" != *$'wrapper-debug\trecent-history-correction\tihao\tnihao\t1'* ]]; then
    echo "generated custom model wrapper should expose bounded supervision history summaries for custom models" >&2
    exit 1
fi
if [[ "$generated_model_history_debug" == *$'wrapper-debug\trecent-history-selected-candidate\t旧\t1'* ||
    "$generated_model_history_debug" == *$'wrapper-debug\trecent-history-preedit-selected\told\t旧\t1'* ]]; then
    echo "generated custom model wrapper should not learn passive default selected_candidate history rows" >&2
    exit 1
fi
generated_model_guarded_debug=$(
    printf 'protocol\t1\npreedit\thaodewokanyxiahaiyoumeiyu\ncandidates\t好的我看一下还有美誉\ncorrection\tihao\tnihao\t2\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_guarded_debug" != *$'wrapper-debug\trealtime-correction\tguarded\tlong-preedit\tmissing\tn\t0\t2'* ]]; then
    echo "generated custom model wrapper should expose long-preedit guards for custom models" >&2
    exit 1
fi
if "$ROOT/scripts/model-wrapper-check.sh" --command "TIPE_WRAPPER_DEBUG_SUMMARY=1 $generated_model" >/dev/null 2>&1; then
    echo "model wrapper checker should reject wrapper-debug rows as model output" >&2
    exit 1
fi
generated_model_no_newline_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号' | "$generated_model")
if [[ -n "$generated_model_no_newline_output" ]]; then
    echo "generated custom model wrapper should parse a final request row without a trailing newline" >&2
    exit 1
fi
generated_model_pass_through_output=$(
    printf 'protocol\t1\npreedit\t\ncandidates\nevents\tspace:\tdelete:\tcursor-move:Down\nevent_counts\tspace:1\tdelete:1\tcursor-move:1\n' |
        "$generated_model"
)
if [[ -n "$generated_model_pass_through_output" ]]; then
    echo "generated custom model wrapper should treat empty-preedit pass-through supervision as context only" >&2
    exit 1
fi
generated_model_pass_through_debug=$(
    printf 'protocol\t1\npreedit\t\ncandidates\nevents\tspace:\tdelete:\tcursor-move:Down\nevent_counts\tspace:1\tdelete:1\tcursor-move:1\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_pass_through_debug" != *$'wrapper-debug\tlearning-status\tprimary\tkeyboard-context-only\tnext-step\twait for an active preedit before emitting candidates or learning rows'* ]]; then
    echo "generated custom model wrapper should expose keyboard-context-only learning status" >&2
    exit 1
fi
generated_model_ui_state_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t2\tsource\tprefix\tscore\t999999\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nselected_candidate\t1\t你号\nvisible_candidates\t0:你好\t1:你号\nnumbered_candidates\t1:1:你号\n' |
        "$generated_model"
)
if [[ -n "$generated_model_ui_state_output" ]]; then
    echo "generated custom model wrapper should parse UI state rows without hardcoded rows" >&2
    exit 1
fi
generated_model_prefix_selected_debug=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t2\tsource\tprefix\tscore\t999999\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nselected_candidate\t1\t你号\nvisible_candidates\t0:你好\t1:你号\nnumbered_candidates\t1:1:你号\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_prefix_selected_debug" != *$'wrapper-debug\tlearning-status\tprimary\tawaiting-suffix-confirmation\tnext-step\twait until the prefix-only selection is followed by a confirmed suffix candidate'* ||
    "$generated_model_prefix_selected_debug" == *$'wrapper-debug\tlearning-status-suggested-protocol\tpreference\tnihao\t你号\t2'* ]]; then
    echo "generated custom model wrapper should expose prefix-only selected candidates as awaiting suffix, not preferences" >&2
    exit 1
fi
generated_model_selected_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nselected_candidate\t1\t你号\nvisible_candidates\t0:你好\t1:你号\nnumbered_candidates\t1:1:你号\n' |
        "$generated_model"
)
if [[ "$generated_model_selected_output" != $'candidate\t你号\npreference\tnihao\t你号\t2' ]]; then
    echo "generated custom model wrapper should demonstrate selected-candidate ranking and learning rows" >&2
    exit 1
fi
generated_model_selected_debug=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nselected_candidate\t1\t你号\nvisible_candidates\t0:你好\t1:你号\nnumbered_candidates\t1:1:你号\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_selected_debug" != *$'wrapper-debug\tlearning-status\tprimary\tready-to-learn\tnext-step\tprefer suggested protocol rows that match the current active preedit and visible UI state'* ||
    "$generated_model_selected_debug" != *$'wrapper-debug\tlearning-status-suggested-protocol\tpreference\tnihao\t你号\t2'* ||
    "$generated_model_selected_debug" != *$'wrapper-debug\tlearning-status-signal-count\tselected_candidate:1'* ]]; then
    echo "generated custom model wrapper should expose selected-candidate learning status" >&2
    exit 1
fi
generated_wrapper_history_preference_cache="$tmp_dir/generated-wrapper-history-preference-cache"
mkdir -p "$generated_wrapper_history_preference_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcandidate-selected:你号'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcandidate-selected:你号'
} >"$generated_wrapper_history_preference_cache/tipe/supervision-history.tsv"
generated_model_history_preference_debug=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        XDG_CACHE_HOME="$generated_wrapper_history_preference_cache" TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_history_preference_debug" != *$'wrapper-debug\tlearning-status-suggested-protocol\tpreference\tnihao\t你号\t2'* ||
    "$generated_model_history_preference_debug" != *$'wrapper-debug\tlearning-status-signal-count\thistory_preference:1'* ]]; then
    echo "generated custom model wrapper should turn repeated history pairs into learning status" >&2
    exit 1
fi
generated_wrapper_history_prefix_cache="$tmp_dir/generated-wrapper-history-prefix-cache"
mkdir -p "$generated_wrapper_history_prefix_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tjixuzuo\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tjixuzuo'
    printf '%s\n' $'candidates\t继续做\t继续'
    printf '%s\n' $'candidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000'
    printf '%s\n' $'candidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999'
    printf '%s\n' $'selected_candidate\t1\t继续'
    printf '%s\n' $'events\tcandidate-selected:继续'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tjixuzuo\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tjixuzuo'
    printf '%s\n' $'candidates\t继续做\t继续'
    printf '%s\n' $'candidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000'
    printf '%s\n' $'candidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999'
    printf '%s\n' $'selected_candidate\t1\t继续'
    printf '%s\n' $'events\tcandidate-selected:继续'
} >"$generated_wrapper_history_prefix_cache/tipe/supervision-history.tsv"
generated_model_history_prefix_debug=$(
    printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\n' |
        XDG_CACHE_HOME="$generated_wrapper_history_prefix_cache" TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_history_prefix_debug" == *$'wrapper-debug\tlearning-status-suggested-protocol\tpreference\tjixuzuo\t继续\t2'* ||
    "$generated_model_history_prefix_debug" == *$'wrapper-debug\tlearning-status-signal-count\thistory_preference:1'* ]]; then
    echo "generated custom model wrapper should not turn repeated prefix-only history pairs into preferences" >&2
    exit 1
fi
generated_wrapper_history_correction_cache="$tmp_dir/generated-wrapper-history-correction-cache"
mkdir -p "$generated_wrapper_history_correction_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0\tterminal\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你好'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0\tterminal\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你好'
} >"$generated_wrapper_history_correction_cache/tipe/supervision-history.tsv"
generated_model_history_correction_debug=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        XDG_CACHE_HOME="$generated_wrapper_history_correction_cache" TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_history_correction_debug" != *$'wrapper-debug\tlearning-status-suggested-protocol\tcorrection\tihao\tnihao'* ||
    "$generated_model_history_correction_debug" != *$'wrapper-debug\tlearning-status-signal-count\thistory_correction:1'* ]]; then
    echo "generated custom model wrapper should turn repeated history corrections into learning status" >&2
    exit 1
fi
generated_model_known_selected_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nselected_candidate\t1\t你号\npreference\tnihao\t你号\t4\n' |
        "$generated_model"
)
if [[ "$generated_model_known_selected_output" != $'candidate\t你号' ]]; then
    echo "generated custom model wrapper should not re-emit known selected-candidate preference rows" >&2
    exit 1
fi
generated_model_known_selected_debug=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nselected_candidate\t1\t你号\npreference\tnihao\t你号\t4\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_known_selected_debug" == *$'wrapper-debug\tlearning-status-suggested-protocol\tpreference\tnihao\t你号\t2'* ]]; then
    echo "generated custom model wrapper debug summary should not suggest already-known selected-candidate preferences" >&2
    exit 1
fi
generated_model_known_correction_debug=$(
    printf 'protocol\t1\npreedit\tihao\ncandidates\t你好\t你号\ncorrection\tihao\tnihao\t3\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_known_correction_debug" == *$'wrapper-debug\tlearning-status-suggested-protocol\tcorrection\tihao\tnihao'* ||
    "$generated_model_known_correction_debug" != *$'wrapper-debug\tlearning-status-evidence-protocol\tcorrection\tihao\tnihao'* ]]; then
    echo "generated custom model wrapper debug summary should expose known corrections as evidence, not suggested learning" >&2
    exit 1
fi
generated_model_segment_output=$(
    printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\nsegment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做\n' |
        "$generated_model"
)
if [[ "$generated_model_segment_output" != $'candidate\t继续做' ]]; then
    echo "generated custom model wrapper should demonstrate segment-chain candidate rows" >&2
    exit 1
fi
generated_model_segment_suffix_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t从\t操\t曹\ncontext\t我\nsegment_chain\twoc\two\t我\tc\twocao\t我操\nsegment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做\n' |
        "$generated_model"
)
if [[ "$generated_model_segment_suffix_output" != $'candidate\t操' ]]; then
    echo "generated custom model wrapper should demonstrate segment-chain suffix candidate rows" >&2
    exit 1
fi
generated_model_pending_segment_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t0\t操\n' |
        "$generated_model"
)
if [[ -n "$generated_model_pending_segment_output" ]]; then
    echo "generated custom model wrapper should not confirm pending segments from a passive top candidate" >&2
    exit 1
fi
generated_model_pending_segment_debug=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t0\t操\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_pending_segment_debug" != *$'wrapper-debug\tlearning-status\tprimary\tawaiting-suffix-confirmation'* ||
    "$generated_model_pending_segment_debug" != *$'wrapper-debug\tlearning-status-awaiting-suffix\twoc\two\t我\tc'* ]]; then
    echo "generated custom model wrapper should keep passive top suffix pending" >&2
    exit 1
fi
generated_model_explicit_pending_segment_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t1\t从\n' |
        "$generated_model"
)
if [[ "$generated_model_explicit_pending_segment_output" != $'candidate\t从\npreference\tc\t从\t2\nsegment_chain\twoc\two\t我\tc\twoc\t我从\t1' ]]; then
    echo "generated custom model wrapper should demonstrate explicitly selected pending-segment learning rows" >&2
    exit 1
fi
generated_model_explicit_pending_segment_debug=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t1\t从\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_explicit_pending_segment_debug" != *$'wrapper-debug\tlearning-status\tprimary\tready-to-learn'* ||
    "$generated_model_explicit_pending_segment_debug" != *$'wrapper-debug\tlearning-status-suggested-protocol\tsegment_chain\twoc\two\t我\tc\twoc\t我从\t1'* ]]; then
    echo "generated custom model wrapper should expose explicit pending-segment confirmation learning status" >&2
    exit 1
fi
generated_model_known_pending_segment_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t0\t操\nsegment_chain\twoc\two\t我\tc\twoc\t我操\n' |
        "$generated_model"
)
if [[ "$generated_model_known_pending_segment_output" != $'candidate\t操' ]]; then
    echo "generated custom model wrapper should rerank from known segment chains without re-emitting them as learning rows" >&2
    exit 1
fi
generated_model_known_pending_segment_debug=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t0\t操\nsegment_chain\twoc\two\t我\tc\twoc\t我操\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_known_pending_segment_debug" == *$'wrapper-debug\tlearning-status-suggested-protocol\tsegment_chain\twoc\two\t我\tc\twoc\t我操\t1'* ]]; then
    echo "generated custom model wrapper debug summary should not suggest already-known segment chains" >&2
    exit 1
fi
generated_model_awaiting_segment_debug=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\n' |
        TIPE_WRAPPER_DEBUG_SUMMARY=1 "$generated_model"
)
if [[ "$generated_model_awaiting_segment_debug" != *$'wrapper-debug\tlearning-status\tprimary\tawaiting-suffix-confirmation'* ||
    "$generated_model_awaiting_segment_debug" != *$'wrapper-debug\tlearning-status-awaiting-suffix\twoc\two\t我\tc'* ]]; then
    echo "generated custom model wrapper should expose awaiting-suffix learning status" >&2
    exit 1
fi
generated_model_pending_segment_check=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t1\t从\n' |
        "$ROOT/scripts/model-wrapper-check.sh" --command "$generated_model" --request -
)
if [[ "$generated_model_pending_segment_check" != wrapper-ok$'\t'"$generated_model"$'\t'rows$'\t3' ]]; then
    echo "model wrapper checker should accept generated pending-segment learning rows" >&2
    exit 1
fi
model_wrapper_check_output=$("$ROOT/scripts/model-wrapper-check.sh" --command "$generated_model")
if [[ "$model_wrapper_check_output" != wrapper-ok$'\t'"$generated_model"$'\t'rows$'\t1' ]]; then
    echo "model wrapper checker should accept generated wrapper output" >&2
    exit 1
fi
model_wrapper_check_stdin_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        "$ROOT/scripts/model-wrapper-check.sh" --command "$generated_model" --request -
)
if [[ "$model_wrapper_check_stdin_output" != wrapper-ok$'\t'"$generated_model"$'\t'rows$'\t0' ]]; then
    echo "model wrapper checker should accept --request - as stdin" >&2
    exit 1
fi
custom_arg_command="TIPE_ARG_MODEL_MODE=safe $custom_arg_model --prefer second"
model_wrapper_arg_check_output=$("$ROOT/scripts/model-wrapper-check.sh" --command "$custom_arg_command")
if [[ "$model_wrapper_arg_check_output" != wrapper-ok$'\t'"$custom_arg_command"$'\t'rows$'\t1' ]]; then
    echo "model wrapper checker should run safe custom commands with env assignments and arguments" >&2
    exit 1
fi
learning_action_model="$tmp_dir/learning-action-model.sh"
cat >"$learning_action_model" <<'LEARNMODEL'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'preference\tnihao\t你号\t4' $'segment_chain\tnihao\tni\t你\thao\tnihao\t你好\t5'
LEARNMODEL
chmod +x "$learning_action_model"
learning_action_check_output=$("$ROOT/scripts/model-wrapper-check.sh" --command "$learning_action_model")
if [[ "$learning_action_check_output" != wrapper-ok$'\t'"$learning_action_model"$'\t'rows$'\t2' ]]; then
    echo "model wrapper checker should accept explicit preference and segment-chain learning rows" >&2
    exit 1
fi
bad_candidate_model="$tmp_dir/bad-candidate-model.sh"
cat >"$bad_candidate_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'candidate\t不存在'
BAD
chmod +x "$bad_candidate_model"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$bad_candidate_model" >/dev/null 2>&1; then
    echo "model wrapper checker should reject candidates not present in the request" >&2
    exit 1
fi
raw_english_model="$tmp_dir/raw-english-model.sh"
cat >"$raw_english_model" <<'RAWEN'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'candidate\tvite'
RAWEN
chmod +x "$raw_english_model"
raw_english_request="$tmp_dir/raw-english-request.tsv"
printf 'protocol\t1\npreedit\tvite\ncandidates\t尴尬\n' >"$raw_english_request"
if ! "$ROOT/scripts/model-wrapper-check.sh" --command "$raw_english_model" --request "$raw_english_request" >/dev/null; then
    echo "model wrapper checker should allow a non-pinyin raw-English current preedit candidate" >&2
    exit 1
fi
raw_pinyin_model="$tmp_dir/raw-pinyin-model.sh"
cat >"$raw_pinyin_model" <<'RAWPY'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'candidate\tnihao'
RAWPY
chmod +x "$raw_pinyin_model"
raw_pinyin_request="$tmp_dir/raw-pinyin-request.tsv"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' >"$raw_pinyin_request"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$raw_pinyin_model" --request "$raw_pinyin_request" >/dev/null 2>&1; then
    echo "model wrapper checker should reject normal pinyin raw preedit candidates" >&2
    exit 1
fi
bad_correction_model="$tmp_dir/bad-correction-model.sh"
cat >"$bad_correction_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'correction\tihao\tzzzz'
BAD
chmod +x "$bad_correction_model"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$bad_correction_model" >/dev/null 2>&1; then
    echo "model wrapper checker should reject implausible corrections" >&2
    exit 1
fi
bad_unrelated_correction_request="$tmp_dir/bad-unrelated-correction-request.tsv"
bad_unrelated_correction_model="$tmp_dir/bad-unrelated-correction-model.sh"
printf 'protocol\t1\npreedit\tihao\ncandidates\t以后\t一号\n' >"$bad_unrelated_correction_request"
cat >"$bad_unrelated_correction_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'correction\tnhao\tnihao'
BAD
chmod +x "$bad_unrelated_correction_model"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$bad_unrelated_correction_model" \
    --request "$bad_unrelated_correction_request" >/dev/null 2>&1; then
    echo "model wrapper checker should reject corrections unrelated to the current preedit" >&2
    exit 1
fi
bad_preference_model="$tmp_dir/bad-preference-model.sh"
cat >"$bad_preference_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'preference\tbie\t你号\t4'
BAD
chmod +x "$bad_preference_model"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$bad_preference_model" >/dev/null 2>&1; then
    echo "model wrapper checker should reject preference rows for a different preedit" >&2
    exit 1
fi
bad_prefix_preference_request="$tmp_dir/bad-prefix-preference-request.tsv"
bad_prefix_preference_model="$tmp_dir/bad-prefix-preference-model.sh"
printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\n' >"$bad_prefix_preference_request"
cat >"$bad_prefix_preference_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'preference\tjixuzuo\t继续\t4'
BAD
chmod +x "$bad_prefix_preference_model"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$bad_prefix_preference_model" \
    --request "$bad_prefix_preference_request" >/dev/null 2>&1; then
    echo "model wrapper checker should reject prefix-only candidate preferences" >&2
    exit 1
fi
good_full_consumed_preference_request="$tmp_dir/good-full-consumed-preference-request.tsv"
good_full_consumed_preference_model="$tmp_dir/good-full-consumed-preference-model.sh"
printf 'protocol\t1\npreedit\tpinyinjiu\ncandidates\t品饮酒\t拼音就\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t9\tsource\tprefix-continuation\tscore\t999999\n' >"$good_full_consumed_preference_request"
cat >"$good_full_consumed_preference_model" <<'GOOD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'preference\tpinyinjiu\t拼音就\t4'
GOOD
chmod +x "$good_full_consumed_preference_model"
if ! "$ROOT/scripts/model-wrapper-check.sh" --command "$good_full_consumed_preference_model" \
    --request "$good_full_consumed_preference_request" >/dev/null 2>&1; then
    echo "model wrapper checker should accept full-consuming consumed-prefix preferences" >&2
    exit 1
fi
passive_pending_segment_chain_request="$tmp_dir/passive-pending-segment-chain-request.tsv"
passive_pending_segment_chain_model="$tmp_dir/passive-pending-segment-chain-model.sh"
printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t0\t操\n' >"$passive_pending_segment_chain_request"
cat >"$passive_pending_segment_chain_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'segment_chain\twoc\two\t我\tc\twoc\t我操\t5'
BAD
chmod +x "$passive_pending_segment_chain_model"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$passive_pending_segment_chain_model" \
    --request "$passive_pending_segment_chain_request" >/dev/null 2>&1; then
    echo "model wrapper checker should reject pending segment-chain rows from passive top suffix highlights" >&2
    exit 1
fi
pending_segment_chain_request="$tmp_dir/pending-segment-chain-request.tsv"
pending_segment_chain_model="$tmp_dir/pending-segment-chain-model.sh"
printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t1\t从\n' >"$pending_segment_chain_request"
cat >"$pending_segment_chain_model" <<'GOOD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'segment_chain\twoc\two\t我\tc\twoc\t我从\t5'
GOOD
chmod +x "$pending_segment_chain_model"
if ! "$ROOT/scripts/model-wrapper-check.sh" --command "$pending_segment_chain_model" \
    --request "$pending_segment_chain_request" >/dev/null 2>&1; then
    echo "model wrapper checker should accept segment-chain rows confirmed from pending prefix selections" >&2
    exit 1
fi
bad_segment_chain_model="$tmp_dir/bad-segment-chain-model.sh"
cat >"$bad_segment_chain_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'segment_chain\tnihao\tni\t你\thao\tnihao\t不存在\t5'
BAD
chmod +x "$bad_segment_chain_model"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$bad_segment_chain_model" >/dev/null 2>&1; then
    echo "model wrapper checker should reject segment-chain rows whose combined candidate is absent" >&2
    exit 1
fi
bad_segment_chain_shape_model="$tmp_dir/bad-segment-chain-shape-model.sh"
cat >"$bad_segment_chain_shape_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'segment_chain\tnihao\tn\t你\thao\tnihao\t你好\t5'
BAD
chmod +x "$bad_segment_chain_shape_model"
if "$ROOT/scripts/model-wrapper-check.sh" --command "$bad_segment_chain_shape_model" >/dev/null 2>&1; then
    echo "model wrapper checker should reject segment-chain rows with impossible consumed/remaining shape" >&2
    exit 1
fi
debug_learn_request="$tmp_dir/debug-learn-request.tsv"
debug_learn_preferences="$tmp_dir/debug-learn-preferences.tsv"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' >"$debug_learn_request"
if "$ROOT/scripts/model-replay.sh" --request "$debug_learn_request" --command "TIPE_WRAPPER_DEBUG_SUMMARY=1 $generated_model" \
    --learn-output --preferences "$debug_learn_preferences" >/dev/null 2>&1; then
    echo "model replay --learn-output should reject debug rows before learning" >&2
    exit 1
fi
if [[ -e "$debug_learn_preferences" && -s "$debug_learn_preferences" ]]; then
    echo "model replay should not persist preferences from debug wrapper output" >&2
    exit 1
fi
unrelated_correction_learn_request="$tmp_dir/unrelated-correction-learn-request.tsv"
unrelated_correction_learn_preferences="$tmp_dir/unrelated-correction-learn-preferences.tsv"
unrelated_correction_learn_model="$tmp_dir/unrelated-correction-learn-model.sh"
printf 'protocol\t1\npreedit\tihao\ncandidates\t以后\t一号\n' >"$unrelated_correction_learn_request"
cat >"$unrelated_correction_learn_model" <<'BAD'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'correction\tnhao\tnihao'
BAD
chmod +x "$unrelated_correction_learn_model"
if "$ROOT/scripts/model-replay.sh" --request "$unrelated_correction_learn_request" \
    --command "$unrelated_correction_learn_model" --learn-output \
    --preferences "$unrelated_correction_learn_preferences" >/dev/null 2>&1; then
    echo "model replay should reject corrections unrelated to the current preedit before learning" >&2
    exit 1
fi
if [[ -e "$unrelated_correction_learn_preferences" ]] &&
    grep -qF $'__correction__\tnhao\tnihao' "$unrelated_correction_learn_preferences"; then
    echo "model replay should not persist corrections unrelated to the current preedit" >&2
    exit 1
fi
configured_generated_model="$tmp_dir/configured-generated-model.sh"
configured_generated_config="$tmp_dir/configured-generated-model-env"
"$ROOT/scripts/model-wrapper-new.sh" --path "$configured_generated_model" --configure \
    --config "$configured_generated_config" >/dev/null
if [[ ! -x "$configured_generated_model" ]] ||
    ! grep -qF 'export TIPE_MODEL_MODE=custom' "$configured_generated_config" ||
    ! grep -qF "TIPE_MODEL_CUSTOM_COMMAND=" "$configured_generated_config"; then
    echo "model wrapper generator should create a wrapper and custom model config when requested" >&2
    exit 1
fi
configured_generated_output=$(printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\nsegment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做\n' |
    TIPE_MODEL_CONFIG="$configured_generated_config" "$ROOT/scripts/model-current.sh")
if [[ "$configured_generated_output" != $'candidate\t继续做' ]]; then
    echo "configured generated custom model wrapper should work through model-current" >&2
    exit 1
fi
model_config_custom_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write custom \
    --command "$custom_model" --dry-run)
if [[ "$model_config_custom_dry_run" != *'export TIPE_MODEL_MODE=custom'* ||
    "$model_config_custom_dry_run" != *"export TIPE_MODEL_CUSTOM_COMMAND="*"$custom_model"* ||
    -e "$model_config_path" ]]; then
    echo "model config helper dry-run should print custom wrapper config without writing" >&2
    exit 1
fi
model_config_custom_arg_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write custom \
    --command "$custom_arg_command" --dry-run)
if [[ "$model_config_custom_arg_dry_run" != *'export TIPE_MODEL_MODE=custom'* ||
    "$model_config_custom_arg_dry_run" != *"export TIPE_MODEL_CUSTOM_COMMAND="*"custom-arg-model.sh"* ||
    -e "$model_config_path" ]]; then
    echo "model config helper should allow safe custom wrapper arguments without writing during dry-run" >&2
    exit 1
fi
model_config_continuous_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write heuristic \
    --continuous on --dry-run)
if [[ "$model_config_continuous_dry_run" != *'export TIPE_CONTINUOUS_MODE=1'* ||
    "$model_config_continuous_dry_run" != *'export TIPE_PERSONAL_TRAIN_CONTEXT=0'* ||
    "$model_config_continuous_dry_run" != *'export TIPE_PERSONAL_TRAIN_SURROUNDING=0'* ||
    -e "$model_config_path" ]]; then
    echo "model config helper dry-run should print continuous-mode defaults without writing" >&2
    exit 1
fi
model_config_context_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write heuristic \
    --training-context on --training-surrounding on --dry-run)
if [[ "$model_config_context_dry_run" != *'export TIPE_PERSONAL_TRAIN_CONTEXT=1'* ||
    "$model_config_context_dry_run" != *'export TIPE_PERSONAL_TRAIN_SURROUNDING=1'* ||
    -e "$model_config_path" ]]; then
    echo "model config helper should persist explicitly enabled personal-training context fingerprints" >&2
    exit 1
fi
model_config_context_path="$tmp_dir/context-model-env"
TIPE_MODEL_CONFIG="$model_config_context_path" "$ROOT/scripts/model-config.sh" --write personal \
    --training-context on --training-surrounding on >/dev/null
model_config_context_show=$(TIPE_MODEL_CONFIG="$model_config_context_path" "$ROOT/scripts/model-config.sh" --show)
model_current_context_show=$(TIPE_MODEL_CONFIG="$model_config_context_path" "$ROOT/scripts/model-current.sh" --show)
model_current_context_env=$(TIPE_MODEL_CONFIG="$model_config_context_path" "$ROOT/scripts/model-current.sh" --print-env)
doctor_context_show=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$model_config_context_path" "$ROOT/scripts/doctor.sh" --no-runtime)
learning_panel_context_show=$(XDG_DATA_HOME="$learning_panel_data_home" \
    TIPE_MODEL_CONFIG="$model_config_context_path" "$ROOT/scripts/learning-panel.sh" --raw-panel \
    "$model_explain_sample")
if [[ "$model_config_context_show" != *$'model-status\ttraining-context\t1'* ||
    "$model_config_context_show" != *$'model-status\ttraining-surrounding\t1'* ||
    "$model_current_context_show" != *$'training-context\t1'* ||
    "$model_current_context_show" != *$'training-surrounding\t1'* ||
    "$model_current_context_env" != *'export TIPE_PERSONAL_TRAIN_CONTEXT=1'* ||
    "$model_current_context_env" != *'export TIPE_PERSONAL_TRAIN_SURROUNDING=1'* ||
    "$doctor_context_show" != *$'model\ttraining-context\t1'* ||
    "$doctor_context_show" != *$'model\ttraining-surrounding\t1'* ||
    "$learning_panel_context_show" != *$'panel\tmodel-config\ttraining-context\t1'* ||
    "$learning_panel_context_show" != *$'panel\tmodel-config\ttraining-surrounding\t1'* ]]; then
    echo "model config helper should report personal-training privacy choices independently of the backend" >&2
    exit 1
fi
model_config_before_failed_test=$(sha256sum "$model_config_context_path")
if TIPE_MODEL_CONFIG="$model_config_context_path" "$ROOT/scripts/model-config.sh" --write personal \
    --personal-model "$tmp_dir/missing-personal-model.json" --test >/dev/null 2>&1; then
    echo "model config helper should reject a personal backend whose model self-test fails" >&2
    exit 1
fi
if [[ "$(sha256sum "$model_config_context_path")" != "$model_config_before_failed_test" ]]; then
    echo "a failed model config self-test should preserve the previously working config atomically" >&2
    exit 1
fi
model_config_test_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write heuristic \
    --dry-run --test)
if [[ "$model_config_test_dry_run" != *'export TIPE_MODEL_MODE=heuristic'* ||
    "$model_config_test_dry_run" != *$'self-test-command\t'*"model-current.sh"* ||
    "$model_config_test_dry_run" != *$'wrapper-ok\t'*"model-current.sh"* ||
    -e "$model_config_path" ]]; then
    echo "model config helper should test dry-run configs without writing" >&2
    exit 1
fi
model_config_openai_test_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" \
    --write openai-compatible --base-url https://api.example.test/v1 --model cloud-test --dry-run --test-dry-run)
if [[ "$model_config_openai_test_dry_run" != *'export TIPE_MODEL_MODE=openai-compatible'* ||
    "$model_config_openai_test_dry_run" != *'export TIPE_MODEL_SEND_RECENT_INPUT=0'* ||
    "$model_config_openai_test_dry_run" != *'export TIPE_MODEL_SEND_SURROUNDING=0'* ||
    "$model_config_openai_test_dry_run" != *$'model-dry-run-request\thttps://api.example.test/v1/chat/completions'* ||
    "$model_config_openai_test_dry_run" != *$'model-dry-run-ok\trequest-json'* ||
    -e "$model_config_path" ]]; then
    echo "model config helper should dry-run-test OpenAI-compatible configs without network or writes" >&2
    exit 1
fi
model_config_official_openai_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" \
    --write openai --model tipe-test-model --dry-run --test-dry-run)
if [[ "$model_config_official_openai_dry_run" != *'export TIPE_MODEL_MODE=openai'* ||
    "$model_config_official_openai_dry_run" != *'export TIPE_MODEL_BACKEND=openai-compatible'* ||
    "$model_config_official_openai_dry_run" != *'export TIPE_MODEL_BASE_URL=https://api.openai.com/v1'* ||
    "$model_config_official_openai_dry_run" != *'export TIPE_MODEL_NAME=tipe-test-model'* ||
    "$model_config_official_openai_dry_run" != *'export TIPE_MODEL_API_KEY="${OPENAI_API_KEY:-}"'* ||
    "$model_config_official_openai_dry_run" != *'export TIPE_MODEL_SEND_RECENT_INPUT=0'* ||
    "$model_config_official_openai_dry_run" != *'export TIPE_MODEL_SEND_SURROUNDING=0'* ||
    "$model_config_official_openai_dry_run" != *$'model-dry-run-request\thttps://api.openai.com/v1/chat/completions'* ||
    "$model_config_official_openai_dry_run" != *$'model-dry-run-ok\trequest-json'* ||
    -e "$model_config_path" ]]; then
    echo "model config helper should make official OpenAI mode a real shortcut without network or writes" >&2
    exit 1
fi
model_config_official_openai_key_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" \
    --write openai --model tipe-test-model --api-key-env TIPE_TEST_OPENAI_KEY --dry-run)
if [[ "$model_config_official_openai_key_dry_run" != *'export TIPE_MODEL_API_KEY="${TIPE_TEST_OPENAI_KEY:-}"'* ||
    -e "$model_config_path" ]]; then
    echo "model config helper should allow overriding official OpenAI API key env" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai \
    --model tipe-test-model >/dev/null
model_config_official_openai_show=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --show)
doctor_official_openai_show=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/doctor.sh" --no-runtime)
model_current_official_openai_show=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-current.sh" --show)
if [[ "$model_config_official_openai_show" != *$'model-status\tconfigured-mode\topenai'* ||
    "$model_config_official_openai_show" != *$'model-status\tkind\tofficial-openai:openai-compatible'* ||
    "$model_config_official_openai_show" != *$'model-status\tchat-path\t/chat/completions'* ||
    "$model_config_official_openai_show" != *$'model-status\tapi-key-env\tOPENAI_API_KEY'* ||
    "$model_config_official_openai_show" != *$'model-status\tapi-key-runtime\tunset'* ||
    "$model_config_official_openai_show" != *$'model-status\tsend-recent-input\t0'* ||
    "$model_config_official_openai_show" != *$'model-status\tsend-surrounding\t0'* ||
    "$model_config_official_openai_show" != *$'model-status\tdry-run-test-supported\t1'* ||
    "$model_current_official_openai_show" != *$'mode\topenai'* ||
    "$model_current_official_openai_show" != *$'kind\tofficial-openai:openai-compatible'* ||
    "$model_current_official_openai_show" != *$'chat-path\t/chat/completions'* ||
    "$model_current_official_openai_show" != *$'api-key-env\tOPENAI_API_KEY'* ||
    "$model_current_official_openai_show" != *$'api-key-runtime\tunset'* ||
    "$model_current_official_openai_show" != *$'send-recent-input\t0'* ||
    "$model_current_official_openai_show" != *$'send-surrounding\t0'* ||
    "$model_current_official_openai_show" != *$'dry-run-test-supported\t1'* ||
    "$model_current_official_openai_show" != *$'configured-command-valid\t1'* ||
    "$doctor_official_openai_show" != *$'model\tconfigured-mode\topenai'* ||
    "$doctor_official_openai_show" != *$'model\tkind\tofficial-openai:openai-compatible'* ||
    "$doctor_official_openai_show" != *$'model\tdry-run-test-supported\t1'* ||
    "$doctor_official_openai_show" != *$'model\tmodel\ttipe-test-model'* ||
    "$doctor_official_openai_show" != *$'model\tchat-path\t/chat/completions'* ||
    "$doctor_official_openai_show" != *$'model\tapi-key-env\tOPENAI_API_KEY'* ||
    "$doctor_official_openai_show" != *$'model\tapi-key-runtime\tunset'* ||
    "$doctor_official_openai_show" != *$'model\tsend-recent-input\t0'* ||
    "$doctor_official_openai_show" != *$'model\tsend-surrounding\t0'* ]]; then
    echo "model config and doctor helpers should report official OpenAI distinctly from compatible providers" >&2
    exit 1
fi
rm -f "$model_config_path"
model_config_ollama_test_dry_run=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" \
    --write ollama --base-url http://127.0.0.1:11434/v1 --model qwen-test --dry-run --test-dry-run)
if [[ "$model_config_ollama_test_dry_run" != *'export TIPE_MODEL_MODE=ollama'* ||
    "$model_config_ollama_test_dry_run" != *$'model-dry-run-request\thttp://127.0.0.1:11434/v1/chat/completions'* ||
    "$model_config_ollama_test_dry_run" != *$'model-dry-run-ok\trequest-json'* ||
    -e "$model_config_path" ]]; then
    echo "model config helper should dry-run-test Ollama configs without network or writes" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write heuristic \
    --continuous maybe >/dev/null 2>&1; then
    echo "model config helper should reject invalid continuous-mode defaults" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write heuristic \
    --training-context maybe >/dev/null 2>&1; then
    echo "model config helper should reject invalid personal-training context choices" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write heuristic \
    --training-surrounding maybe >/dev/null 2>&1; then
    echo "model config helper should reject invalid personal-training surrounding choices" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai \
    --send-recent-input maybe >/dev/null 2>&1; then
    echo "model config helper should reject invalid cloud recent-input choices" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai \
    --send-surrounding maybe >/dev/null 2>&1; then
    echo "model config helper should reject invalid cloud surrounding choices" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write custom \
    --command "$custom_model;rm" >/dev/null 2>&1; then
    echo "model config helper should reject shell metacharacters in custom commands" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write custom >/dev/null 2>&1; then
    echo "model config helper should require --command for custom mode" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai-compatible \
    --base-url https://api.example.test/v1 --model cloud-test --api-key-env TIPE_TEST_API_KEY \
    --temperature 0 --max-tokens 64 --send-recent-input on --send-surrounding on >/dev/null
if [[ ! -f "$model_config_path" ]] ||
    [[ "$(stat -c '%a' "$model_config_path")" != "600" ]] ||
    ! grep -qF 'export TIPE_MODEL_BACKEND=openai-compatible' "$model_config_path" ||
    ! grep -qF 'export TIPE_MODEL_API_KEY="${TIPE_TEST_API_KEY:-}"' "$model_config_path" ||
    ! grep -qF 'export TIPE_MODEL_SEND_RECENT_INPUT=1' "$model_config_path" ||
    ! grep -qF 'export TIPE_MODEL_SEND_SURROUNDING=1' "$model_config_path" ||
    ! grep -qF 'export TIPE_MODEL_TEMPERATURE=0' "$model_config_path" ||
    grep -qF 'actual-secret' "$model_config_path"; then
    echo "model config helper should write sourceable OpenAI-compatible config without embedding secrets" >&2
    exit 1
fi
model_config_openai_compatible_show=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --show)
doctor_openai_compatible_show=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/doctor.sh" --no-runtime)
model_current_openai_compatible_show=$(TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-current.sh" --show)
model_current_openai_compatible_env=$(
    TIPE_TEST_API_KEY=actual-secret TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-current.sh" --print-env
)
if [[ "$model_config_openai_compatible_show" != *$'model-status\tconfigured-mode\topenai-compatible'* ||
    "$model_config_openai_compatible_show" != *$'model-status\tkind\topenai-compatible:openai-compatible'* ||
    "$model_config_openai_compatible_show" != *$'model-status\tchat-path\t/chat/completions'* ||
    "$model_config_openai_compatible_show" != *$'model-status\tapi-key-env\tTIPE_TEST_API_KEY'* ||
    "$model_config_openai_compatible_show" != *$'model-status\tapi-key-runtime\tunset'* ||
    "$model_config_openai_compatible_show" != *$'model-status\tsend-recent-input\t1'* ||
    "$model_config_openai_compatible_show" != *$'model-status\tsend-surrounding\t1'* ||
    "$model_config_openai_compatible_show" != *$'model-status\tdry-run-test-supported\t1'* ||
    "$model_current_openai_compatible_show" != *$'mode\topenai-compatible'* ||
    "$model_current_openai_compatible_show" != *$'kind\topenai-compatible:openai-compatible'* ||
    "$model_current_openai_compatible_show" != *$'chat-path\t/chat/completions'* ||
    "$model_current_openai_compatible_show" != *$'api-key-env\tTIPE_TEST_API_KEY'* ||
    "$model_current_openai_compatible_show" != *$'api-key-runtime\tunset'* ||
    "$model_current_openai_compatible_show" != *$'send-recent-input\t1'* ||
    "$model_current_openai_compatible_show" != *$'send-surrounding\t1'* ||
    "$model_current_openai_compatible_show" != *$'dry-run-test-supported\t1'* ||
    "$model_current_openai_compatible_show" != *$'configured-command-valid\t1'* ||
    "$doctor_openai_compatible_show" != *$'model\tconfigured-mode\topenai-compatible'* ||
    "$doctor_openai_compatible_show" != *$'model\tkind\topenai-compatible:openai-compatible'* ||
    "$doctor_openai_compatible_show" != *$'model\tdry-run-test-supported\t1'* ||
    "$doctor_openai_compatible_show" != *$'model\tmodel\tcloud-test'* ||
    "$doctor_openai_compatible_show" != *$'model\tchat-path\t/chat/completions'* ||
    "$doctor_openai_compatible_show" != *$'model\tapi-key-env\tTIPE_TEST_API_KEY'* ||
    "$doctor_openai_compatible_show" != *$'model\tapi-key-runtime\tunset'* ||
    "$doctor_openai_compatible_show" != *$'model\tsend-recent-input\t1'* ||
    "$doctor_openai_compatible_show" != *$'model\tsend-surrounding\t1'* ]]; then
    echo "model config and doctor helpers should keep OpenAI-compatible provider status distinct" >&2
    exit 1
fi
if [[ "$model_current_openai_compatible_env" != *'export TIPE_MODEL_BASE_URL=https://api.example.test/v1'* ||
    "$model_current_openai_compatible_env" != *'export TIPE_MODEL_NAME=cloud-test'* ||
    "$model_current_openai_compatible_env" != *'export TIPE_MODEL_TEMPERATURE=0'* ||
    "$model_current_openai_compatible_env" != *'export TIPE_MODEL_MAX_TOKENS=64'* ||
    "$model_current_openai_compatible_env" != *'export TIPE_MODEL_SEND_RECENT_INPUT=1'* ||
    "$model_current_openai_compatible_env" != *'export TIPE_MODEL_SEND_SURROUNDING=1'* ||
    "$model_current_openai_compatible_env" != *'export TIPE_MODEL_API_KEY="${TIPE_TEST_API_KEY:-}"'* ||
    "$model_current_openai_compatible_env" == *'actual-secret'* ]]; then
    echo "model current helper should print a complete reusable model env without leaking API keys" >&2
    exit 1
fi
stored_model_config="$tmp_dir/stored-model/model-env"
stored_model_key="$tmp_dir/stored-model/model-api-key"
stored_key_value='tipe-test-secret-value'
stored_key_test_dry_run=$(printf '%s\n' "$stored_key_value" | TIPE_MODEL_CONFIG="$stored_model_config" \
    "$ROOT/scripts/model-config.sh" --write openai-compatible --base-url https://api.example.test/v1 \
        --model stored-key-test --api-key-stdin --dry-run --test-dry-run)
if [[ "$stored_key_test_dry_run" != *'export TIPE_MODEL_API_KEY_FILE='* ||
    "$stored_key_test_dry_run" != *$'model-dry-run-ok\trequest-json'* ||
    "$stored_key_test_dry_run" == *"$stored_key_value"* || -e "$stored_model_config" ||
    -e "$stored_model_key" ]]; then
    echo "direct API key dry-run testing should use a private temporary file without persisting or exposing the key" >&2
    exit 1
fi
printf '%s\n' "$stored_key_value" | TIPE_MODEL_CONFIG="$stored_model_config" \
    "$ROOT/scripts/model-config.sh" --write openai-compatible --base-url https://api.example.test/v1 \
        --model stored-key-test --api-key-stdin >/dev/null
stored_model_show=$(TIPE_MODEL_CONFIG="$stored_model_config" "$ROOT/scripts/model-config.sh" --show)
stored_current_show=$(TIPE_MODEL_CONFIG="$stored_model_config" "$ROOT/scripts/model-current.sh" --show)
stored_doctor_show=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$stored_model_config" "$ROOT/scripts/doctor.sh" --no-runtime)
stored_print_env=$(TIPE_MODEL_CONFIG="$stored_model_config" "$ROOT/scripts/model-current.sh" --print-env)
stored_dry_run=$(TIPE_MODEL_CONFIG="$stored_model_config" TIPE_MODEL_DRY_RUN=1 \
    "$ROOT/scripts/model-current.sh" <"$model_explain_sample")
if [[ ! -f "$stored_model_config" || ! -f "$stored_model_key" ||
    "$(stat -c '%a' "$stored_model_config")" != "600" ||
    "$(stat -c '%a' "$stored_model_key")" != "600" ||
    "$(cat "$stored_model_key")" != "$stored_key_value" ||
    "$(cat "$stored_model_config")" == *"$stored_key_value"* ||
    "$(cat "$stored_model_config")" != *'export TIPE_MODEL_API_KEY_FILE='* ||
    "$stored_model_show" != *$'model-status\tapi-key-source\tstored-file'* ||
    "$stored_model_show" != *$'model-status\tapi-key-runtime\tset'* ||
    "$stored_current_show" != *$'api-key-source\tstored-file'* ||
    "$stored_current_show" != *$'api-key-runtime\tset'* ||
    "$stored_doctor_show" != *$'model\tapi-key-source\tstored-file'* ||
    "$stored_doctor_show" != *$'model\tapi-key-runtime\tset'* ||
    "$stored_print_env" != *'export TIPE_MODEL_API_KEY_FILE='* ||
    "$stored_print_env" == *"$stored_key_value"* ||
    "$stored_dry_run" != *$'request\thttps://api.example.test/v1/chat/completions'* ||
    "$stored_dry_run" == *"$stored_key_value"* ]]; then
    echo "model config should store a direct API key in a private file without exposing it in config, argv, or diagnostics" >&2
    exit 1
fi
chmod 0640 "$stored_model_key"
stored_invalid_show=$(TIPE_MODEL_CONFIG="$stored_model_config" "$ROOT/scripts/model-config.sh" --show)
stored_invalid_current=$(TIPE_MODEL_CONFIG="$stored_model_config" "$ROOT/scripts/model-current.sh" --show)
stored_invalid_doctor=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$stored_model_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$stored_invalid_show" != *$'model-status\tapi-key-runtime\tinvalid'* ||
    "$stored_invalid_current" != *$'api-key-runtime\tinvalid'* ||
    "$stored_invalid_doctor" != *$'model\tapi-key-runtime\tinvalid'* ]]; then
    echo "model diagnostics should report an unsafe stored API key as invalid" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$stored_model_config" TIPE_MODEL_DRY_RUN=1 \
    "$ROOT/scripts/model-current.sh" <"$model_explain_sample" >/dev/null 2>&1; then
    echo "model adapter should reject a stored API key readable by group or other users" >&2
    exit 1
fi
chmod 0600 "$stored_model_key"
TIPE_MODEL_CONFIG="$stored_model_config" "$ROOT/scripts/model-config.sh" --write heuristic \
    --clear-api-key >/dev/null
if [[ -e "$stored_model_key" ]] || rg -q 'TIPE_MODEL_API_KEY_FILE' "$stored_model_config"; then
    echo "model config should remove a stored API key only through the explicit clear action" >&2
    exit 1
fi
model_replay_dry_run_stderr="$tmp_dir/model-replay-dry-run.err"
model_replay_dry_run_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_explain_sample" --command "$ROOT/scripts/model-current.sh" \
        --config "$model_config_path" --dry-run-model 2>"$model_replay_dry_run_stderr"
)
if [[ "$model_replay_dry_run_output" != *$'request\thttps://api.example.test/v1/chat/completions'* ||
    "$model_replay_dry_run_output" != *$'request-json\t'* ||
    "$(cat "$model_replay_dry_run_stderr")" != *$'model-dry-run-ok\trequest-json'* ]]; then
    echo "model replay helper should dry-run adapter HTTP requests without network calls" >&2
    exit 1
fi
model_replay_dry_run_alias_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_explain_sample" --command "$ROOT/scripts/model-current.sh" \
        --config "$model_config_path" --dry-run 2>/dev/null
)
if [[ "$model_replay_dry_run_alias_output" != *$'request\thttps://api.example.test/v1/chat/completions'* ||
    "$model_replay_dry_run_alias_output" != *$'request-json\t'* ]]; then
    echo "model replay --dry-run alias should match --dry-run-model" >&2
    exit 1
fi
model_replay_dry_run_json="${model_replay_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$model_replay_dry_run_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
behavior = prompt.get("behavior_summary", {})
if "preedit_leading_context" not in behavior:
    sys.exit(1)
if "supervised_learning_signals" not in behavior:
    sys.exit(2)
if not any(item.get("kind") == "possible_correction" for item in behavior["supervised_learning_signals"]):
    sys.exit(3)
PY
model_replay_pass_through_request="$tmp_dir/model-replay-pass-through-request.tsv"
cat >"$model_replay_pass_through_request" <<'PASSREQ'
protocol	1
preedit
candidates
state	preedit_cursor	0	candidate_cursor	0	expanded	0
runtime_state	continuous	0
events	space:	delete:	cursor-move:Down	observed:Tab
event_counts	space:1	delete:1	cursor-move:1	observed:1
correction_events	space:	delete:	cursor-move:Down	observed:Tab
correction_event_counts	space:1	delete:1	cursor-move:1	observed:1
PASSREQ
model_replay_pass_through_stderr="$tmp_dir/model-replay-pass-through-dry-run.err"
model_replay_pass_through_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_replay_pass_through_request" \
        --command "$ROOT/scripts/model-current.sh" --config "$model_config_path" --dry-run-model \
        2>"$model_replay_pass_through_stderr"
)
if [[ "$model_replay_pass_through_output" != *$'request-json\t'* ||
    "$(cat "$model_replay_pass_through_stderr")" != *$'model-dry-run-ok\trequest-json\t'"$model_replay_pass_through_request"* ]]; then
    echo "model replay helper should dry-run empty-preedit pass-through supervision requests" >&2
    exit 1
fi
model_replay_pass_through_json="${model_replay_pass_through_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$model_replay_pass_through_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
behavior = prompt.get("behavior_summary", {})
if prompt.get("supervision_mode") != "pass-through-only":
    sys.exit(1)
if behavior.get("preedit_leading_context", {}).get("events_before_preedit") != 4:
    sys.exit(2)
if behavior.get("preedit_leading_context", {}).get("events") != [
    "space:",
    "delete:",
    "cursor-move:Down",
    "observed:Tab",
]:
    sys.exit(3)
if "supervised_learning_signals" not in behavior:
    sys.exit(4)
PY
learning_panel_dry_run_output=$(
    "$ROOT/scripts/learning-panel.sh" --replay --dry-run-model --command "$ROOT/scripts/model-current.sh" \
        --config "$model_config_path" "$model_explain_sample"
)
if [[ "$learning_panel_dry_run_output" != *$'模型回放'* ||
    "$learning_panel_dry_run_output" != *$'replay-check\tmodel-dry-run-ok\trequest-json'* ||
    "$learning_panel_dry_run_output" != *$'replay-output\trequest-json\t'* ]]; then
    echo "learning panel helper should expose model dry-run replay output" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write ollama --api-key-env 'bad-name!' >/dev/null 2>&1; then
    echo "model config helper should reject unsafe API key environment names" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write heuristic --timeout 99 >/dev/null 2>&1; then
    echo "model config helper should reject out-of-range TiPE command timeouts" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai-compatible --temperature not-a-number >/dev/null 2>&1; then
    echo "model config helper should reject non-numeric model temperatures" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai-compatible --temperature 3 >/dev/null 2>&1; then
    echo "model config helper should reject out-of-range model temperatures" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write ollama --model >/dev/null 2>&1; then
    echo "model config helper should reject missing option values clearly" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write llama-cpp \
    --model relative.gguf >/dev/null 2>&1; then
    echo "model config helper should require an absolute GGUF path" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write llama-cpp \
    --model "$llama_model_file" --llama-command relative-llama-cli >/dev/null 2>&1; then
    echo "model config helper should require an absolute llama-cli path" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write llama-cpp \
    --model "$llama_model_file" --llama-threads 0 >/dev/null 2>&1; then
    echo "model config helper should reject invalid llama.cpp thread counts" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write llama-cpp \
    --model "$llama_model_file" --llama-context 128 >/dev/null 2>&1; then
    echo "model config helper should reject undersized llama.cpp contexts" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai-compatible \
    --base-url 'file:///tmp/not-http' --model cloud-test >/dev/null 2>&1; then
    echo "model config helper should reject non-http endpoint URLs" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai-compatible \
    --base-url 'https://api.example.test/v1 bad' --model cloud-test >/dev/null 2>&1; then
    echo "model config helper should reject endpoint URLs with whitespace" >&2
    exit 1
fi
if TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write openai-compatible \
    --base-url https://api.example.test/v1 --chat-path chat/completions --model cloud-test >/dev/null 2>&1; then
    echo "model config helper should reject relative chat paths" >&2
    exit 1
fi
model_config_test_output=$(
    TIPE_MODEL_CONFIG="$model_config_path" "$ROOT/scripts/model-config.sh" --write heuristic --test
)
if [[ "$model_config_test_output" != *$'wrote\t'"$model_config_path"* ||
    "$model_config_test_output" != *$'self-test-command\t'*"model-current.sh"* ||
    "$model_config_test_output" != *$'dry-run-test-command\t'*"tipe-model-self-test --current --config $model_config_path --adapter-dry-run"* ||
    "$model_config_test_output" != *$'activation-hint\ttipe-restart-fcitx5'* ||
    "$model_config_test_output" != *$'note\tNo fcitx5 restart or input-method switch was performed.'* ||
    "$model_config_test_output" != *$'model-output\tcandidate\t我操'* ]]; then
    echo "model config helper should test written configs through model-current" >&2
    exit 1
fi
model_current_config="$tmp_dir/current-model-env"
TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-config.sh" --write heuristic >/dev/null
model_current_output=$(printf 'protocol\t1\npreedit\tihao\ncandidates\t你好\t你号\n' |
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-current.sh")
if [[ -n "$model_current_output" ]]; then
    echo "model current helper should run the configured heuristic adapter" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-config.sh" --write custom --command "$custom_model" >/dev/null
model_current_custom_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-current.sh")
if [[ "$model_current_custom_output" != $'candidate\t你号' ]]; then
    echo "model current helper should run the configured custom wrapper" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-config.sh" --write custom \
    --command "$custom_arg_command" >/dev/null
model_current_custom_arg_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-current.sh")
if [[ "$model_current_custom_arg_output" != $'candidate\t你号' ]]; then
    echo "model current helper should run configured custom wrapper commands with arguments" >&2
    exit 1
fi
model_replay_request="$tmp_dir/model-replay-request.tsv"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' >"$model_replay_request"
model_replay_stderr="$tmp_dir/model-replay.err"
model_replay_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_replay_request" --command "$ROOT/scripts/model-current.sh" \
        --config "$model_current_config" --check 2>"$model_replay_stderr"
)
if [[ "$model_replay_output" != $'candidate\t你号' ]] ||
    [[ "$(cat "$model_replay_stderr")" != wrapper-ok$'\t'"$ROOT/scripts/model-current.sh"$'\t'rows$'\t1' ]]; then
    echo "model replay helper should replay configured model-current and check output safely" >&2
    exit 1
fi
model_replay_stdin_output=$(
    cat "$model_replay_request" | "$ROOT/scripts/model-replay.sh" --request - --command "$ROOT/scripts/model-current.sh" \
        --config "$model_current_config" --check 2>/dev/null
)
if [[ "$model_replay_stdin_output" != $'candidate\t你号' ]]; then
    echo "model replay helper should accept --request - as stdin" >&2
    exit 1
fi
live_model_replay_cache="$tmp_dir/live-model-replay-cache"
mkdir -p "$live_model_replay_cache/tipe"
cp "$model_replay_request" "$live_model_replay_cache/tipe/supervision-current.tsv"
live_model_replay_output=$(
    XDG_CACHE_HOME="$live_model_replay_cache" "$ROOT/scripts/model-replay.sh" \
        --command "$ROOT/scripts/model-current.sh" --config "$model_current_config" --check 2>/dev/null
)
if [[ "$live_model_replay_output" != $'candidate\t你号' ]]; then
    echo "model replay helper should prefer the live supervision snapshot when no request path is passed" >&2
    exit 1
fi
last_model_replay_cache="$tmp_dir/last-model-replay-cache"
mkdir -p "$last_model_replay_cache/tipe"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' >"$last_model_replay_cache/tipe/supervision-last.tsv"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\twoc\tcandidates\t2\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\twoc'
    printf '%s\n' $'candidates\t我操\t我曹'
} >"$last_model_replay_cache/tipe/supervision-history.tsv"
last_model_replay_output=$(
    XDG_CACHE_HOME="$last_model_replay_cache" "$ROOT/scripts/model-replay.sh" \
        --command "$ROOT/scripts/model-current.sh" --config "$model_current_config" --check 2>/dev/null
)
if [[ "$last_model_replay_output" != $'candidate\t你号' ]]; then
    echo "model replay helper should prefer the last supervision snapshot before falling back to history" >&2
    exit 1
fi
model_replay_arg_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_replay_request" --command "$custom_arg_command" --check 2>/dev/null
)
if [[ "$model_replay_arg_output" != $'candidate\t你号' ]]; then
    echo "model replay helper should run safe model commands with arguments" >&2
    exit 1
fi
model_explain_output_model="$tmp_dir/model-explain-output.sh"
cat >"$model_explain_output_model" <<'MODELEXPLAIN'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'candidate\t你号' $'correction\tihao\tnihao'
MODELEXPLAIN
chmod +x "$model_explain_output_model"
model_replay_explain_stderr="$tmp_dir/model-replay-explain.err"
model_replay_explain_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_replay_request" --command "$model_explain_output_model" \
        --explain-output 2>"$model_replay_explain_stderr"
)
if [[ "$model_replay_explain_output" != $'candidate\t你号\ncorrection\tihao\tnihao' ||
    "$(cat "$model_replay_explain_stderr")" != *$'model-output-accepted\tcandidate\t1\t你号'* ||
    "$(cat "$model_replay_explain_stderr")" != *$'model-output-accepted\tcorrection\t1\tihao\tnihao'* ||
    "$(cat "$model_replay_explain_stderr")" != *$'model-output-summary\trows\t2\tcandidates\t1\tcorrections\t1\tpreferences\t0\tsegment-chains\t0'* ]]; then
    echo "model replay helper should explain accepted model output rows" >&2
    exit 1
fi
model_replay_learn_preferences="$tmp_dir/model-replay-learn-preferences.tsv"
printf '__correction_pattern__\tmissing\t\tn\t0\t0\t2\n__key_habit__\tmissing\t\tn\t3\n' \
    >"$model_replay_learn_preferences"
model_replay_learn_stderr="$tmp_dir/model-replay-learn.err"
model_replay_learn_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_replay_request" --command "$model_explain_output_model" \
        --learn-output --preferences "$model_replay_learn_preferences" 2>"$model_replay_learn_stderr"
)
if [[ "$model_replay_learn_output" != $'candidate\t你号\ncorrection\tihao\tnihao' ||
	    "$(cat "$model_replay_learn_stderr")" != *$'model-output-learned\tpreferences\t1\tcorrections\t1\tsegment-chains\t0\tpath\t'"$model_replay_learn_preferences"* ||
	    "$(cat "$model_replay_learn_stderr")" != *$'model-output-learned-top-preference\t1\tnihao\t你号\t2'* ||
	    "$(cat "$model_replay_learn_stderr")" != *$'model-output-learned-top-correction\t1\tihao\tnihao\t2'* ||
	    "$(cat "$model_replay_learn_stderr")" != *$'model-output-learned-top-correction-pattern\t1\t2\tmissing\tn\t0'* ||
	    "$(cat "$model_replay_learn_stderr")" != *$'model-output-preferences-summary\tpreferences\t1\t2'* ||
	    "$(cat "$model_replay_learn_stderr")" != *$'model-output-preferences-summary\tcorrections\t1\t2'* ||
	    "$(cat "$model_replay_learn_stderr")" != *$'model-output-preferences-top-preference\t1\t2\tnihao\t你号'* ||
	    "$(cat "$model_replay_learn_stderr")" != *$'model-output-preferences-top-correction\t1\t2\tihao\tnihao'* ||
	    "$(cat "$model_replay_learn_preferences")" != *$'nihao\t你号\t2'* ||
	    "$(cat "$model_replay_learn_preferences")" != *$'__correction__\tihao\tnihao\t2'* ||
	    "$(cat "$model_replay_learn_preferences")" != *$'__correction_pattern__\tmissing\t\tn\t0\t0\t2'* ||
	    "$(cat "$model_replay_learn_preferences")" != *$'__key_habit__\tmissing\t\tn\t3'* ]]; then
    echo "model replay helper should persist accepted model output rows when requested" >&2
    exit 1
fi
model_replay_full_order_request="$tmp_dir/model-replay-full-order-request.tsv"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\t你毫\t泥好\n' >"$model_replay_full_order_request"
model_replay_top_order_model="$tmp_dir/model-replay-top-order-model.sh"
cat >"$model_replay_top_order_model" <<'MODELTOPORDER'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'candidate\t你好' $'candidate\t你号' $'candidate\t你毫' $'candidate\t泥好'
MODELTOPORDER
chmod +x "$model_replay_top_order_model"
model_replay_top_order_preferences="$tmp_dir/model-replay-top-order-preferences.tsv"
model_replay_top_order_stderr="$tmp_dir/model-replay-top-order.err"
model_replay_top_order_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_replay_full_order_request" \
        --command "$model_replay_top_order_model" --learn-output \
        --preferences "$model_replay_top_order_preferences" 2>"$model_replay_top_order_stderr"
)
if [[ "$model_replay_top_order_output" != $'candidate\t你好\ncandidate\t你号\ncandidate\t你毫\ncandidate\t泥好' ||
    "$(cat "$model_replay_top_order_stderr")" != *$'model-output-learned\tpreferences\t0\tcorrections\t0\tsegment-chains\t0'* ||
    ( -e "$model_replay_top_order_preferences" && -s "$model_replay_top_order_preferences" ) ]]; then
    echo "model replay should not learn tail candidates when the model keeps the current first candidate" >&2
    exit 1
fi
model_replay_promoted_order_model="$tmp_dir/model-replay-promoted-order-model.sh"
cat >"$model_replay_promoted_order_model" <<'MODELPROMOTEDORDER'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'candidate\t你号' $'candidate\t你好' $'candidate\t你毫' $'candidate\t泥好'
MODELPROMOTEDORDER
chmod +x "$model_replay_promoted_order_model"
model_replay_promoted_order_preferences="$tmp_dir/model-replay-promoted-order-preferences.tsv"
model_replay_promoted_order_stderr="$tmp_dir/model-replay-promoted-order.err"
"$ROOT/scripts/model-replay.sh" --request "$model_replay_full_order_request" \
    --command "$model_replay_promoted_order_model" --learn-output \
    --preferences "$model_replay_promoted_order_preferences" >/dev/null 2>"$model_replay_promoted_order_stderr"
if [[ "$(cat "$model_replay_promoted_order_stderr")" != *$'model-output-learned\tpreferences\t1\tcorrections\t0\tsegment-chains\t0'* ||
    "$(cat "$model_replay_promoted_order_preferences")" != $'nihao\t你号\t2' ]]; then
    echo "model replay should learn only the candidate promoted to first place from a complete ordering" >&2
    exit 1
fi
model_replay_inactive_request="$tmp_dir/model-replay-inactive-request.tsv"
printf 'protocol\t1\npreedit\tihao\ncandidates\t以后\t一号\t你好\npreference\tihao\t一号\t1\n' \
    >"$model_replay_inactive_request"
model_replay_inactive_model="$tmp_dir/model-replay-inactive-model.sh"
cat >"$model_replay_inactive_model" <<'MODELINACTIVE'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'candidate\t一号' $'candidate\t一号' $'correction\tihao\tnihao' $'correction\tihao\tnihao'
MODELINACTIVE
chmod +x "$model_replay_inactive_model"
model_replay_inactive_preferences="$tmp_dir/model-replay-inactive-preferences.tsv"
printf 'ihao\t一号\t1\n' >"$model_replay_inactive_preferences"
model_replay_inactive_stderr="$tmp_dir/model-replay-inactive.err"
model_replay_inactive_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_replay_inactive_request" \
        --command "$model_replay_inactive_model" --learn-output \
        --preferences "$model_replay_inactive_preferences" 2>"$model_replay_inactive_stderr"
)
if [[ "$model_replay_inactive_output" != $'candidate\t一号\ncandidate\t一号\ncorrection\tihao\tnihao\ncorrection\tihao\tnihao' ||
    "$(cat "$model_replay_inactive_stderr")" != *$'model-output-learned\tpreferences\t1\tcorrections\t1\tsegment-chains\t0'* ||
    "$(cat "$model_replay_inactive_preferences")" != *$'ihao\t一号\t3'* ||
    "$(cat "$model_replay_inactive_preferences")" != *$'__correction__\tihao\tnihao\t2'* ]]; then
    echo "model replay should activate retained low-count evidence once and deduplicate repeated model rows" >&2
    exit 1
fi
model_learning_action_output_model="$tmp_dir/model-learning-action-output.sh"
cat >"$model_learning_action_output_model" <<'MODELACTION'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'preference\tnihao\t你号\t4' $'segment_chain\tnihao\tni\t你\thao\tnihao\t你好\t5'
MODELACTION
chmod +x "$model_learning_action_output_model"
model_learning_action_preferences="$tmp_dir/model-learning-action-preferences.tsv"
model_learning_action_stderr="$tmp_dir/model-learning-action.err"
model_learning_action_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_replay_request" --command "$model_learning_action_output_model" \
        --learn-output --preferences "$model_learning_action_preferences" 2>"$model_learning_action_stderr"
)
if [[ "$model_learning_action_output" != $'preference\tnihao\t你号\t4\nsegment_chain\tnihao\tni\t你\thao\tnihao\t你好\t5' ||
    "$(cat "$model_learning_action_stderr")" != *$'model-output-learned\tpreferences\t1\tcorrections\t0\tsegment-chains\t1\tpath\t'"$model_learning_action_preferences"* ||
    "$(cat "$model_learning_action_stderr")" != *$'model-output-learned-top-preference\t1\tnihao\t你好\t5'* ||
    "$(cat "$model_learning_action_stderr")" != *$'model-output-learned-top-segment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好\t5'* ||
    "$(cat "$model_learning_action_preferences")" != *$'nihao\t你号\t4'* ||
    "$(cat "$model_learning_action_preferences")" != *$'nihao\t你好\t5'* ||
    "$(cat "$model_learning_action_preferences")" != *$'__segment_chain__\tnihao\tni\t你\thao\tnihao\t你好\t5'* ]]; then
    echo "model replay helper should persist explicit model preference and segment-chain learning actions" >&2
    exit 1
fi
model_pending_segment_request="$tmp_dir/model-pending-segment-request.tsv"
printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t1\t从\n' >"$model_pending_segment_request"
model_pending_segment_output_model="$tmp_dir/model-pending-segment-output.sh"
cat >"$model_pending_segment_output_model" <<'MODELACTION'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'segment_chain\twoc\two\t我\tc\twoc\t我从\t5'
MODELACTION
chmod +x "$model_pending_segment_output_model"
model_pending_segment_preferences="$tmp_dir/model-pending-segment-preferences.tsv"
model_pending_segment_stderr="$tmp_dir/model-pending-segment.err"
model_pending_segment_output=$(
    "$ROOT/scripts/model-replay.sh" --request "$model_pending_segment_request" --command "$model_pending_segment_output_model" \
        --learn-output --preferences "$model_pending_segment_preferences" 2>"$model_pending_segment_stderr"
)
if [[ "$model_pending_segment_output" != $'segment_chain\twoc\two\t我\tc\twoc\t我从\t5' ||
    "$(cat "$model_pending_segment_stderr")" != *$'model-output-learned\tpreferences\t0\tcorrections\t0\tsegment-chains\t1\tpath\t'"$model_pending_segment_preferences"* ||
    "$(cat "$model_pending_segment_preferences")" != *$'__segment_chain__\twoc\two\t我\tc\twoc\t我从\t5'* ]]; then
    echo "model replay helper should learn segment chains confirmed from pending prefix selections" >&2
    exit 1
fi
model_bad_segment_chain_output_model="$tmp_dir/model-bad-segment-chain-output.sh"
cat >"$model_bad_segment_chain_output_model" <<'MODELACTION'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'segment_chain\tnihao\tn\t你\thao\tnihao\t你好\t5'
MODELACTION
chmod +x "$model_bad_segment_chain_output_model"
model_bad_segment_chain_preferences="$tmp_dir/model-bad-segment-chain-preferences.tsv"
model_bad_segment_chain_stderr="$tmp_dir/model-bad-segment-chain.err"
if "$ROOT/scripts/model-replay.sh" --request "$model_replay_request" --command "$model_bad_segment_chain_output_model" \
    --learn-output --preferences "$model_bad_segment_chain_preferences" >/dev/null 2>"$model_bad_segment_chain_stderr"; then
    echo "model replay helper should reject malformed segment-chain learning actions before learning" >&2
    exit 1
fi
if [[ "$(cat "$model_bad_segment_chain_stderr")" != *"segment_chain row shape is not plausible"* ||
    "$(cat "$model_bad_segment_chain_stderr")" != *$'model-output-rejected-row\t1\tsegment_chain\tnihao\tn\t你\thao'* ||
    "$(cat "$model_bad_segment_chain_stderr")" != *$'model-output-rejected-summary\trows\t1\tstatus\t1'* ||
    -s "$model_bad_segment_chain_preferences" ]]; then
    echo "model replay helper should not persist malformed segment-chain learning actions" >&2
    exit 1
fi
learning_panel_bad_segment_chain_output=$(
    "$ROOT/scripts/learning-panel.sh" --raw-panel --replay --learn-output \
        --preferences "$model_bad_segment_chain_preferences" --command "$model_bad_segment_chain_output_model" \
        "$model_replay_request"
)
if [[ "$learning_panel_bad_segment_chain_output" != *$'panel\tmodel-replay\tstatus\terror\t1'* ||
    "$learning_panel_bad_segment_chain_output" != *$'panel\tmodel-output\trejected-row\t1\tsegment_chain\tnihao\tn\t你\thao'* ||
    "$learning_panel_bad_segment_chain_output" != *$'panel\tmodel-output\trejected-summary\trows\t1\tstatus\t1'* ]]; then
    echo "learning panel raw replay should expose rejected model output rows for debugging" >&2
    exit 1
fi
model_segment_chain_suffix_model="$tmp_dir/model-segment-chain-suffix-output.sh"
cat >"$model_segment_chain_suffix_model" <<'MODELCHAIN'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' $'segment_chain\tnihao\tni\t你\thao\tnihao\t你号\t5'
MODELCHAIN
chmod +x "$model_segment_chain_suffix_model"
model_segment_chain_suffix_preferences="$tmp_dir/model-segment-chain-suffix-preferences.tsv"
"$ROOT/scripts/model-replay.sh" --request "$model_replay_request" --command "$model_segment_chain_suffix_model" \
    --learn-output --preferences "$model_segment_chain_suffix_preferences" >/dev/null 2>&1
model_segment_chain_suffix_output=$(
    printf '%s\n' 'type nihao' 'select 你' 'expect-preedit hao' 'expect-candidate 0 号' |
        "$ROOT/build/tipe-state-probe" "" --preferences "$model_segment_chain_suffix_preferences" --script -
)
if [[ "$model_segment_chain_suffix_output" != *$'commit\t你'* ||
    "$model_segment_chain_suffix_output" != *$'preedit\thao'* ||
    "$model_segment_chain_suffix_output" != *$'candidate\t0\t号'* ]]; then
    echo "learned segment-chain rows should rerank the remaining suffix after a later prefix selection" >&2
    exit 1
fi
learning_panel_action_preferences="$tmp_dir/learning-panel-action-preferences.tsv"
learning_panel_action_raw_output=$(
    "$ROOT/scripts/learning-panel.sh" --raw-panel --replay --explain-output --learn-output \
        --preferences "$learning_panel_action_preferences" --command "$model_learning_action_output_model" \
        "$model_replay_request"
)
if [[ "$learning_panel_action_raw_output" != *$'panel\tmodel-output\taccepted-preference\t1\tnihao\t你号'* ||
    "$learning_panel_action_raw_output" != *$'panel\tmodel-output\taccepted-segment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好'* ||
    "$learning_panel_action_raw_output" != *$'panel\tmodel-output\tsummary\trows\t2\tcandidates\t0\tcorrections\t0\tpreferences\t1\tsegment-chains\t1'* ||
    "$learning_panel_action_raw_output" != *$'panel\tmodel-output\tlearned\tpreferences\t1\tcorrections\t0\tsegment-chains\t1\tpath\t'"$learning_panel_action_preferences"* ]]; then
    echo "learning panel raw replay should expose explicit accepted preference and segment-chain learning actions" >&2
    exit 1
fi
learning_panel_explain_output=$(
    "$ROOT/scripts/learning-panel.sh" --replay --explain-output --command "$model_explain_output_model" \
        "$model_replay_request"
)
if [[ "$learning_panel_explain_output" != *$'replay-check\tmodel-output-accepted\tcandidate\t1\t你号'* ||
    "$learning_panel_explain_output" != *$'replay-check\tmodel-output-summary\trows\t2\tcandidates\t1\tcorrections\t1\tpreferences\t0\tsegment-chains\t0'* ||
    "$learning_panel_explain_output" != *$'replay-output\tcorrection\tihao\tnihao'* ]]; then
    echo "learning panel helper should expose explained replay output rows" >&2
    exit 1
fi
learning_panel_learn_preferences="$tmp_dir/learning-panel-learn-preferences.tsv"
learning_panel_learn_output=$(
    "$ROOT/scripts/learning-panel.sh" --replay --learn-output --preferences "$learning_panel_learn_preferences" \
        --command "$model_explain_output_model" "$model_replay_request"
)
if [[ "$learning_panel_learn_output" != *$'replay-check\tmodel-output-learned\tpreferences\t1\tcorrections\t1\tsegment-chains\t0\tpath\t'"$learning_panel_learn_preferences"* ||
    "$(cat "$learning_panel_learn_preferences")" != *$'nihao\t你号\t2'* ||
    "$(cat "$learning_panel_learn_preferences")" != *$'__correction__\tihao\tnihao\t2'* ]]; then
    echo "learning panel helper should persist replay output when learning is requested" >&2
    exit 1
fi
learning_panel_raw_learn_output=$(
    "$ROOT/scripts/learning-panel.sh" --raw-panel --replay --learn-output --preferences "$learning_panel_learn_preferences" \
        --command "$model_explain_output_model" "$model_replay_request"
)
if [[ "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tlearned\tpreferences\t0\tcorrections\t0\tsegment-chains\t0\tpath\t'"$learning_panel_learn_preferences"* ]]; then
    echo "learning panel raw replay should expose structured learned-output rows without re-learning existing rows" >&2
    exit 1
fi
if [[ "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tlearned-top-preference\t1\tnihao\t你号\t'* ||
    "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tlearned-top-correction\t1\tihao\tnihao\t'* ||
    "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tnote\tno-new-learning\tnihao\talready-known-or-no-new-safe-row'* ||
    "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tlearned-top-correction-pattern\t1\t'*$'\tmissing\tn\t0'* ||
    "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tpreferences-summary\tpreferences\t1\t'* ||
    "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tpreferences-summary\tcorrections\t1\t'* ||
    "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tpreferences-top-preference\t1\t'*$'\tnihao\t你号'* ||
    "$learning_panel_raw_learn_output" != *$'panel\tmodel-output\tpreferences-top-correction\t1\t'*$'\tihao\tnihao'* ]]; then
    echo "learning panel raw replay should expose top learned rows after learn-output" >&2
    exit 1
fi
learning_panel_raw_replay_output=$(
    "$ROOT/scripts/learning-panel.sh" --raw-panel --replay --explain-output --command "$model_explain_output_model" \
        "$model_replay_request"
)
if [[ "$learning_panel_raw_replay_output" != *$'panel\tmodel-replay\tstatus\tok'* ||
    "$learning_panel_raw_replay_output" != *$'panel\tmodel-output\taccepted-candidate\t1\t你号'* ||
    "$learning_panel_raw_replay_output" != *$'panel\tmodel-output\taccepted-correction\t1\tihao\tnihao'* ||
    "$learning_panel_raw_replay_output" != *$'panel\tmodel-output\tsummary\trows\t2\tcandidates\t1\tcorrections\t1\tpreferences\t0\tsegment-chains\t0'* ||
    "$learning_panel_raw_replay_output" != *$'panel\tmodel-output\trow-correction\t2\tihao\tnihao'* ]]; then
    echo "learning panel raw replay should expose model output supervision rows for the window" >&2
    exit 1
fi
if [[ -x "$ROOT/build/tipe-learning-panel-window" ]]; then
    learning_panel_window_replay_parse=$(printf '%s\n' "$learning_panel_raw_replay_output" |
        "$ROOT/build/tipe-learning-panel-window" --parse-panel -)
    if [[ "$learning_panel_window_replay_parse" != *$'model-replay\tstatus\tok'* ||
        "$learning_panel_window_replay_parse" != *$'model-output\taccepted-candidate\t1\t你号'* ||
        "$learning_panel_window_replay_parse" != *$'model-output\trow-correction\t2\tihao\tnihao'* ]]; then
        echo "learning panel window parser should consume replay supervision rows" >&2
        exit 1
    fi
fi
learning_panel_window_replay_count="$tmp_dir/learning-panel-window-replay-count"
learning_panel_window_replay_model="$tmp_dir/learning-panel-window-replay-model.sh"
cat >"$learning_panel_window_replay_model" <<MODEL
#!/usr/bin/env bash
set -euo pipefail
count_file="$learning_panel_window_replay_count"
count=0
if [[ -r "\$count_file" ]]; then
    count=\$(cat "\$count_file")
fi
printf '%s\n' "\$((count + 1))" >"\$count_file"
cat >/dev/null
printf '%s\n' \$'candidate\t你号'
MODEL
chmod +x "$learning_panel_window_replay_model"
learning_panel_window_replay_mock="$tmp_dir/learning-panel-window-replay-mock.sh"
cat >"$learning_panel_window_replay_mock" <<'WINREPLAY'
#!/usr/bin/env bash
set -euo pipefail
printf 'window-args'
for arg in "$@"; do
    printf '\t%s' "$arg"
done
printf '\n'
panel_path=""
for arg in "$@"; do
    if [[ -f "$arg" ]] && awk -F '\t' '$1 == "panel" {found=1; exit} END {exit found ? 0 : 1}' "$arg"; then
        panel_path="$arg"
    fi
done
test -n "$panel_path"
awk -F '\t' '$1 == "panel" {print; seen=1} END {exit seen ? 0 : 1}' "$panel_path"
sleep 2
WINREPLAY
chmod +x "$learning_panel_window_replay_mock"
learning_panel_window_replay_preferences="$tmp_dir/learning-panel-window-replay-preferences.tsv"
printf '__raw_token__\tto\t3\n' >"$learning_panel_window_replay_preferences"
learning_panel_window_replay_output=$(
    TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_replay_mock" TIPE_LEARNING_PANEL_REFRESH_SECONDS=1 \
        "$ROOT/scripts/learning-panel.sh" --window --replay --explain-output --learn-output \
        --preferences "$learning_panel_window_replay_preferences" \
        --command "$learning_panel_window_replay_model" "$model_replay_request"
)
if [[ "$learning_panel_window_replay_output" != *$'window-args\t--title\tTiPE 学习面板'* ||
    "$learning_panel_window_replay_output" != *$'\t--refresh-command\t'* ||
    "$learning_panel_window_replay_output" != *$'panel\tmodel-output\taccepted-candidate\t1\t你号'* ||
    "$learning_panel_window_replay_output" != *$'panel\tmodel-output\tlearned\tpreferences\t1\tcorrections\t0\tsegment-chains\t0\tpath\t'"$learning_panel_window_replay_preferences"* ||
    "$(cat "$learning_panel_window_replay_count")" != "1" ||
    "$(cat "$learning_panel_window_replay_preferences")" != *$'nihao\t你号\t2'* ||
    "$(cat "$learning_panel_window_replay_preferences")" != *$'__raw_token__\tto\t3'* ]]; then
    echo "learning panel replay window should expose an Analyze refresh command without repeatedly calling the model" >&2
    exit 1
fi
rm -f "$learning_panel_window_replay_count"
learning_panel_window_defer_output=$(
    TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_replay_mock" TIPE_LEARNING_PANEL_REFRESH_SECONDS=1 \
        "$ROOT/scripts/learning-panel.sh" --window --replay --defer-replay --explain-output \
        --command "$learning_panel_window_replay_model" "$model_replay_request"
)
if [[ "$learning_panel_window_defer_output" != *$'window-args\t--title\tTiPE 学习面板'* ||
    "$learning_panel_window_defer_output" != *$'\t--refresh-command\t'* ||
    "$learning_panel_window_defer_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$learning_panel_window_defer_output" != *$'panel\tmodel-replay\tmode\tmanual\tlearn-output\t0\ttrigger\tAnalyze'* ||
    "$learning_panel_window_defer_output" == *$'panel\tmodel-output\taccepted-candidate\t1\t你号'* ||
    -e "$learning_panel_window_replay_count" ]]; then
    echo "learning panel deferred replay window should wait for Analyze before calling the model" >&2
    exit 1
fi
learning_panel_live_request="$tmp_dir/learning-panel-live-request.tsv"
printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' >"$learning_panel_live_request"
learning_panel_live_mock="$tmp_dir/learning-panel-live-mock.sh"
cat >"$learning_panel_live_mock" <<'WINLIVE'
#!/usr/bin/env bash
set -euo pipefail
panel_path=""
for arg in "$@"; do
    if [[ -f "$arg" ]] && awk -F '\t' '$1 == "panel" {found=1; exit} END {exit found ? 0 : 1}' "$arg"; then
        panel_path="$arg"
    fi
done
test -n "$panel_path"
printf 'protocol\t1\npreedit\tshijie\ncandidates\t世界\t时节\n' >"$TIPE_TEST_LIVE_REQUEST"
sleep 1.2
awk -F '\t' '$1 == "panel" {print; seen=1} END {exit seen ? 0 : 1}' "$panel_path"
WINLIVE
chmod +x "$learning_panel_live_mock"
learning_panel_live_output=$(
    TIPE_TEST_LIVE_REQUEST="$learning_panel_live_request" \
        TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_live_mock" TIPE_LEARNING_PANEL_REFRESH_SECONDS=0.1 \
        "$ROOT/scripts/learning-panel.sh" --window --replay --defer-replay --explain-output \
        --command "$learning_panel_window_replay_model" "$learning_panel_live_request"
)
if [[ "$learning_panel_live_output" != *$'panel\tstate\tpreedit\tshijie'* ||
    "$learning_panel_live_output" != *$'panel\tmodel-replay\tmode\tmanual\tlearn-output\t0\ttrigger\tAnalyze'* ||
    -e "$learning_panel_window_replay_count" ]]; then
    echo "deferred analysis windows should refresh supervised input without calling the model" >&2
    exit 1
fi
analyze_window_preferences="$tmp_dir/analyze-window-preferences.tsv"
rm -f "$learning_panel_window_replay_count"
analyze_window_output=$(
    TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_replay_mock" TIPE_LEARNING_PANEL_REFRESH_SECONDS=1 \
        "$ROOT/scripts/analyze-window.sh" --learn-output --preferences "$analyze_window_preferences" \
        --command "$learning_panel_window_replay_model" "$model_replay_request"
)
if [[ "$analyze_window_output" != *$'window-args\t--title\tTiPE 分析窗口'* ||
    "$analyze_window_output" != *$'\t--refresh-command\t'* ||
    "$analyze_window_output" != *$'panel\tmodel-replay\tmode\tmanual\tlearn-output\t1\ttrigger\tAnalyze'* ||
    "$analyze_window_output" == *$'panel\tmodel-output\taccepted-candidate\t1\t你号'* ||
    -e "$learning_panel_window_replay_count" ||
    -e "$analyze_window_preferences" ]]; then
    echo "analyze-window helper should default to clicked analysis without calling the model on open" >&2
    exit 1
fi
analyze_window_run_on_open_preferences="$tmp_dir/analyze-window-run-on-open-preferences.tsv"
rm -f "$learning_panel_window_replay_count"
analyze_window_run_on_open_output=$(
    TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_replay_mock" TIPE_LEARNING_PANEL_REFRESH_SECONDS=1 \
        "$ROOT/scripts/analyze-window.sh" --run-on-open --learn-output \
        --preferences "$analyze_window_run_on_open_preferences" \
        --command "$learning_panel_window_replay_model" "$model_replay_request"
)
if [[ "$analyze_window_run_on_open_output" != *$'window-args\t--title\tTiPE 分析窗口'* ||
    "$analyze_window_run_on_open_output" != *$'\t--refresh-command\t'* ||
    "$analyze_window_run_on_open_output" != *$'panel\tmodel-output\tlearned\tpreferences\t1\tcorrections\t0\tsegment-chains\t0\tpath\t'"$analyze_window_run_on_open_preferences"* ||
    "$(cat "$learning_panel_window_replay_count")" != "1" ||
    "$(cat "$analyze_window_run_on_open_preferences")" != *$'nihao\t你号\t2'* ]]; then
    echo "analyze-window helper should support explicit run-on-open analysis" >&2
    exit 1
fi
live_analyze_cache="$tmp_dir/live-analyze-cache"
live_analyze_preferences="$tmp_dir/live-analyze-preferences.tsv"
mkdir -p "$live_analyze_cache/tipe"
cp "$model_replay_request" "$live_analyze_cache/tipe/supervision-current.tsv"
rm -f "$learning_panel_window_replay_count"
analyze_window_live_output=$(
    XDG_CACHE_HOME="$live_analyze_cache" TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_replay_mock" \
        TIPE_LEARNING_PANEL_REFRESH_SECONDS=1 "$ROOT/scripts/analyze-window.sh" --learn-output \
        --preferences "$live_analyze_preferences" --command "$learning_panel_window_replay_model"
)
if [[ "$analyze_window_live_output" != *$'window-args\t--title\tTiPE 分析窗口'* ||
    "$analyze_window_live_output" != *$'\t--refresh-command\t'* ||
    "$analyze_window_live_output" != *$'panel\tstate\trequest-source\tlive-supervision\t'* ||
    "$analyze_window_live_output" != *$'panel\tmodel-replay\tmode\tmanual\tlearn-output\t1\ttrigger\tAnalyze'* ||
    "$analyze_window_live_output" == *$'panel\tmodel-output\taccepted-candidate\t1\t你号'* ||
    -e "$learning_panel_window_replay_count" ||
    -e "$live_analyze_preferences" ]]; then
    echo "analyze-window helper should prefer live supervision but wait for clicked analysis" >&2
    exit 1
fi
missing_analyze_cache="$tmp_dir/missing-analyze-cache"
missing_analyze_preferences="$tmp_dir/missing-analyze-preferences.tsv"
mkdir -p "$missing_analyze_cache/tipe"
rm -f "$learning_panel_window_replay_count"
analyze_window_wait_output=$(
    XDG_CACHE_HOME="$missing_analyze_cache" TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_replay_mock" \
        TIPE_LEARNING_PANEL_REFRESH_SECONDS=1 "$ROOT/scripts/analyze-window.sh" --learn-output \
        --preferences "$missing_analyze_preferences" --command "$learning_panel_window_replay_model"
)
if [[ "$analyze_window_wait_output" != *$'window-args\t--title\tTiPE 分析窗口'* ||
    "$analyze_window_wait_output" != *$'\t--refresh-command\t'* ||
    "$analyze_window_wait_output" != *$'panel\tstate\tstatus\twaiting-for-live-supervision'* ||
    "$analyze_window_wait_output" != *$'panel\tmodel-replay\tmode\tmanual\tlearn-output\t1\ttrigger\tAnalyze'* ||
    -e "$learning_panel_window_replay_count" ]]; then
    echo "analyze-window helper should open and wait without calling a model when no live supervision snapshot exists" >&2
    exit 1
fi
last_analyze_cache="$tmp_dir/last-analyze-cache"
last_analyze_preferences="$tmp_dir/last-analyze-preferences.tsv"
mkdir -p "$last_analyze_cache/tipe"
cp "$model_replay_request" "$last_analyze_cache/tipe/supervision-last.tsv"
rm -f "$learning_panel_window_replay_count"
analyze_window_last_output=$(
    XDG_CACHE_HOME="$last_analyze_cache" TIPE_LEARNING_PANEL_WINDOW_COMMAND="$learning_panel_window_replay_mock" \
        TIPE_LEARNING_PANEL_REFRESH_SECONDS=1 "$ROOT/scripts/analyze-window.sh" --learn-output \
        --preferences "$last_analyze_preferences" --command "$learning_panel_window_replay_model"
)
if [[ "$analyze_window_last_output" != *$'window-args\t--title\tTiPE 分析窗口'* ||
    "$analyze_window_last_output" != *$'\t--refresh-command\t'* ||
    "$analyze_window_last_output" != *$'panel\tstate\tpreedit\tnihao'* ||
    "$analyze_window_last_output" != *$'panel\tstate\tstatus\tshowing-last-supervision'* ||
    "$analyze_window_last_output" != *$'panel\tmodel-replay\tmode\tmanual\tlearn-output\t1\ttrigger\tAnalyze'* ||
    -e "$learning_panel_window_replay_count" ]]; then
    echo "analyze-window helper should show the last supervision snapshot while waiting without calling a model" >&2
    exit 1
fi
model_self_test_output=$("$ROOT/scripts/model-self-test.sh")
if [[ "$model_self_test_output" != *$'self-test-command\t/usr/bin/env TIPE_MODEL_BACKEND=heuristic '*"model-adapter.sh"* ||
    "$model_self_test_output" != *$'wrapper-ok\t/usr/bin/env TIPE_MODEL_BACKEND=heuristic '*$'\trows\t2'* ||
    "$model_self_test_output" != *$'model-output\tcandidate\t我操'* ||
    "$model_self_test_output" != *$'model-output\tcorrection\twoc\twocao'* ||
    "$model_self_test_output" == *$'model-output\tpreference\twoc\t我操\t2'* ||
    "$model_self_test_output" == *$'model-output\tsegment_chain\twoc\two\t我\tc\twocao\t我操\t1'* ||
    "$model_self_test_output" != *$'pending-check\twrapper-ok\t/usr/bin/env TIPE_MODEL_BACKEND=heuristic '*$'\trows\t3'* ||
    "$model_self_test_output" != *$'model-output-pending\tsegment_chain\twoc\two\t我\tc\twoc\t我从\t1'* ||
    "$model_self_test_output" != *$'model-output-pending\tcandidate\t从'* ||
    "$model_self_test_output" != *$'model-output-pending\tpreference\tc\t从\t2'* ]]; then
    echo "model self-test helper should validate the offline heuristic model hook" >&2
    exit 1
fi
model_self_test_dry_run_output=$("$ROOT/scripts/model-self-test.sh" --adapter-dry-run)
if [[ "$model_self_test_dry_run_output" != *$'self-test-command\t/usr/bin/env TIPE_MODEL_BACKEND=openai-compatible '*"model-adapter.sh"* ||
    "$model_self_test_dry_run_output" != *$'model-dry-run-request\t'* ||
    "$model_self_test_dry_run_output" != *$'model-dry-run-ok\trequest-json'* ||
    "$model_self_test_dry_run_output" != *$'model-dry-run-ok\tpass-through-request-json'* ||
    "$model_self_test_dry_run_output" != *$'model-dry-run-ok\tsegment-chain-suffix-request-json'* ||
    "$model_self_test_dry_run_output" != *$'model-dry-run-ok\tpending-segment-request-json'* ||
    "$model_self_test_dry_run_output" != *$'model-dry-run-ok\tgeneralized-correction-request-json'* ||
    "$model_self_test_dry_run_output" != *$'model-dry-run-ok\tguarded-long-request-json'* ]]; then
    echo "model self-test helper should validate adapter dry-run JSON without network calls" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-config.sh" --write heuristic >/dev/null
model_self_test_current_output=$(
    "$ROOT/scripts/model-self-test.sh" --current --config "$model_current_config"
)
if [[ "$model_self_test_current_output" != *$'self-test-command\t'*"model-current.sh"* ||
    "$model_self_test_current_output" != *$'wrapper-ok\t'*"model-current.sh"* ]]; then
    echo "model self-test helper should validate model-current with an explicit config" >&2
    exit 1
fi
single_call_model="$tmp_dir/single-call-model.sh"
single_call_count="$tmp_dir/single-call-count"
cat >"$single_call_model" <<SINGLE
#!/usr/bin/env bash
set -euo pipefail
cat >/dev/null
count=0
if [[ -f "$single_call_count" ]]; then
    count=\$(cat "$single_call_count")
fi
printf '%s\n' \$((count + 1)) >"$single_call_count"
printf '%s\n' \$'candidate\t你号'
SINGLE
chmod +x "$single_call_model"
single_call_output=$("$ROOT/scripts/model-replay.sh" --request "$model_replay_request" --command "$single_call_model" --check 2>/dev/null)
if [[ "$single_call_output" != $'candidate\t你号' ]] || [[ "$(cat "$single_call_count")" != "1" ]]; then
    echo "model replay --check should validate the captured output without calling the model twice" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-config.sh" --write custom \
    --command "$custom_arg_command" >/dev/null
doctor_cache="$tmp_dir/doctor-cache"
mkdir -p "$doctor_cache/tipe"
printf '%s\n' \
    $'wayland\tcreated\tname=\tdisplay=100\tfocusDisplay=wayland:\tcompositor=101\tshm=102\tseat=103\tpointer=0' \
    $'wayland\tpointer-ready\tname=\tdisplay=100\tpointer=104' \
    $'popup\tcandidate-click\tindex=2' \
    $'popup\tstatus-rendered\tw=54\th=26\tscale=2' \
    $'popup\tstatus-render-with-candidate-stale-rect\ttext=TiPE' \
    $'popup\tstatus-edge-fallback\tstatus=TiPE\trect=32,901,21,24\tpid=1234' \
    $'popup\tdefer-candidate-render\tawaiting-fresh-text-rect' \
    $'popup\ttext-rect-rerender\tdisplay=' \
    $'popup\thidden\ttext-rect-stale=1' \
    $'popup\tcandidate-edge-fallback' \
    $'popup\tcandidate-edge-fallback-terminate\treason=hide-popup' \
    $'popup\trendered\tw=486\th=68\tscale=2\tpixelW=972\tpixelH=136\tcandidates=6\tcursor=0\tpreeditCursor=5\texpanded=0\trows=1\tcolumnW=74\tmaxRight=444\tboundsOk=1' \
    >"$doctor_cache/tipe/tipeui.log"
for ((index = 1; index <= 1200; ++index)); do
    printf 'popup\trendered\tw=486\th=68\tindex=%s\tboundsOk=1\n' "$index" >>"$doctor_cache/tipe/tipeui.log"
done
printf '%s\n' \
    $'fallback\tposition\tmode=candidate\tpreedit=nihao\texpanded=1\tcursor=1240,700,2,18\tmonitor=0,0,1280,720\tleft=660\ttop=552\twidth=596\theight=96\tright=1256\tbottom=648\tboundsOk=1' \
    $'fallback\tposition\tmode=status\tpreedit=\texpanded=0\tcursor=32,901,21,24\tmonitor=0,0,1560,1040\tleft=32\ttop=871\twidth=54\theight=26\tright=86\tbottom=897\tboundsOk=1' \
    >"$doctor_cache/tipe/candidate-window.log"
printf '%s\n' \
    'wayland-popup-edge-fallback rect=1240,700,2,18 candidates=40' \
    'status-panel-show status=TiPE' \
    'status-window-fallback source=live rect=320,640,20,24 status=Eng frontend=dbus program=zen pid=4321' \
    'model-async-start serial=4 preedit=nihao' \
    'model-async-finish serial=4 result=applied' \
    'continuous-mode toggled=1 active-preedit=0' \
    'preserve-state reason=deactivate preedit=nihao cursor=5 expanded=0 inputContext=1 frontend=wayland program=Editor display=wayland-0 reason=deactivate surrounding=0 preserved=1' \
    'restore-state allowing-same-input-context saved=Editor current=EditorHelper frontend=same display=changed' \
    'restore-state allowing-program-change saved=Editor current=editor frontend=same display=same surrounding=different-or-unavailable' \
    'restore-state preedit=nihao candidates=8 remaining=0' \
    >"$doctor_cache/tipe/engine-trace.log"
doctor_custom_output=$(XDG_CACHE_HOME="$doctor_cache" XDG_DATA_HOME="$tmp_dir/doctor-data" \
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_custom_output" != *$'env\tTIPE_MODEL_CUSTOM_COMMAND\t'* ||
    "$doctor_custom_output" != *$'env\tTIPE_WAYLAND_POPUP_EDGE_FALLBACK\t0'* ||
    "$doctor_custom_output" != *$'ok\tmodel-custom\t'*"custom-arg-model.sh"* ||
    "$doctor_custom_output" != *$'section\tmodel\t'* ||
    "$doctor_custom_output" != *$'model\tconfigured-mode\tcustom'* ||
    "$doctor_custom_output" != *$'model\tkind\tcustom-wrapper'* ||
    "$doctor_custom_output" != *$'model\tclick-trigger\tF9'* ||
    "$doctor_custom_output" != *$'model\tcontinuous-mode\tlocal-light-rerank'* ||
    "$doctor_custom_output" != *$'model\tcontinuous-default\t0'* ||
    "$doctor_custom_output" != *$'model\tcontinuous-toggle\tShift+F9'* ||
    "$doctor_custom_output" != *$'model\tanalyze-window\t'*"tipe-analyze-window"* ||
    "$doctor_custom_output" != *$'model\tsupervision-window\t'*"tipe-supervision-window"* ||
    "$doctor_custom_output" != *$'model\tanalyze-learn\t'*"tipe-analyze-window --learn-output"* ||
    "$doctor_custom_output" != *$'model\ttraining-export\t'*"tipe-training-export --stats"* ||
    "$doctor_custom_output" != *$'model\tpersonal-model\t'*"personal-reranker.json"* ||
    "$doctor_custom_output" != *$'model\tpersonal-model-status\tuntrained'* ||
    "$doctor_custom_output" != *$'model\tpersonal-model-train\t'*"tipe-personal-model-train"* ||
    "$doctor_custom_output" != *$'model\tself-test-command\t'*"tipe-model-self-test --current --config $model_current_config"* ||
    "$doctor_custom_output" != *$'model\tdry-run-test-command\t'*"tipe-model-self-test --current --config $model_current_config --adapter-dry-run"* ||
    "$doctor_custom_output" != *$'model\tdry-run-test-supported\t0'* ||
    "$doctor_custom_output" != *$'model\tconfigured-command\t'*"tipe-model-current"* ||
    "$doctor_custom_output" != *$'model\tconfigured-command-valid\t1'* ||
    "$doctor_custom_output" != *$'model\tprocess-command\tunset'* ||
    "$doctor_custom_output" != *$'model\tprocess-command-active-scope\tcurrent-shell-only-not-fcitx5-runtime'* ||
    "$doctor_custom_output" != *$'model\truntime-verification\truntime section below is authoritative when available'* ||
    "$doctor_custom_output" != *$'model\tprocess-command-active\t0'* ||
    "$doctor_custom_output" != *$'model\tactivation-hint\ttipe-restart-fcitx5'* ||
    "$doctor_custom_output" != *$'section\tdictionary\t'* ||
    "$doctor_custom_output" != *$'dictionary-sample\tnihao\tbase=你好'* ||
    "$doctor_custom_output" != *$'dictionary-sample\tgithub\tbase=github'* ||
    "$doctor_custom_output" != *$'dictionary-sample\twoc\tbase=我操'* ||
    "$doctor_custom_output" != *$'ok\ttipeui-log\tcandidate popup draw bounds ok'* ||
    "$doctor_custom_output" != *$'ok\ttipeui-log\tcandidate pointer input is available'* ||
    "$doctor_custom_output" != *$'ok\ttipeui-log\tcandidate mouse selection observed'* ||
    "$doctor_custom_output" != *$'ok\ttipeui-log\tstatus popup rendered'* ||
    "$doctor_custom_output" != *$'info\ttipeui-log\tpopup\tstatus-render-with-candidate-stale-rect\ttext=TiPE'* ||
    "$doctor_custom_output" != *$'ok\ttipeui-log\tpopup hide invalidated stale text rect'* ||
    "$doctor_custom_output" != *$'info\ttipeui-log\thistorical candidate edge fallback observed; normal Wayland input now stays on the compositor popup path'* ||
    "$doctor_custom_output" != *$'ok\ttipeui-log\tcandidate fallback termination observed'* ||
    "$doctor_custom_output" != *$'ok\tcandidate-window-log\tGTK fallback window position inside monitor'* ||
    "$doctor_custom_output" != *$'ok\tcandidate-window-log\tGTK status fallback position inside monitor'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\thistorical candidate edge fallback observed; disabled by default'* ||
    "$doctor_custom_output" != *$'ok\tengine-trace\tTiPE input-mode status activation observed'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\tstatus-panel-show status=TiPE'* ||
    "$doctor_custom_output" != *$'ok\tengine-trace\tstatus GTK fallback shown for non-wayland frontend'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\tstatus-window-fallback source=live rect=320,640,20,24 status=Eng frontend=dbus program=zen pid=4321'* ||
    "$doctor_custom_output" != *$'ok\tengine-trace\tpreedit preserve observed'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\tpreserve-state reason=deactivate preedit=nihao'* ||
    "$doctor_custom_output" != *$'ok\tengine-trace\tpreedit restore observed'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\trestore-state preedit=nihao candidates=8 remaining=0'* ||
    "$doctor_custom_output" != *$'ok\tengine-trace\tpreedit restore allowed same input context'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\trestore-state allowing-same-input-context saved=Editor current=EditorHelper'* ||
    "$doctor_custom_output" != *$'ok\tengine-trace\tpreedit restore allowed changed program metadata'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\trestore-state allowing-program-change saved=Editor current=editor'* ||
    "$doctor_custom_output" != *$'ok\tengine-trace\tasynchronous model request observed'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\tmodel-async-finish serial=4 result=applied'* ||
    "$doctor_custom_output" != *$'ok\tengine-trace\tcontinuous mode toggle observed'* ||
    "$doctor_custom_output" != *$'info\tengine-trace\tcontinuous-mode toggled=1 active-preedit=0'* ||
    "$doctor_custom_output" == *$'warn\tcandidate-window-log\tGTK status fallback used invalid cursor rect'* ]]; then
    echo "doctor helper should report configured custom model command status" >&2
    exit 1
fi
printf '%s\n' \
    $'popup\tcandidate-frontend-fallback-start\tpid=1234\tevents=ready' \
    $'popup\tfrontend-fallback-anchor\tfrontend=xim\tsource=pointer\traw=0,0,0,0\tlogical=0,0,0,0' \
    $'popup\tcandidate-frontend-fallback' \
    >"$doctor_cache/tipe/tipeui.log"
doctor_frontend_fallback_output=$(XDG_CACHE_HOME="$doctor_cache" XDG_DATA_HOME="$tmp_dir/doctor-data" \
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_frontend_fallback_output" != *$'ok\ttipeui-log\tcandidate pointer input is available through GTK fallback'* ]] ||
    [[ "$doctor_frontend_fallback_output" != *$'ok\ttipeui-log\tcandidate GTK frontend fallback rendered'* ]] ||
    [[ "$doctor_frontend_fallback_output" != *$'info\ttipeui-log\tpopup\tfrontend-fallback-anchor\tfrontend=xim\tsource=pointer'* ]]; then
    echo "doctor helper should report the unified GTK frontend fallback and pointer channel" >&2
    exit 1
fi
printf '%s\n' \
    $'popup\tstatus-edge-fallback\tstatus=TiPE\trect=32,901,21,24\tpid=1234' \
    $'popup\thidden\ttext-rect-stale=1' \
    $'popup\tcandidate-edge-fallback' \
    $'popup\tcandidate-edge-fallback-terminate\treason=hide-popup' \
    $'popup\trendered\tw=486\th=68\tscale=2\tpixelW=972\tpixelH=136\tcandidates=6\tcursor=0\tpreeditCursor=5\texpanded=0\trows=1\tcolumnW=74\tmaxRight=444\tboundsOk=1' \
    >"$doctor_cache/tipe/tipeui.log"
printf '%s\n' \
    $'fallback\tposition\tmode=candidate\tpreedit=nihao\texpanded=1\tcursor=1240,700,2,18\tmonitor=0,0,1280,720\tleft=660\ttop=552\twidth=596\theight=96\tright=1256\tbottom=648\tboundsOk=1' \
    $'fallback\tposition\tmode=status\tpreedit=\texpanded=0\tcursor=32,901,21,24\tmonitor=0,0,1560,1040\tleft=32\ttop=871\twidth=54\theight=26\tright=86\tbottom=897\tboundsOk=1' \
    >"$doctor_cache/tipe/candidate-window.log"
doctor_status_edge_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_cache" \
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_status_edge_output" != *$'ok\ttipeui-log\tstatus popup edge fallback observed'* ||
    "$doctor_status_edge_output" != *$'info\ttipeui-log\tpopup\tstatus-edge-fallback\tstatus=TiPE\trect=32,901,21,24\tpid=1234'* ||
    "$doctor_status_edge_output" != *$'ok\tcandidate-window-log\tGTK status fallback position inside monitor'* ||
    "$doctor_status_edge_output" == *$'ok\ttipeui-log\tstatus popup rendered'* ||
    "$doctor_status_edge_output" == *$'warn\ttipeui-log\tno status popup render row'* ||
    "$doctor_status_edge_output" == *$'warn\tcandidate-window-log\tGTK status fallback used invalid cursor rect'* ]]; then
    echo "doctor helper should treat TiPE UI status edge fallback as healthy" >&2
    exit 1
fi
printf '%s\n' \
    $'popup\tstatus-rendered\tw=54\th=26\tscale=2' \
    $'popup\trendered\tw=486\th=68\tscale=2\tpixelW=972\tpixelH=136\tcandidates=6\tcursor=0\tpreeditCursor=5\texpanded=0\trows=1\tcolumnW=74\tmaxRight=520\tboundsOk=0' \
    >"$doctor_cache/tipe/tipeui.log"
printf '%s\n' \
    $'fallback\tposition\tmode=candidate\tpreedit=nihao\texpanded=1\tcursor=1240,700,2,18\tmonitor=0,0,1280,720\tleft=692\ttop=644\twidth=596\theight=96\tright=1288\tbottom=740\tboundsOk=0' \
    >"$doctor_cache/tipe/candidate-window.log"
doctor_bounds_warn_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_cache" \
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_bounds_warn_output" != *$'warn\ttipeui-log\tcandidate popup draw bounds exceeded panel padding'* ]]; then
    echo "doctor helper should warn when live candidate popup draw bounds exceed the panel" >&2
    exit 1
fi
if [[ "$doctor_bounds_warn_output" != *$'warn\tcandidate-window-log\tGTK fallback window position exceeded monitor bounds'* ]]; then
    echo "doctor helper should warn when GTK fallback window position exceeds the monitor" >&2
    exit 1
fi
printf '%s\n' \
    'status-window-edge-fallback rect=0,0,0,0 status=TiPE' \
    >"$doctor_cache/tipe/engine-trace.log"
doctor_invalid_status_warn_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_cache" \
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_invalid_status_warn_output" != *$'warn\tengine-trace\tstatus popup used invalid cursor rect'* ||
    "$doctor_invalid_status_warn_output" != *$'info\tengine-trace\tstatus-window-edge-fallback rect=0,0,0,0 status=TiPE'* ]]; then
    echo "doctor helper should warn when old status fallback used an invalid cursor rectangle" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-config.sh" --write heuristic \
    --continuous on >/dev/null
doctor_continuous_output=$(XDG_DATA_HOME="$doctor_default_data" XDG_CACHE_HOME="$doctor_default_cache" \
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/doctor.sh" --no-runtime)
if [[ "$doctor_continuous_output" != *$'model\tcontinuous-default\t1'* ]]; then
    echo "doctor helper should report configured continuous-mode default" >&2
    exit 1
fi
restart_model_current_dry_run=$(
    TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/restart-fcitx5.sh" --model-current --dry-run
)
if [[ "$restart_model_current_dry_run" != *'TIPE_CONTINUOUS_MODE=1'* ]]; then
    echo "restart helper dry-run should pass configured continuous-mode default to fcitx5" >&2
    exit 1
fi
TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-config.sh" --write dump --dump-path "$tmp_dir/current-request.tsv" >/dev/null
model_current_dump_output=$(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
    TIPE_MODEL_DUMP_RESPONSE=$'candidate\t你号\n' TIPE_MODEL_CONFIG="$model_current_config" "$ROOT/scripts/model-current.sh")
if [[ "$model_current_dump_output" != $'candidate\t你号' ]] ||
    ! cmp -s "$tmp_dir/current-request.tsv" <(printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n'); then
    echo "model current helper should route dump mode through tipe-model-dump" >&2
    exit 1
fi
probe_model_dump_path="$tmp_dir/probe-model-request.tsv"
probe_model_dump_output=$(TIPE_MODEL_COMMAND="$ROOT/scripts/model-dump.sh" \
    TIPE_MODEL_DUMP_PATH="$probe_model_dump_path" \
    TIPE_MODEL_DUMP_RESPONSE=$'candidate\t世界\n' \
    "$ROOT/build/tipe-state-probe" "" --application Alacritty \
        --surrounding-before $'编辑器\t左侧' --surrounding-after $'右侧\n内容' \
        --script <(printf '%s\n' 'type nihao' 'select 你好' 'type shijie' 'rerank'))
if [[ "$probe_model_dump_output" != *$'action\trerank\t1\tupdate'* ||
    "$probe_model_dump_output" != *$'candidate\t0\t世界'* ||
    "$(cat "$probe_model_dump_path")" != *$'context\t你好'* ||
    "$(cat "$probe_model_dump_path")" != *$'application\tAlacritty'* ||
    "$(cat "$probe_model_dump_path")" != *$'surrounding_before\t编辑器\\t左侧'* ||
    "$(cat "$probe_model_dump_path")" != *$'surrounding_after\t右侧\\n内容'* ||
    "$(cat "$probe_model_dump_path")" != *$'preedit\tshijie'* ]]; then
    echo "model dump helper should capture probe-triggered model requests with app and text context" >&2
    exit 1
fi
segment_chain_dump_path="$tmp_dir/segment-chain-model-request.tsv"
segment_chain_dump_output=$(TIPE_MODEL_COMMAND="$ROOT/scripts/model-dump.sh" \
    TIPE_MODEL_DUMP_PATH="$segment_chain_dump_path" \
    "$ROOT/build/tipe-state-probe" "" \
        --script <(printf '%s\n' 'type woc' 'select 我' 'expect-candidate 0 操' 'select 操' \
            'type jixuzuo' 'select 继续' 'select 做' 'type jixuzuo' 'rerank'))
segment_chain_dump=$(cat "$segment_chain_dump_path")
if [[ "$segment_chain_dump_output" != *$'action\trerank\t1\tupdate'* ||
    "$segment_chain_dump" != *$'segment_chain\twoc\two\t我\tc\twocao\t我操'* ||
    "$segment_chain_dump" != *$'segment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做'* ||
    "$segment_chain_dump" != *$'preference\tjixuzuo\t继续做\t'* ||
    "$segment_chain_dump" != *$'correction\twoc\twocao\t'* ]]; then
    echo "model dump helper should capture general segment-selection chains for later model reranks" >&2
    exit 1
fi
adapter_segment_chain_output=$(
    printf 'protocol\t1\npreedit\twoc\ncandidates\t我操\t我曹\t我\nsegment_chain\twoc\two\t我\tc\twocao\t我操\nsegment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_segment_chain_output" != *$'candidate\t我操'* ||
    "$adapter_segment_chain_output" == *$'segment_chain\twoc\two\t我\tc\twocao\t我操\t1'* ||
    "$adapter_segment_chain_output" != *$'correction\twoc\twocao'* ||
    "$adapter_segment_chain_output" == *$'candidate\t继续做'* ]]; then
    echo "model adapter heuristic should use matching segment chains without leaking unrelated chains" >&2
    exit 1
fi
adapter_segment_chain_suffix_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t从\t操\t曹\ncontext\t我\nsegment_chain\twoc\two\t我\tc\twocao\t我操\nsegment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_segment_chain_suffix_output" != *$'candidate\t操'* ||
    "$adapter_segment_chain_suffix_output" != *$'correction\twoc\twocao'* ||
    "$adapter_segment_chain_suffix_output" == *$'candidate\t继续做'* ||
    "$adapter_segment_chain_suffix_output" == *$'segment_chain\twoc'* ]]; then
    echo "model adapter heuristic should use segment-chain suffix evidence after a recent prefix commit" >&2
    exit 1
fi
adapter_pending_segment_confirm_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t0\t操\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ -n "$adapter_pending_segment_confirm_output" ]]; then
    echo "model adapter heuristic should not confirm pending prefix selections from the passive top suffix" >&2
    exit 1
fi
adapter_pending_segment_explicit_confirm_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\t曹\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t1\t从\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_pending_segment_explicit_confirm_output" != *$'segment_chain\twoc\two\t我\tc\twoc\t我从\t1'* ]]; then
    echo "model adapter heuristic should confirm pending prefix selections when a non-top suffix candidate is selected" >&2
    exit 1
fi
adapter_segment_chain_dry_run_output=$(
    printf 'protocol\t1\npreedit\twoc\ncandidates\t我操\t我曹\nsegment_chain\twoc\two\t我\tc\twocao\t我操\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_segment_chain_dry_run_json="${adapter_segment_chain_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_segment_chain_dry_run_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["behavior_summary"]["learning_signals"] != [
    {
        "kind": "segment_chain",
        "status": "correction_chain",
        "original_preedit": "woc",
        "consumed_preedit": "wo",
        "committed_text": "我",
        "remaining_preedit": "c",
        "corrected_full_preedit": "wocao",
        "combined_candidate": "我操",
        "evidence_protocol": "segment_chain\twoc\two\t我\tc\twocao\t我操\t1",
        "suggested_correction_protocol": "correction\twoc\twocao",
    }
]:
    sys.exit(1)
PY
adapter_bad_segment_chain_dry_run_output=$(
    printf 'protocol\t1\npreedit\twoc\ncandidates\t我操\t我曹\nsegment_chain\twoc\two\t我\tx\twocao\t我操\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_bad_segment_chain_dry_run_json="${adapter_bad_segment_chain_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_bad_segment_chain_dry_run_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["recent_segment_chains"] or prompt["behavior_summary"]["learning_signals"]:
    sys.exit(1)
PY
adapter_selected_top_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nselected_candidate\t0\t你好\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ -n "$adapter_selected_top_output" ]]; then
    echo "model adapter heuristic should not learn the default top selected candidate" >&2
    exit 1
fi
adapter_selected_non_top_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\nselected_candidate\t1\t你号\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_selected_non_top_output" != $'candidate\t你号\npreference\tnihao\t你号\t2' ]]; then
    echo "model adapter heuristic should immediately rank and learn an explicitly selected non-top candidate" >&2
    exit 1
fi
adapter_history_pair_cache="$tmp_dir/adapter-history-pair-cache"
mkdir -p "$adapter_history_pair_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcandidate-selected:你号'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcandidate-selected:你号'
    printf '%s\n' $'---\tunix_ms\t3\tprogram\tTest\tpreedit\tstart\tcandidates\t1\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tstart'
    printf '%s\n' $'candidates\t开始'
    printf '%s\n' $'selected_candidate\t1\tstart'
    printf '%s\n' $'events\traw-committed:start'
    printf '%s\n' $'---\tunix_ms\t4\tprogram\tTest\tpreedit\tstart\tcandidates\t1\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tstart'
    printf '%s\n' $'candidates\t开始'
    printf '%s\n' $'selected_candidate\t1\tstart'
    printf '%s\n' $'events\traw-committed:start'
    printf '%s\n' $'---\tunix_ms\t5\tprogram\tTest\tpreedit\tnihao\tcandidates\t3\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号\t错'
    printf '%s\n' $'selected_candidate\t1\t错'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t1\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t6\tprogram\tTest\tpreedit\tnihao\tcandidates\t3\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号\t错'
    printf '%s\n' $'selected_candidate\t1\t错'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t1\tcorrection_events\t0'
} >"$adapter_history_pair_cache/tipe/supervision-history.tsv"
adapter_history_pair_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\t错\n' |
        XDG_CACHE_HOME="$adapter_history_pair_cache" TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_history_pair_output" != $'candidate\t你号\npreference\tnihao\t你号\t2' ]]; then
    echo "model adapter heuristic should rank repeated active preedit-selected history pairs" >&2
    exit 1
fi
adapter_history_pair_dry_run_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\t错\n' |
        XDG_CACHE_HOME="$adapter_history_pair_cache" TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 \
        "$ROOT/scripts/model-adapter.sh"
)
adapter_history_pair_json="${adapter_history_pair_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_history_pair_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
expected_signal = {
    "kind": "history_preference",
    "status": "repeated_supervised_history",
    "preedit": "nihao",
    "candidate": "你号",
    "count": 2,
    "suggested_protocol": "preference\tnihao\t你号\t2",
}
if expected_signal not in prompt["behavior_summary"]["supervised_learning_signals"]:
    sys.exit(1)
if "preference\tnihao\t你号\t2" not in prompt["learning_status"]["suggested_protocols"]:
    sys.exit(2)
PY
adapter_history_prefix_only_cache="$tmp_dir/adapter-history-prefix-only-cache"
mkdir -p "$adapter_history_prefix_only_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tjixuzuo\tcandidates\t2\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tjixuzuo'
    printf '%s\n' $'candidates\t继续做\t继续'
    printf '%s\n' $'candidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000'
    printf '%s\n' $'candidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999'
    printf '%s\n' $'selected_candidate\t1\t继续'
    printf '%s\n' $'events\tcandidate-selected:继续'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t1\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tjixuzuo\tcandidates\t2\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tjixuzuo'
    printf '%s\n' $'candidates\t继续做\t继续'
    printf '%s\n' $'candidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000'
    printf '%s\n' $'candidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999'
    printf '%s\n' $'selected_candidate\t1\t继续'
    printf '%s\n' $'events\tcandidate-selected:继续'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t1\tcorrection_events\t0'
} >"$adapter_history_prefix_only_cache/tipe/supervision-history.tsv"
adapter_history_prefix_only_output=$(
    printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\n' |
        XDG_CACHE_HOME="$adapter_history_prefix_only_cache" TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ -n "$adapter_history_prefix_only_output" ]]; then
    echo "model adapter heuristic should not learn repeated prefix-only history as a full preedit preference" >&2
    exit 1
fi
adapter_history_prefix_only_dry_run_output=$(
    printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\n' |
        XDG_CACHE_HOME="$adapter_history_prefix_only_cache" TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 \
        "$ROOT/scripts/model-adapter.sh"
)
adapter_history_prefix_only_json="${adapter_history_prefix_only_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_history_prefix_only_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
bad_pair = {"preedit": "jixuzuo", "candidate": "继续", "count": 2}
if bad_pair in prompt["recent_history_summary"]["top_preedit_selected_pairs"]:
    sys.exit(1)
for signal in prompt["behavior_summary"]["supervised_learning_signals"]:
    if signal.get("kind") == "history_preference" and signal.get("preedit") == "jixuzuo" and signal.get("candidate") == "继续":
        sys.exit(2)
for protocol in prompt["learning_status"]["suggested_protocols"]:
    if protocol == "preference\tjixuzuo\t继续\t2":
        sys.exit(3)
PY
adapter_history_default_selected_cache="$tmp_dir/adapter-history-default-selected-cache"
mkdir -p "$adapter_history_default_selected_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t0\t你好'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t0\t你好'
} >"$adapter_history_default_selected_cache/tipe/supervision-history.tsv"
adapter_history_default_selected_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你号\t你好\n' |
        XDG_CACHE_HOME="$adapter_history_default_selected_cache" TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ -n "$adapter_history_default_selected_output" ]]; then
    echo "model adapter heuristic should not learn passive default selected_candidate history rows" >&2
    exit 1
fi
adapter_history_raw_output=$(
    printf 'protocol\t1\npreedit\tstart\ncandidates\t开始\n' |
        XDG_CACHE_HOME="$adapter_history_pair_cache" TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_history_raw_output" != $'candidate\tstart\npreference\tstart\tstart\t2' ]]; then
    echo "model adapter heuristic should learn repeated raw-English history pairs without hardcoded words" >&2
    exit 1
fi
adapter_history_correction_cache="$tmp_dir/adapter-history-correction-cache"
mkdir -p "$adapter_history_correction_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0\tterminal\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你好'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tTest\tpreedit\tnihao\tcandidates\t2\texpanded\t0\tterminal\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\tcandidate-selected:你好'
    printf '%s\n' $'---\tunix_ms\t3\tprogram\tTest\tpreedit\twocao\tcandidates\t1\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\twocao'
    printf '%s\n' $'candidates\t我操'
    printf '%s\n' $'correction_events\tletter:w\tletter:o\tletter:c\tletter:a\tletter:o'
} >"$adapter_history_correction_cache/tipe/supervision-history.tsv"
adapter_history_correction_output=$(
    printf 'protocol\t1\npreedit\tihao\ncandidates\t以后\t一号\n' |
        XDG_CACHE_HOME="$adapter_history_correction_cache" TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_history_correction_output" != $'correction\tihao\tnihao' ]]; then
    echo "model adapter heuristic should infer repeated typo corrections from supervision history" >&2
    exit 1
fi
adapter_history_correction_dry_run_output=$(
    printf 'protocol\t1\npreedit\tihao\ncandidates\t以后\t一号\n' |
        XDG_CACHE_HOME="$adapter_history_correction_cache" TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 \
        "$ROOT/scripts/model-adapter.sh"
)
adapter_history_correction_json="${adapter_history_correction_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_history_correction_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
expected_row = {"typo": "ihao", "corrected_preedit": "nihao", "count": 2}
if expected_row not in prompt["recent_history_summary"]["top_corrections"]:
    sys.exit(1)
expected_signal = {
    "kind": "history_correction",
    "status": "current_typo",
    "typo": "ihao",
    "corrected_preedit": "nihao",
    "count": 2,
    "suggested_protocol": "correction\tihao\tnihao",
}
if expected_signal not in prompt["behavior_summary"]["supervised_learning_signals"]:
    sys.exit(2)
PY
adapter_history_corrected_correction_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        XDG_CACHE_HOME="$adapter_history_correction_cache" TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_history_corrected_correction_output" != $'correction\tihao\tnihao' ]]; then
    echo "model adapter heuristic should use repeated correction history when the corrected preedit is current" >&2
    exit 1
fi
adapter_history_corrected_correction_dry_run_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        XDG_CACHE_HOME="$adapter_history_correction_cache" TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 \
        "$ROOT/scripts/model-adapter.sh"
)
adapter_history_corrected_correction_json="${adapter_history_corrected_correction_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_history_corrected_correction_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
expected_signal = {
    "kind": "history_correction",
    "status": "current_corrected_preedit",
    "typo": "ihao",
    "corrected_preedit": "nihao",
    "count": 2,
    "suggested_protocol": "correction\tihao\tnihao",
}
if expected_signal not in prompt["behavior_summary"]["supervised_learning_signals"]:
    sys.exit(1)
if "correction\tihao\tnihao" not in prompt["learning_status"]["suggested_protocols"]:
    sys.exit(2)
PY
adapter_cloud_private_output=$(
    printf 'protocol\t1\npreedit\tnihao\napplication\tBrowser\nsurrounding_before\tprivate-before\nsurrounding_after\tprivate-after\ncandidates\t你好\t你号\nevents\tletter:n\tbackspace:\ncorrection_events\tletter:i\tbackspace:\ncontext\tprivate-context\npreference\tnihao\t你号\t3\n' |
        TIPE_MODEL_MODE=openai-compatible TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 \
            "$ROOT/scripts/model-adapter.sh"
)
adapter_cloud_private_json="${adapter_cloud_private_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_cloud_private_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["data_sharing"] != {"recent_input": False, "surrounding_text_and_application": False}:
    sys.exit(1)
if prompt["application"] or any(prompt["surrounding_context"].values()):
    sys.exit(2)
if prompt["recent_events"] or prompt["correction_events"] or prompt["recent_context"]:
    sys.exit(3)
if prompt["known_preferences"] or prompt["recent_history_summary"].get("available"):
    sys.exit(4)
PY
adapter_cloud_shared_output=$(
    printf 'protocol\t1\npreedit\tnihao\napplication\tBrowser\nsurrounding_before\tshared-before\nsurrounding_after\tshared-after\ncandidates\t你好\t你号\nevents\tletter:n\tbackspace:\ncorrection_events\tletter:i\tbackspace:\ncontext\tshared-context\npreference\tnihao\t你号\t3\n' |
        TIPE_MODEL_MODE=openai-compatible TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 \
            TIPE_MODEL_SEND_RECENT_INPUT=1 TIPE_MODEL_SEND_SURROUNDING=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_cloud_shared_json="${adapter_cloud_shared_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_cloud_shared_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["data_sharing"] != {"recent_input": True, "surrounding_text_and_application": True}:
    sys.exit(1)
if prompt["application"] != "Browser" or prompt["surrounding_context"]["before_cursor"] != "shared-before":
    sys.exit(2)
if prompt["recent_events"] != ["letter:n", "backspace:"] or prompt["recent_context"] != ["shared-context"]:
    sys.exit(3)
if prompt["known_preferences"] != ["nihao\t你号\t3"]:
    sys.exit(4)
PY
adapter_dry_run_history_cache="$tmp_dir/adapter-dry-run-history-cache"
mkdir -p "$adapter_dry_run_history_cache/tipe"
{
    printf '%s\n' $'---\tunix_ms\t1\tprogram\tTest\tpreedit\told\tcandidates\t1\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\told'
    printf '%s\n' $'candidates\t旧'
    printf '%s\n' $'event_counts\tobserved:1'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t1\tcorrection_events\t0'
    printf '%s\n' $'---\tunix_ms\t2\tprogram\tAlacritty\tpreedit\tnihao\tcandidates\t2\texpanded\t1'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\tnihao'
    printf '%s\n' $'application\tAlacritty'
    printf '%s\n' $'candidates\t你好\t你号'
    printf '%s\n' $'selected_candidate\t1\t你号'
    printf '%s\n' $'events\tcandidate-selected:你号'
    printf '%s\n' $'event_counts\tletter:1\tcursor-move:1'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o'
    printf '%s\n' $'correction_event_counts\tletter:9\tbackspace:4'
    printf '%s\n' $'supervision_state\tmode\tactive-preedit\tactive_preedit\t1\trecent_events\t2\tcorrection_events\t13'
    printf '%s\n' $'segment_chain\tnihao\tni\t你\thao\tnihao\t你好'
    printf '%s\n' $'---\tunix_ms\t3\tprogram\tAlacritty\tpreedit\t\tcandidates\t0\texpanded\t0'
    printf '%s\n' $'protocol\t1'
    printf '%s\n' $'preedit\t'
    printf '%s\n' $'candidates'
    printf '%s\n' $'event_counts\tspace:2\tdelete:1\tcursor-move:1'
    printf '%s\n' $'correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o'
    printf '%s\n' $'correction_event_counts\tletter:9\tbackspace:4'
    printf '%s\n' $'supervision_state\tmode\tpass-through-only\tactive_preedit\t0\trecent_events\t4\tcorrection_events\t0'
} >"$adapter_dry_run_history_cache/tipe/supervision-history.tsv"
adapter_dry_run_output=$(
    printf 'protocol\t1\npreedit\tnihao\napplication\tAlacritty\nsurrounding_before\t刚才\\tPath\\\\Name\nsurrounding_after\t后面\\nText\ncandidates\t你好\t你号\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t0\tsource\tfull\tscore\t999999\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nruntime_state\tcontinuous\t1\nselected_candidate\t1\t你号\nvisible_candidates\t0:你好\t1:你号\nnumbered_candidates\t1:1:你号\nevents\tspace:\tdelete:\tletter:n\tobserved:Tab\\tLine\tcursor-move:Down\nevent_counts\tspace:1\tdelete:1\tletter:1\tobserved:1\tcursor-move:1\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\ncorrection_event_counts\tletter:9\tbackspace:4\tdelete:1\ncontext\t刚才\tPath\\\\Name\nsegment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做\npreference\tnihao\t你号\t3\ncorrection\tihao\tnihao\t2\n' |
        XDG_CACHE_HOME="$adapter_dry_run_history_cache" TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 \
            TIPE_MODEL_BASE_URL=http://dry-run.local/v1/ TIPE_MODEL_CHAT_PATH=custom/chat \
            TIPE_MODEL_NAME=dry-run-model "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_dry_run_output" != request$'\t''http://dry-run.local/v1/custom/chat'$'\n'request-json$'\t'* ]]; then
    echo "model adapter dry-run returned unexpected header: $adapter_dry_run_output" >&2
    exit 1
fi
adapter_dry_run_json="${adapter_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_dry_run_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
if request["model"] != "dry-run-model":
    sys.exit(1)
message = request["messages"][1]["content"]
prompt = json.loads(message)
if prompt["preedit"] != "nihao":
    sys.exit(2)
if prompt["supervision_mode"] != "active-preedit":
    sys.exit(43)
if prompt["application"] != "Alacritty":
    sys.exit(7)
if prompt["surrounding_context"] != {"before_cursor": "刚才\tPath\\Name", "after_cursor": "后面\nText"}:
    sys.exit(24)
if prompt["candidates"] != ["你好", "你号"]:
    sys.exit(3)
if prompt["candidate_metadata"] != [
    {"index": 0, "text": "你好", "consumed_prefix": 0, "source": "full", "score": 1000000},
    {"index": 1, "text": "你号", "consumed_prefix": 0, "source": "full", "score": 999999},
]:
    sys.exit(48)
if prompt["input_state"] != {"preedit_cursor": 5, "candidate_cursor": 1, "expanded": True}:
    sys.exit(9)
if prompt["runtime_state"] != {"continuous": True}:
    sys.exit(10)
if prompt["selected_candidate"] != {"index": 1, "text": "你号"}:
    sys.exit(18)
if prompt["visible_candidates"] != [{"index": 0, "text": "你好"}, {"index": 1, "text": "你号"}]:
    sys.exit(19)
if prompt["numbered_candidates"] != [{"shortcut": "1", "index": 1, "text": "你号"}]:
    sys.exit(20)
if prompt["behavior_summary"]["recent_event_counts"] != {"space": 1, "delete": 1, "letter": 1, "observed": 1, "cursor-move": 1}:
    sys.exit(21)
if prompt["behavior_summary"]["preedit_leading_context"] != {
    "active": True,
    "events": ["space:", "delete:"],
    "event_counts": {"space": 1, "delete": 1},
    "events_before_preedit": 2,
    "meaning": "supervised pass-through keys observed immediately before the current preedit",
}:
    sys.exit(46)
if prompt["behavior_summary"]["correction_event_counts"] != {"letter": 9, "backspace": 4, "delete": 1}:
    sys.exit(22)
if prompt["behavior_summary"]["possible_corrections"] != [
    {"source": "full-delete-retype", "typo": "ihao", "corrected_preedit": "nihao"}
]:
    sys.exit(23)
if prompt["behavior_summary"]["edit_summary"] != {
    "current": "nihao",
    "cursor": 5,
    "typed_tail": "nihao",
    "last_fully_erased": "ihao",
    "last_edited_original": "ihao",
    "middle_edit_original": "",
}:
    sys.exit(35)
if prompt["behavior_summary"]["correction_patterns"] != [
    {"kind": "missing", "text": "n", "position": 0, "relative_to_end": False, "count": 2}
]:
    sys.exit(36)
if prompt["behavior_summary"]["realtime_correction_decisions"] != [
    {
        "kind": "missing",
        "text": "n",
        "position": 0,
        "relative_to_end": False,
        "count": 2,
        "status": "skipped",
        "reason": "already-present",
        "corrected_preedit": "",
    }
]:
    sys.exit(37)
if prompt["behavior_summary"]["raw_english_hint"] != {"active": False, "source": "none", "count": 0}:
    sys.exit(33)
if prompt["recent_history_summary"]["available"] is not True:
    sys.exit(49)
if prompt["recent_history_summary"]["records"] != 3:
    sys.exit(50)
if prompt["recent_history_summary"]["active_preedit_records"] != 2:
    sys.exit(51)
if prompt["recent_history_summary"]["top_preedits"] != [
    {"text": "nihao", "count": 1},
    {"text": "old", "count": 1},
]:
    sys.exit(52)
if prompt["recent_history_summary"]["top_selected_candidates"] != [{"text": "你号", "count": 1}]:
    sys.exit(53)
if prompt["recent_history_summary"]["top_preedit_selected_pairs"] != [
    {"preedit": "nihao", "candidate": "你号", "count": 1}
]:
    sys.exit(57)
if prompt["recent_history_summary"]["pass_through_records"] != 1:
    sys.exit(58)
if prompt["recent_history_summary"]["event_counts"] != {"observed": 1, "letter": 1, "cursor-move": 2, "space": 2, "delete": 1}:
    sys.exit(54)
expected_active_event_counts = {"observed": 1, "letter": 1, "cursor-move": 1}
if prompt["recent_history_summary"]["active_event_counts"] != expected_active_event_counts:
    print(
        "unexpected active history event counts:",
        prompt["recent_history_summary"]["active_event_counts"],
        "expected:",
        expected_active_event_counts,
        file=sys.stderr,
    )
    sys.exit(59)
if prompt["recent_history_summary"]["pass_through_event_counts"] != {"space": 2, "delete": 1, "cursor-move": 1}:
    sys.exit(60)
if prompt["recent_history_summary"]["correction_event_counts"] != {"letter": 9, "backspace": 4}:
    sys.exit(55)
if prompt["recent_history_summary"]["segment_chain_count"] != 1:
    sys.exit(56)
if prompt["behavior_summary"]["learning_signals"] != [
    {
        "kind": "selected_candidate",
        "status": "would_learn_preference",
        "preedit": "nihao",
        "candidate": "你号",
        "index": 1,
        "suggested_protocol": "preference\tnihao\t你号\t2",
    }
]:
    sys.exit(38)
if prompt["behavior_summary"]["supervised_learning_signals"] != [
    {
        "kind": "selected_candidate",
        "status": "would_learn_preference",
        "preedit": "nihao",
        "candidate": "你号",
        "index": 1,
        "suggested_protocol": "preference\tnihao\t你号\t2",
    },
    {
        "kind": "possible_correction",
        "status": "supervised_evidence",
        "source": "full-delete-retype",
        "typo": "ihao",
        "corrected_preedit": "nihao",
        "suggested_protocol": "correction\tihao\tnihao",
    },
    {
        "kind": "segment_chain",
        "status": "recent_supervised",
        "original_preedit": "jixuzuo",
        "consumed_preedit": "jixu",
        "committed_text": "继续",
        "remaining_preedit": "zuo",
        "corrected_full_preedit": "jixuzuo",
        "combined_candidate": "继续做",
        "evidence_protocol": "segment_chain\tjixuzuo\tjixu\t继续\tzuo\tjixuzuo\t继续做\t1",
    },
    {
        "kind": "known_correction",
        "status": "current_corrected_preedit",
        "typo": "ihao",
        "corrected_preedit": "nihao",
        "count": 2,
        "evidence_protocol": "correction\tihao\tnihao",
    },
]:
    sys.exit(47)
if prompt["learning_status"] != {
    "mode": "active-preedit",
    "primary": "ready-to-learn",
    "preedit": "nihao",
    "candidate_count": 2,
    "selected_candidate": {"index": 1, "text": "你号"},
    "signal_counts": {
        "selected_candidate": 1,
        "possible_correction": 1,
        "segment_chain": 1,
        "known_correction": 1,
    },
    "status_counts": {
        "would_learn_preference": 1,
        "supervised_evidence": 1,
        "recent_supervised": 1,
        "current_corrected_preedit": 1,
    },
    "suggested_protocols": [
        "preference\tnihao\t你号\t2",
        "correction\tihao\tnihao",
    ],
    "awaiting_suffix": [],
    "history_available": True,
    "history_records": 3,
    "next_step": "prefer suggested_protocol rows that match the current active preedit and visible UI state",
}:
    sys.exit(58)
if prompt["learning_objectives"] != [
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
]:
    sys.exit(45)
if prompt["recent_events"] != ["space:", "delete:", "letter:n", "observed:Tab\tLine", "cursor-move:Down"]:
    sys.exit(4)
if prompt["recent_context"] != ["刚才", "Path\\Name"]:
    sys.exit(5)
if prompt["recent_segment_chains"] != [{
    "original_preedit": "jixuzuo",
    "consumed_preedit": "jixu",
    "committed_text": "继续",
    "remaining_preedit": "zuo",
    "corrected_full_preedit": "jixuzuo",
    "combined_candidate": "继续做",
}]:
    sys.exit(34)
if prompt["pending_segments"] != []:
    sys.exit(35)
rules = "\n".join(prompt["rules"])
if "preference<TAB>PREEDIT<TAB>CANDIDATE<TAB>COUNT" not in rules:
    sys.exit(38)
if "segment_chain<TAB>ORIGINAL<TAB>CONSUMED<TAB>COMMITTED<TAB>REMAINING<TAB>CORRECTED_FULL<TAB>COMBINED<TAB>COUNT" not in rules:
    sys.exit(39)
if "selected_candidate" not in rules:
    sys.exit(40)
if "Do not infer raw English preference from application name or code-looking context alone" not in rules:
    sys.exit(41)
if "do not hardcode example words" not in rules:
    sys.exit(42)
if "0 < consumed_prefix < preedit length" not in rules:
    sys.exit(49)
if "pending_segments as unfinished prefix selections" not in rules:
    sys.exit(51)
if "Do not output preference rows for prefix-only candidates" not in rules:
    sys.exit(50)
if "Do not echo rows that already appear in known_preferences, known_corrections, or recent_segment_chains" not in rules:
    sys.exit(57)
if "pass-through-only" not in rules:
    sys.exit(44)
if prompt["known_preferences"] != ["nihao\t你号\t3"]:
    sys.exit(8)
if prompt["known_corrections"] != ["ihao\tnihao\t2"]:
    sys.exit(6)
PY
adapter_prefix_signal_dry_run_output=$(
    printf 'protocol\t1\npreedit\tjixuzuo\ncandidates\t继续做\t继续\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t4\tsource\tprefix\tscore\t999999\nselected_candidate\t1\t继续\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_prefix_signal_json="${adapter_prefix_signal_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_prefix_signal_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
expected = [{
    "kind": "selected_candidate",
    "status": "prefix_only_no_preference",
    "preedit": "jixuzuo",
    "candidate": "继续",
    "index": 1,
    "meaning": "selected candidate commits only a prefix; learn a segment_chain after the suffix is confirmed, not a full-preedit preference",
}]
if prompt["behavior_summary"]["learning_signals"] != expected:
    sys.exit(1)
if prompt["behavior_summary"]["supervised_learning_signals"] != expected:
    sys.exit(2)
PY
adapter_short_partial_signal_dry_run_output=$(
    printf 'protocol\t1\npreedit\tshdfjshdkjfa\ncandidates\t是的福建省雕刻技法\t水电费\nselected_candidate\t1\t水电费\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_short_partial_signal_json="${adapter_short_partial_signal_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_short_partial_signal_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
expected = [{
    "kind": "selected_candidate",
    "status": "prefix_only_no_preference",
    "preedit": "shdfjshdkjfa",
    "candidate": "水电费",
    "index": 1,
    "meaning": "selected candidate commits only a prefix; learn a segment_chain after the suffix is confirmed, not a full-preedit preference",
}]
if prompt["behavior_summary"]["learning_signals"] != expected:
    sys.exit(1)
if prompt["behavior_summary"]["supervised_learning_signals"] != expected:
    sys.exit(2)
PY
adapter_pending_segment_dry_run_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\ncontext\t我\npending_segment\twoc\two\t我\tc\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_pending_segment_json="${adapter_pending_segment_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_pending_segment_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
expected_segment = {
    "original_preedit": "woc",
    "consumed_preedit": "wo",
    "committed_text": "我",
    "remaining_preedit": "c",
}
if prompt["pending_segments"] != [expected_segment]:
    sys.exit(1)
expected_signal = {
    "kind": "pending_segment",
    "status": "awaiting_suffix_confirmation",
    "original_preedit": "woc",
    "consumed_preedit": "wo",
    "committed_text": "我",
    "remaining_preedit": "c",
    "meaning": "prefix was selected from a longer preedit; wait for the suffix candidate before outputting a segment_chain row",
}
if expected_signal not in prompt["behavior_summary"]["supervised_learning_signals"]:
    sys.exit(2)
if prompt["learning_status"]["primary"] != "awaiting-suffix-confirmation":
    sys.exit(3)
if prompt["learning_status"]["awaiting_suffix"] != [{
    "original_preedit": "woc",
    "consumed_preedit": "wo",
    "committed_text": "我",
    "remaining_preedit": "c",
}]:
    sys.exit(4)
PY
adapter_pending_confirm_dry_run_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t0\t操\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_pending_confirm_json="${adapter_pending_confirm_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_pending_confirm_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
expected_signal = {
    "kind": "pending_segment",
    "status": "awaiting_suffix_confirmation",
    "original_preedit": "woc",
    "consumed_preedit": "wo",
    "committed_text": "我",
    "remaining_preedit": "c",
    "meaning": "prefix was selected from a longer preedit; wait for the suffix candidate before outputting a segment_chain row",
}
if expected_signal not in prompt["behavior_summary"]["supervised_learning_signals"]:
    sys.exit(1)
if prompt["learning_status"]["primary"] != "awaiting-suffix-confirmation":
    sys.exit(2)
if prompt["learning_status"]["suggested_protocols"] != []:
    sys.exit(3)
PY
adapter_pending_explicit_confirm_dry_run_output=$(
    printf 'protocol\t1\npreedit\tc\ncandidates\t操\t从\ncontext\t我\npending_segment\twoc\two\t我\tc\nselected_candidate\t1\t从\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_pending_explicit_confirm_json="${adapter_pending_explicit_confirm_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_pending_explicit_confirm_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
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
if expected_signal not in prompt["behavior_summary"]["supervised_learning_signals"]:
    sys.exit(1)
if prompt["learning_status"]["primary"] != "ready-to-learn":
    sys.exit(2)
if prompt["learning_status"]["suggested_protocols"] != [
    "preference\tc\t从\t2",
    "segment_chain\twoc\two\t我\tc\twoc\t我从\t1",
]:
    sys.exit(3)
PY
adapter_pass_through_cache="$tmp_dir/adapter-pass-through-cache"
mkdir -p "$adapter_pass_through_cache/tipe"
adapter_pass_through_dry_run_output=$(
    printf 'protocol\t1\npreedit\t\ncandidates\nstate\tpreedit_cursor\t0\tcandidate_cursor\t0\texpanded\t0\nruntime_state\tcontinuous\t0\nevents\tspace:\tdelete:\tcursor-move:Down\tobserved:Tab\nevent_counts\tspace:1\tdelete:1\tcursor-move:1\tobserved:1\ncorrection_events\tspace:\tdelete:\tcursor-move:Down\tobserved:Tab\ncorrection_event_counts\tspace:1\tdelete:1\tcursor-move:1\tobserved:1\nsegment_chain\twoc\two\t我\tc\twocao\t我操\n' |
        XDG_CACHE_HOME="$adapter_pass_through_cache" TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_pass_through_json="${adapter_pass_through_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_pass_through_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["preedit"] != "":
    sys.exit(1)
if prompt["supervision_mode"] != "pass-through-only":
    sys.exit(2)
if prompt["candidates"] != []:
    sys.exit(3)
if prompt["recent_events"] != ["space:", "delete:", "cursor-move:Down", "observed:Tab"]:
    sys.exit(4)
if prompt["behavior_summary"]["recent_event_counts"] != {
    "space": 1,
    "delete": 1,
    "cursor-move": 1,
    "observed": 1,
}:
    sys.exit(5)
if prompt["behavior_summary"]["preedit_leading_context"] != {
    "active": True,
    "events": ["space:", "delete:", "cursor-move:Down", "observed:Tab"],
    "event_counts": {"space": 1, "delete": 1, "cursor-move": 1, "observed": 1},
    "events_before_preedit": 4,
    "meaning": "all supervised pass-through keys because no preedit is active",
}:
    sys.exit(7)
if prompt["recent_segment_chains"] or prompt["known_preferences"] or prompt["known_corrections"]:
    sys.exit(8)
if prompt["behavior_summary"]["learning_signals"] or prompt["behavior_summary"]["supervised_learning_signals"]:
    sys.exit(8)
if prompt["learning_status"] != {
    "mode": "pass-through-only",
    "primary": "keyboard-context-only",
    "preedit": "",
    "candidate_count": 0,
    "selected_candidate": None,
    "signal_counts": {},
    "status_counts": {},
    "suggested_protocols": [],
    "awaiting_suffix": [],
    "history_available": False,
    "history_records": 0,
    "next_step": "wait for an active preedit before emitting candidates or learning rows",
}:
    sys.exit(10)
rules = "\n".join(prompt["rules"])
if "read-only keyboard behavior context" not in rules:
    sys.exit(6)
if "supervised_learning_signals" not in rules:
    sys.exit(9)
PY
adapter_pass_through_heuristic_output=$(
    printf 'protocol\t1\npreedit\t\ncandidates\nsegment_chain\twoc\two\t我\tc\twocao\t我操\npreference\tnihao\t你好\t8\ncorrection\tihao\tnihao\t2\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ -n "$adapter_pass_through_heuristic_output" ]]; then
    echo "model adapter heuristic should not emit candidates or learning rows for empty-preedit pass-through supervision" >&2
    exit 1
fi
adapter_raw_hint_dry_run_output=$(
    printf 'protocol\t1\npreedit\tstarted\nsurrounding_before\tconst taskStatus = \ncandidates\t三他人特点\t三他人特定\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_raw_hint_json="${adapter_raw_hint_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_raw_hint_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["behavior_summary"]["raw_english_hint"] != {"active": False, "source": "none", "count": 0}:
    sys.exit(1)
PY
adapter_pinyin_raw_hint_dry_run_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\tnihao\npreference\tnihao\tnihao\t9\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_pinyin_raw_hint_json="${adapter_pinyin_raw_hint_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_pinyin_raw_hint_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["known_preferences"]:
    sys.exit(1)
if prompt["behavior_summary"]["raw_english_hint"] != {"active": False, "source": "none", "count": 0}:
    sys.exit(2)
PY
adapter_learned_raw_hint_dry_run_output=$(
    printf 'protocol\t1\npreedit\tstarted\ncandidates\t三他人特定\tstarted\npreference\tstarted\tstarted\t3\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_learned_raw_hint_json="${adapter_learned_raw_hint_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_learned_raw_hint_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["behavior_summary"]["raw_english_hint"] != {
    "active": True,
    "source": "learned-raw-preference",
    "count": 3,
}:
    sys.exit(1)
PY
adapter_transpose_dry_run_output=$(
    printf 'protocol\t1\npreedit\tjibengongnegn\ncandidates\t基本功呢功能\t基本功能\ncorrection\tgongnegn\tgongneng\t2\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_transpose_json="${adapter_transpose_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_transpose_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["behavior_summary"]["correction_patterns"] != [
    {"kind": "transpose", "text": "gn->ng", "position": 0, "relative_to_end": True, "count": 2}
]:
    sys.exit(1)
if prompt["behavior_summary"]["realtime_correction_decisions"] != [
    {
        "kind": "transpose",
        "text": "gn->ng",
        "position": 0,
        "relative_to_end": True,
        "count": 2,
        "status": "applied",
        "reason": "ok",
        "corrected_preedit": "jibengongneng",
    }
]:
    sys.exit(2)
PY
adapter_long_guard_dry_run_output=$(
    printf 'protocol\t1\npreedit\thaodewokanyxiahaiyoumeiyu\ncandidates\t好的我看一下还有美誉\ncorrection\tihao\tnihao\t2\n' |
        TIPE_MODEL_BACKEND=openai-compatible TIPE_MODEL_DRY_RUN=1 "$ROOT/scripts/model-adapter.sh"
)
adapter_long_guard_json="${adapter_long_guard_dry_run_output#*$'request-json\t'}"
TIPE_DRY_RUN_JSON="$adapter_long_guard_json" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_DRY_RUN_JSON"])
prompt = json.loads(request["messages"][1]["content"])
if prompt["behavior_summary"]["realtime_correction_decisions"] != [
    {
        "kind": "missing",
        "text": "n",
        "position": 0,
        "relative_to_end": False,
        "count": 2,
        "status": "guarded",
        "reason": "long-preedit",
        "corrected_preedit": "",
    }
]:
    sys.exit(1)
PY
adapter_known_correction_output=$(
    printf 'protocol\t1\npreedit\tong\ncandidates\t弄\t农\t工\ncorrection\tong\tnong\t3\ncorrection\tong\tgong\t2\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_known_correction_output" != $'correction\tong\tnong' ]]; then
    echo "model adapter heuristic should keep only the strongest known correction hint" >&2
    exit 1
fi
adapter_tied_correction_output=$(
    printf 'protocol\t1\npreedit\tong\ncandidates\t弄\t农\t工\ncorrection\tong\tnong\t3\ncorrection\tong\tgong\t3\n' |
        TIPE_MODEL_BACKEND=heuristic "$ROOT/scripts/model-adapter.sh"
)
if [[ -n "$adapter_tied_correction_output" ]]; then
    echo "model adapter heuristic should suppress tied known correction hints" >&2
    exit 1
fi
if printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
    TIPE_MODEL_BACKEND=unknown "$ROOT/scripts/model-adapter.sh" >/dev/null 2>&1; then
    echo "model adapter should reject unknown backends" >&2
    exit 1
fi

adapter_fake_bin="$tmp_dir/fake-bin"
mkdir -p "$adapter_fake_bin"
cat >"$adapter_fake_bin/curl" <<'EOF'
#!/usr/bin/env bash
request=$(cat)
if [[ "${TIPE_EXPECT_API_KEY_FILE:-}" == "1" ]]; then
    header_file=""
    previous_argument=""
    for argument in "$@"; do
        [[ "$argument" != *'fake-secret-value'* ]] || exit 58
        if [[ "$previous_argument" == "-H" && "$argument" == @* ]]; then
            header_file="${argument#@}"
        fi
        previous_argument="$argument"
    done
    if [[ -z "$header_file" || ! -r "$header_file" ]]; then
        printf 'missing readable API header file; curl args:' >&2
        printf ' %q' "$@" >&2
        printf '\n' >&2
        exit 59
    fi
    [[ "$(stat -c '%a' "$header_file")" == "600" ]] || exit 60
    [[ "$(cat "$header_file")" == 'Authorization: Bearer fake-secret-value' ]] || exit 61
    printf '%s\n' "$header_file" >"$TIPE_HEADER_PATH_LOG"
fi
if [[ "${TIPE_EXPECT_CUSTOM_URL:-}" == "1" ]]; then
    target_url="${@: -1}"
    [[ "$target_url" == "http://fake.local/base/custom/chat" ]] || exit 9
fi
TIPE_FAKE_REQUEST="$request" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_FAKE_REQUEST"])
if request["model"] != "fake-model":
    sys.exit(8)
if request["temperature"] != 0.25:
    sys.exit(7)
if request["max_tokens"] != 64:
    sys.exit(6)
message = request["messages"][1]["content"]
prompt = json.loads(message)
if prompt["preedit"] != "nihao":
    sys.exit(10)
if prompt["supervision_mode"] != "active-preedit":
    sys.exit(43)
if prompt["application"] != "Alacritty":
    sys.exit(15)
if prompt["surrounding_context"] != {"before_cursor": "刚才\tPath\\Name", "after_cursor": "后面\nText"}:
    sys.exit(24)
if prompt["candidates"] != ["你好", "你号"]:
    sys.exit(11)
if prompt["candidate_metadata"] != [
    {"index": 0, "text": "你好", "consumed_prefix": 0, "source": "full", "score": 1000000},
    {"index": 1, "text": "你号", "consumed_prefix": 0, "source": "full", "score": 999999},
]:
    sys.exit(48)
if prompt["input_state"] != {"preedit_cursor": 5, "candidate_cursor": 1, "expanded": True}:
    sys.exit(17)
if prompt["runtime_state"] != {"continuous": True}:
    sys.exit(25)
if prompt["selected_candidate"] != {"index": 1, "text": "你号"}:
    sys.exit(18)
if prompt["visible_candidates"] != [{"index": 0, "text": "你好"}, {"index": 1, "text": "你号"}]:
    sys.exit(19)
if prompt["numbered_candidates"] != [{"shortcut": "1", "index": 1, "text": "你号"}]:
    sys.exit(20)
if prompt["behavior_summary"]["recent_event_counts"] != {"letter": 1, "observed": 1, "cursor-move": 1}:
    sys.exit(21)
if prompt["behavior_summary"]["correction_event_counts"] != {"letter": 9, "backspace": 4}:
    sys.exit(22)
if prompt["behavior_summary"]["possible_corrections"] != [
    {"source": "full-delete-retype", "typo": "ihao", "corrected_preedit": "nihao"}
]:
    sys.exit(23)
if prompt["behavior_summary"]["edit_summary"] != {
    "current": "nihao",
    "cursor": 5,
    "typed_tail": "nihao",
    "last_fully_erased": "ihao",
    "last_edited_original": "ihao",
    "middle_edit_original": "",
}:
    sys.exit(35)
if prompt["behavior_summary"]["correction_patterns"] != [
    {"kind": "missing", "text": "n", "position": 0, "relative_to_end": False, "count": 2}
]:
    sys.exit(36)
if prompt["behavior_summary"]["realtime_correction_decisions"] != [
    {
        "kind": "missing",
        "text": "n",
        "position": 0,
        "relative_to_end": False,
        "count": 2,
        "status": "skipped",
        "reason": "already-present",
        "corrected_preedit": "",
    }
]:
    sys.exit(37)
if prompt["behavior_summary"]["learning_signals"] != [
    {
        "kind": "selected_candidate",
        "status": "would_learn_preference",
        "preedit": "nihao",
        "candidate": "你号",
        "index": 1,
        "suggested_protocol": "preference\tnihao\t你号\t2",
    }
]:
    sys.exit(38)
if prompt["recent_events"] != ["letter:n", "observed:Tab\tLine", "cursor-move:Down"]:
    sys.exit(12)
if prompt["recent_context"] != ["刚才", "Path\\Name"]:
    sys.exit(13)
rules = "\n".join(prompt["rules"])
if "preference<TAB>PREEDIT<TAB>CANDIDATE<TAB>COUNT" not in rules:
    sys.exit(38)
if "segment_chain<TAB>ORIGINAL<TAB>CONSUMED<TAB>COMMITTED<TAB>REMAINING<TAB>CORRECTED_FULL<TAB>COMBINED<TAB>COUNT" not in rules:
    sys.exit(39)
if "selected_candidate" not in rules:
    sys.exit(40)
if "Do not infer raw English preference from application name or code-looking context alone" not in rules:
    sys.exit(41)
if "do not hardcode example words" not in rules:
    sys.exit(42)
if "0 < consumed_prefix < preedit length" not in rules:
    sys.exit(49)
if "Do not echo rows that already appear in known_preferences, known_corrections, or recent_segment_chains" not in rules:
    sys.exit(57)
if "pass-through-only" not in rules:
    sys.exit(44)
if prompt["known_preferences"] != ["nihao\t你号\t3"]:
    sys.exit(16)
if prompt["known_corrections"] != ["ihao\tnihao\t2"]:
    sys.exit(14)
PY
printf '%s\n' '{"choices":[{"message":{"content":"candidate\t不存在\ncandidate\t你号\ncorrection\tihao\tzzzz\ncorrection\tihao\tnihao\t2\n"}}]}'
EOF
chmod +x "$adapter_fake_bin/curl"
adapter_header_path_log="$tmp_dir/adapter-header-path"
adapter_openai_output=$(
    printf 'protocol\t1\npreedit\tnihao\napplication\tAlacritty\nsurrounding_before\t刚才\\tPath\\\\Name\nsurrounding_after\t后面\\nText\ncandidates\t你好\t你号\ncandidate_metadata\t0\tconsumed_prefix\t0\tsource\tfull\tscore\t1000000\ncandidate_metadata\t1\tconsumed_prefix\t0\tsource\tfull\tscore\t999999\nstate\tpreedit_cursor\t5\tcandidate_cursor\t1\texpanded\t1\nruntime_state\tcontinuous\t1\nselected_candidate\t1\t你号\nvisible_candidates\t0:你好\t1:你号\nnumbered_candidates\t1:1:你号\nevents\tletter:n\tobserved:Tab\\tLine\tcursor-move:Down\ncorrection_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o\ncontext\t刚才\tPath\\\\Name\npreference\tnihao\t你号\t3\ncorrection\tihao\tnihao\t2\n' |
        PATH="$adapter_fake_bin:$PATH" TIPE_MODEL_BACKEND=openai-compatible \
            TIPE_MODEL_BASE_URL=http://fake.local/base/ TIPE_MODEL_CHAT_PATH=custom/chat \
            TIPE_MODEL_NAME=fake-model TIPE_MODEL_TEMPERATURE=0.25 TIPE_MODEL_MAX_TOKENS=64 \
            TIPE_MODEL_API_KEY=fake-secret-value TIPE_EXPECT_API_KEY_FILE=1 \
            TIPE_HEADER_PATH_LOG="$adapter_header_path_log" TIPE_EXPECT_CUSTOM_URL=1 \
            "$ROOT/scripts/model-adapter.sh"
)
adapter_header_path=$(sed -n '1p' "$adapter_header_path_log")
if [[ "$adapter_openai_output" != $'candidate\t你号\ncorrection\tihao\tnihao' ||
    -z "$adapter_header_path" || -e "$adapter_header_path" ]]; then
    echo "model adapter returned unexpected openai-compatible output: $adapter_openai_output" >&2
    exit 1
fi
cat >"$adapter_fake_bin/curl" <<'EOF'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' '{"choices":[{"message":{"content":"candidate\tvite\ncandidate\t不存在\n"}}]}'
EOF
chmod +x "$adapter_fake_bin/curl"
adapter_raw_english_output=$(
    printf 'protocol\t1\npreedit\tvite\napplication\tAlacritty\ncandidates\t尴尬\n' |
        PATH="$adapter_fake_bin:$PATH" TIPE_MODEL_BACKEND=openai-compatible "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_raw_english_output" != $'candidate\tvite' ]]; then
    echo "model adapter should pass through non-pinyin raw-English current preedit candidates: $adapter_raw_english_output" >&2
    exit 1
fi
cat >"$adapter_fake_bin/curl" <<'EOF'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' '{"choices":[{"message":{"content":"candidate\t不存在\ncandidate\t你号\ncorrection\tihao\tzzzz\ncorrection\tihao\tnihao\t2\n"}}]}'
EOF
chmod +x "$adapter_fake_bin/curl"
adapter_ollama_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        PATH="$adapter_fake_bin:$PATH" TIPE_MODEL_BACKEND=ollama "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_ollama_output" != $'candidate\t你号\ncorrection\tihao\tnihao' ]]; then
    echo "model adapter returned unexpected ollama output: $adapter_ollama_output" >&2
    exit 1
fi
cat >"$adapter_fake_bin/curl" <<'EOF'
#!/usr/bin/env bash
request=$(cat)
TIPE_FAKE_REQUEST="$request" python3 - <<'PY'
import json
import os
import sys

request = json.loads(os.environ["TIPE_FAKE_REQUEST"])
if request["temperature"] != 0:
    sys.exit(20)
if request["max_tokens"] != 4096:
    sys.exit(21)
PY
printf '%s\n' '{"choices":[{"message":{"content":"candidate\t你号\n"}}]}'
EOF
chmod +x "$adapter_fake_bin/curl"
adapter_clamped_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        PATH="$adapter_fake_bin:$PATH" TIPE_MODEL_BACKEND=openai-compatible \
            TIPE_MODEL_TEMPERATURE=not-a-number TIPE_MODEL_MAX_TOKENS=99999 "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_clamped_output" != $'candidate\t你号' ]]; then
    echo "model adapter returned unexpected clamped settings output: $adapter_clamped_output" >&2
    exit 1
fi
cat >"$adapter_fake_bin/curl" <<'EOF'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' '{"choices":[{"message":{"content":"candidate\t你号\r\ncorrection\tihao\tzzzz\r\ncorrection\tihao\tnihao\t2\r\n"}}]}'
EOF
chmod +x "$adapter_fake_bin/curl"
adapter_crlf_output=$(
    printf 'protocol\t1\npreedit\tnihao\ncandidates\t你好\t你号\n' |
        PATH="$adapter_fake_bin:$PATH" TIPE_MODEL_BACKEND=openai-compatible "$ROOT/scripts/model-adapter.sh"
)
if [[ "$adapter_crlf_output" != $'candidate\t你号\ncorrection\tihao\tnihao' ]]; then
    echo "model adapter returned unexpected CRLF output: $adapter_crlf_output" >&2
    exit 1
fi

if [[ -e "$ROOT/build/install_manifest.txt" ]]; then
    manifest_current_home=1
    while IFS= read -r installed_file; do
        if [[ -z "$installed_file" ]]; then
            continue
        fi
        case "$installed_file" in
            "$HOME"/*)
                if [[ ! -e "$installed_file" ]]; then
                    echo "install manifest file is missing: $installed_file" >&2
                    exit 1
                fi
                ;;
            *)
                manifest_current_home=0
                ;;
        esac
    done <"$ROOT/build/install_manifest.txt"
    if [[ "$manifest_current_home" == 1 ]]; then
        echo "installed files from last install:"
        sed 's/^/  /' "$ROOT/build/install_manifest.txt"
        printf '\n'
    else
        echo "install manifest is present but points outside the current HOME; rerun ./scripts/install.sh here to refresh it"
    fi
else
    echo "install manifest is not present; run ./scripts/install.sh after building to install TiPE"
fi

user_home=$(tipe_user_home)
if [[ -e "$user_home/.local/share/fcitx5/addon/tipeui.conf" ]]; then
    if [[ ! -e "$user_home/.local/share/fcitx5/addon/libtipeui.so" ]]; then
        echo "installed tipeui metadata exists but addon compatibility copy is missing" >&2
        exit 1
    fi
    expected_tipeui_library="Library=$user_home/.local/share/fcitx5/addon/libtipeui"
    if ! grep -Fxq "$expected_tipeui_library" "$user_home/.local/share/fcitx5/addon/tipeui.conf"; then
        echo "installed tipeui metadata should point to addon-directory compatibility copy" >&2
        exit 1
    fi
fi
if [[ -e "$user_home/.local/share/fcitx5/addon/tipe.conf" ]]; then
    if [[ ! -e "$user_home/.local/share/fcitx5/addon/libtipe.so" ]]; then
        echo "installed tipe metadata exists but addon compatibility copy is missing" >&2
        exit 1
    fi
    expected_tipe_library="Library=$user_home/.local/share/fcitx5/addon/libtipe"
    if ! grep -Fxq "$expected_tipe_library" "$user_home/.local/share/fcitx5/addon/tipe.conf"; then
        echo "installed tipe metadata should point to addon-directory compatibility copy" >&2
        exit 1
    fi
fi

if systemctl --user is-active --quiet fcitx5.service 2>/dev/null || pgrep -x fcitx5 >/dev/null 2>&1; then
    echo "fcitx5 process is running"
else
    echo "fcitx5 status could not be confirmed from this environment"
fi
