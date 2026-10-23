#!/usr/bin/env bash
set -euo pipefail

# This harness deliberately does not switch the user's input method, edit an
# fcitx profile, or restart fcitx.  It opens a normal GTK4/Qt6 test window and
# records the toolkit's own IM signals.  To observe TiPE, select TiPE yourself
# in the current session before invoking this script.  Optional xdotool input
# is enabled only when TIPE_REAL_APP_AUTOTYPE=1 and DISPLAY is present.

build_dir="${1:-build}"
out_dir="${2:-${build_dir}/real-app-harness}"
mkdir -p "$out_dir"
log="$out_dir/run.log"
: > "$log"

now() { date -u +%Y-%m-%dT%H:%M:%SZ; }
record() { printf '%s %s\n' "$(now)" "$*" | tee -a "$log"; }
record "TiPE real application harness (toolkit-side evidence only)"
record "display=${DISPLAY:-unavailable} wayland_display=${WAYLAND_DISPLAY:-unavailable}"
record "session_type=${XDG_SESSION_TYPE:-unknown} desktop=${XDG_CURRENT_DESKTOP:-unknown}"
if [[ "${TIPE_HARNESS_PASSWORD:-0}" == 1 ||
      "${TIPE_HARNESS_SENSITIVE:-0}" == 1 ||
      "${TIPE_HARNESS_PASSWORD_ECHO:-0}" == 1 ]]; then
  record "password_probe=enabled text_payloads=redacted"
else
  record "password_probe=disabled"
fi

if command -v fcitx5 >/dev/null 2>&1; then
  record "fcitx5=$(fcitx5 --version 2>&1 | head -n 1)"
else
  record "fcitx5=unavailable"
fi
if command -v fcitx5-remote >/dev/null 2>&1; then
  im="$(fcitx5-remote -n 2>/dev/null || true)"
  record "fcitx_im=${im:-unavailable}"
else
  im=""
  record "fcitx_im=unavailable"
fi
for package in gtk+-3.0 gtk4 Qt6Widgets; do
  if command -v pkg-config >/dev/null 2>&1; then
    version="$(pkg-config --modversion "$package" 2>/dev/null || true)"
  else
    version=""
  fi
  record "${package}=${version:-unavailable}"
done
if command -v foot >/dev/null 2>&1; then
  record "foot=$(foot --version 2>&1 | head -n 1)"
else
  record "foot=unavailable"
fi

if [[ ! -x "$build_dir/tipe-gtk-harness" && ! -x "$build_dir/tipe-qt-harness" ]]; then
  record "status=skipped reason=no-gtk4-or-qt6-harness-built"
  exit 0
fi
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  record "status=skipped reason=no-display"
  exit 0
fi
if [[ "${im,,}" != "tipe" ]]; then
  record "tipe_observation=not-active (select TiPE manually to capture preedit/commit)"
else
  record "tipe_observation=active"
fi

run_one() {
  local toolkit="$1" binary="$2" name="$3"
  local output="$out_dir/${name}.jsonl"
  local pid=""
  : > "$output"
  record "launch toolkit=${toolkit} binary=${binary} output=${output}"
  # Keep the environment local to this child.  In particular, do not set
  # FCITX_ADDON_DIRS or call fcitx5-remote: the daemon must already be running
  # with the user's chosen profile.
  env GTK_IM_MODULE=fcitx QT_IM_MODULE=fcitx XMODIFIERS=@im=fcitx \
      TIPE_HARNESS_TIMEOUT_MS="${TIPE_HARNESS_TIMEOUT_MS:-7000}" \
      "$binary" >"$output" 2>"$out_dir/${name}.stderr" &
  pid=$!
  trap 'kill "$pid" 2>/dev/null || true' RETURN

  # Native Wayland clients have no X11 window id.  In the opt-in nested
  # compositor probe, the compositor focuses the newly mapped child and wtype
  # injects real Wayland keyboard events; retain the same JSONL assertions as
  # the X11 path below.
  if [[ "${TIPE_REAL_APP_AUTOTYPE:-0}" == 1 &&
        "${TIPE_REAL_APP_WAYLAND:-0}" == 1 && -n "${WAYLAND_DISPLAY:-}" &&
        -x "$(command -v wtype 2>/dev/null || true)" ]]; then
    for _ in $(seq 1 70); do
      if ! kill -0 "$pid" 2>/dev/null; then break; fi
      if grep -q '"event":"ready"' "$output" 2>/dev/null; then break; fi
      sleep 0.1
    done
    if grep -q '"event":"ready"' "$output" 2>/dev/null; then
      scenario="${TIPE_REAL_APP_SCENARIO:-nihao}"
      case "$scenario" in
        nihao) sequence_label="nihao-space" ;;
        surrounding-fallback) sequence_label="nihao-space-backspace-delete" ;;
        edit) sequence_label="nihao-space-shijie-space-tab-shifttab-home-end-pageup-pagedown-left-backspace-right-github-space-nihao-space" ;;
        *) sequence_label="nihao-space"; scenario=nihao ;;
      esac
      record "autotype toolkit=${toolkit} wayland=${WAYLAND_DISPLAY} sequence=${sequence_label}"
      sleep 0.35
      wtype nihao 2>>"$out_dir/${name}.stderr" || true
      wtype -k space 2>>"$out_dir/${name}.stderr" || true
      if [[ "$scenario" == surrounding-fallback ]]; then
        wtype -k BackSpace 2>>"$out_dir/${name}.stderr" || true
        wtype -k Delete 2>>"$out_dir/${name}.stderr" || true
      elif [[ "$scenario" == edit ]]; then
        wtype shijie 2>>"$out_dir/${name}.stderr" || true
        wtype -k space 2>>"$out_dir/${name}.stderr" || true
        wtype -k Tab 2>>"$out_dir/${name}.stderr" || true
        wtype -M shift -k Tab -m shift 2>>"$out_dir/${name}.stderr" || true
        wtype -k Home 2>>"$out_dir/${name}.stderr" || true
        wtype -k End 2>>"$out_dir/${name}.stderr" || true
        wtype -k Page_Up 2>>"$out_dir/${name}.stderr" || true
        wtype -k Page_Down 2>>"$out_dir/${name}.stderr" || true
        wtype -k Left 2>>"$out_dir/${name}.stderr" || true
        wtype -k BackSpace 2>>"$out_dir/${name}.stderr" || true
        wtype -k Right 2>>"$out_dir/${name}.stderr" || true
        wtype github 2>>"$out_dir/${name}.stderr" || true
        wtype -k space 2>>"$out_dir/${name}.stderr" || true
        wtype nihao 2>>"$out_dir/${name}.stderr" || true
        wtype -k space 2>>"$out_dir/${name}.stderr" || true
      fi
    else
      record "autotype toolkit=${toolkit} skipped reason=wayland-ready-timeout"
    fi
  elif [[ "${TIPE_REAL_APP_AUTOTYPE:-0}" == 1 && -n "${DISPLAY:-}" &&
        -x "$(command -v xdotool 2>/dev/null || true)" ]]; then
    local wid=""
    for _ in $(seq 1 30); do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      # --pid avoids depending on a translated window title (and keeps the
      # automation scoped to the child this script started).
      wid="$(xdotool search --onlyvisible --pid "$pid" 2>/dev/null | tail -n 1 || true)"
      [[ -n "$wid" ]] && break
      sleep 0.1
    done
    if [[ -n "$wid" ]]; then
      # Wait for the toolkit to install its input context before sending the
      # first key.  A mapped window alone is not sufficient: the first key
      # can otherwise arrive before GTK/Qt has registered with Fcitx.
      for _ in $(seq 1 50); do
        if grep -q '"event":"ready"' "$output" 2>/dev/null; then
          break
        fi
        sleep 0.1
      done
      scenario="${TIPE_REAL_APP_SCENARIO:-nihao}"
      case "$scenario" in
        nihao)
          sequence_label="nihao-space"
          ;;
        edit)
          sequence_label="nihao-space-shijie-space-tab-shifttab-home-end-pageup-pagedown-left-backspace-right-github-space-nihao-space"
          ;;
        surrounding-fallback)
          sequence_label="nihao-space-backspace-delete"
          ;;
        *)
          record "autotype toolkit=${toolkit} skipped reason=unknown-scenario-${scenario}"
          scenario=nihao
          sequence_label="nihao-space"
          ;;
      esac
      record "autotype toolkit=${toolkit} window=${wid} sequence=${sequence_label}"
      # Nested Xwayland and some lightweight WMs do not implement
      # _NET_ACTIVE_WINDOW.  A direct click still gives the child a real
      # toolkit focus and works in both that case and a normal X11 session.
      # Ask for activation first when an EWMH manager is available; the
      # subsequent click remains the fallback that works without one.
      xdotool windowactivate --sync "$wid" 2>>"$out_dir/${name}.stderr" || true
      xdotool mousemove --window "$wid" 100 50 2>>"$out_dir/${name}.stderr" || true
      xdotool click --window "$wid" 1 2>>"$out_dir/${name}.stderr" || true
      # Let the focus-in transaction reach the toolkit and Fcitx before the
      # first synthetic key.  Calling windowactivate here is counterproductive
      # on nested Xwayland because there may be no EWMH window manager.
      sleep 0.35
      xdotool type --delay 180 --window "$wid" nihao 2>>"$out_dir/${name}.stderr" || true
      xdotool key --window "$wid" space 2>>"$out_dir/${name}.stderr" || true
      if [[ "$scenario" == surrounding-fallback ]]; then
        # The Qt fallback harness can deliberately report no surrounding
        # text. After the Chinese commit both edits must reach QLineEdit's
        # native client path: Backspace removes one committed code point and
        # Delete is a harmless forward edit at the end. TiPE must not restore
        # the old pinyin chunk when there is no surrounding-text capability.
        xdotool key --window "$wid" BackSpace 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" Delete 2>>"$out_dir/${name}.stderr" || true
      elif [[ "$scenario" == edit ]]; then
        # Exercise two committed Chinese chunks, a real client cursor move and
        # deletion, then an English token and another Chinese commit.  The
        # harness records text/cursor notifications; it does not infer success
        # from a synthetic Fcitx callback.
        xdotool type --delay 180 --window "$wid" shijie 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" space 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" Tab 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" Shift+Tab 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" Home 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" End 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" Page_Up 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" Page_Down 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" Left 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" BackSpace 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" Right 2>>"$out_dir/${name}.stderr" || true
        xdotool type --delay 120 --window "$wid" github 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" space 2>>"$out_dir/${name}.stderr" || true
        xdotool type --delay 180 --window "$wid" nihao 2>>"$out_dir/${name}.stderr" || true
        xdotool key --window "$wid" space 2>>"$out_dir/${name}.stderr" || true
      fi
    else
      record "autotype toolkit=${toolkit} skipped reason=x11-window-not-found"
    fi
  elif [[ "${TIPE_REAL_APP_AUTOTYPE:-0}" == 1 ]]; then
    record "autotype toolkit=${toolkit} skipped reason=requires-DISPLAY-and-xdotool"
  fi
  wait "$pid" || true
  trap - RETURN
  if grep -q '"event":"ready"' "$output"; then
    record "completed toolkit=${toolkit} ready=true lines=$(wc -l < "$output")"
    if [[ "${TIPE_HARNESS_PASSWORD:-0}" == 1 ||
          "${TIPE_HARNESS_SENSITIVE:-0}" == 1 ||
          "${TIPE_HARNESS_PASSWORD_ECHO:-0}" == 1 ]]; then
      if ! grep -q '"sensitive":true' "$output"; then
        record "password_probe toolkit=${toolkit} status=failed reason=sensitive-capability-not-observed"
        harness_failure=1
      elif ! grep -q '"echo":"hidden"\|"echo_mode":"password"' "$output"; then
        record "password_probe toolkit=${toolkit} status=failed reason=password-echo-not-hidden"
        harness_failure=1
      elif grep -Eq '"text":"|"preedit":"|"commit":"|"event":"text".*"value":"|"event":"input_method".*("preedit":"|"commit":")|"event":"input_method_query".*"query":16.*"value":"' "$output"; then
        record "password_probe toolkit=${toolkit} status=failed reason=unredacted-text-payload"
        harness_failure=1
      else
        record "password_probe toolkit=${toolkit} status=verified"
      fi
    fi
  else
    record "completed toolkit=${toolkit} ready=false lines=$(wc -l < "$output")"
    harness_failure=1
  fi
}

harness_failure=0
if [[ -x "$build_dir/tipe-gtk-harness" ]]; then
  run_one gtk4 "$build_dir/tipe-gtk-harness" "gtk4"
fi
if [[ -x "$build_dir/tipe-qt-harness" ]]; then
  run_one qt6 "$build_dir/tipe-qt-harness" "qt6"
fi

record "status=complete toolkit_records_only"
record "note=JSONL records are not proof of real GTK/Qt/terminal/Wayland visual correctness"
exit "$harness_failure"
