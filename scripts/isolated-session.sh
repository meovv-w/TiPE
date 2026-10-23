#!/usr/bin/env bash
set -u

root="${TIPE_ISO_ROOT:?}"
source_root="${TIPE_ISO_SOURCE_ROOT:?}"
build_dir="${TIPE_ISO_BUILD_DIR:?}"
display="${TIPE_ISO_DISPLAY:?}"
if [[ "$build_dir" = /* ]]; then
  build_path="$build_dir"
else
  build_path="$source_root/$build_dir"
fi
strict_apps="${TIPE_REAL_APP_STRICT_APPS:-0}"
export DISPLAY="$display"
# Keep both toolkit caches and TiPE XDG state private. In particular, the
# password probe must never create or compare a model in the real user
# HOME.
export HOME="$root/home" XDG_STATE_HOME="$root/state"
export XDG_CONFIG_HOME="$root/config" XDG_DATA_HOME="$root/data"
export XDG_RUNTIME_DIR="$root/runtime"
export FCITX_ADDON_DIRS="$root/data/fcitx5/addon:$build_path/test/addon:/usr/lib64/fcitx5:/usr/lib/fcitx5"
export LD_LIBRARY_PATH="$build_path:$build_path/test/addon:${LD_LIBRARY_PATH:-}"
export GTK_IM_MODULE=fcitx QT_IM_MODULE=fcitx XMODIFIERS=@im=fcitx
export GDK_BACKEND=x11 QT_QPA_PLATFORM=xcb TIPE_UI_X11=1
chromium_wayland_socket="${TIPE_CHROMIUM_WAYLAND_DISPLAY:-${WAYLAND_DISPLAY:-}}"
chromium_wayland_runtime="${TIPE_CHROMIUM_WAYLAND_RUNTIME:-${XDG_RUNTIME_DIR:-}}"
export TIPE_HARNESS_TIMEOUT_MS="${TIPE_HARNESS_TIMEOUT_MS:-8000}"
if [[ "${TIPE_ISOLATED_WAYLAND:-0}" = 1 &&
      -n "${TIPE_ISOLATED_WAYLAND_RUNTIME:-}" &&
      -S "${TIPE_ISOLATED_WAYLAND_RUNTIME}/${TIPE_ISOLATED_WAYLAND_DISPLAY:-wayland-0}" ]]; then
  # Opt-in native Wayland probe. Keep the default Xwayland harness unchanged
  # because its xdotool focus path is more deterministic.
  export XDG_RUNTIME_DIR="$TIPE_ISOLATED_WAYLAND_RUNTIME"
  export WAYLAND_DISPLAY="${TIPE_ISOLATED_WAYLAND_DISPLAY:-wayland-0}"
  export GDK_BACKEND=wayland QT_QPA_PLATFORM=wayland TIPE_REAL_APP_WAYLAND=1
  # Native GTK4/Qt6 Wayland clients negotiate text-input through the
  # compositor protocol; forcing the X11 Fcitx IM module here prevents that
  # handshake and leaves only literal key events.  Keep the variables unset
  # for this opt-in probe so the toolkit selects its native backend.
  unset GTK_IM_MODULE QT_IM_MODULE XMODIFIERS
else
  unset WAYLAND_DISPLAY
fi

# Start the daemon before any fcitx5-remote query.  Querying too early can
# trigger the D-Bus service activator and create a second daemon without the
# temporary FCITX_ADDON_DIRS environment.
mkdir -p "$root/out"
fcitx5 -D --disable notificationitem >"$root/fcitx.log" 2>&1 &
fcitx_pid=$!
ready=0
for _ in $(seq 1 100); do
  if ! kill -0 "$fcitx_pid" 2>/dev/null; then break; fi
  if grep -q "Loaded addon dbus" "$root/fcitx.log" 2>/dev/null && \
     grep -q "Loaded addon tipe" "$root/fcitx.log" 2>/dev/null; then
    ready=1; break
  fi
  sleep 0.1
done
if [[ "$ready" != 1 ]]; then
    echo "fcitx did not load TiPE"
    cp "$root/fcitx.log" "$root/out/isolated-fcitx.log" 2>/dev/null || true
    printf "status=failed reason=fcitx-did-not-load-tipe\n" > "$root/out/isolated-daemon-status.log"
    kill "$fcitx_pid" 2>/dev/null || true
    exit 1
  fi
# DefaultIM=tipe is the authoritative isolated selection.  This command is
# also useful when the daemon has already created a global group state.
fcitx5-remote -s tipe >/dev/null 2>&1 || true
mkdir -p "$root/out"
TIPE_REAL_APP_AUTOTYPE=1 "$source_root/scripts/run-real-app-harness.sh" \
    "$build_path" "$root/out"
harness_rc=$?

# Re-run both real toolkit windows with password/sensitive input hints. The
# harness redacts all text fields and records the capability/echo metadata;
# the model digest is expected to remain unchanged after the worker drains.
password_out="$root/password-out"
mkdir -p "$password_out"
model_path="$root/state/tipe/model.current"
if [[ -f "$model_path" ]]; then
  model_before="$(sha256sum "$model_path" | cut -d " " -f1)"
else
  model_before=absent
fi
TIPE_HARNESS_PASSWORD_ECHO=1 TIPE_REAL_APP_AUTOTYPE=1 \
  "$source_root/scripts/run-real-app-harness.sh" \
  "$build_path" "$password_out"
password_rc=$?
sleep 0.4
if [[ -f "$model_path" ]]; then
  model_after="$(sha256sum "$model_path" | cut -d " " -f1)"
else
  model_after=absent
fi
printf "before=%s\nafter=%s\n" "$model_before" "$model_after" \
  > "$root/out/password-model-digest.txt"
if [[ "$model_before" != "$model_after" ]]; then
  printf "status=failed reason=sensitive-input-changed-model\n" \
    > "$root/out/password-learning.log"
  harness_rc=1
else
  printf "status=verified reason=sensitive-input-not-persisted\n" \
    > "$root/out/password-learning.log"
fi
[[ "$password_rc" = 0 ]] || harness_rc=1
for toolkit in gtk4 qt6; do
  [[ -f "$password_out/${toolkit}.jsonl" ]] &&
    cp "$password_out/${toolkit}.jsonl" "$root/out/password-${toolkit}.jsonl"
  [[ -f "$password_out/${toolkit}.stderr" ]] &&
    cp "$password_out/${toolkit}.stderr" "$root/out/password-${toolkit}.stderr"
done

# Exercise a browser input element in a real Chromium renderer.  The page
# mirrors the input value into its title, which gives the harness a
# toolkit-independent observation channel via xdotool.  Chromium is always
# given a throw-away profile and an X11 ozone backend so this probe cannot
# touch the existing browser session or profile.
chromium_bin=""
chromium_wid=""
chromium_pid=""
for candidate in chromium-browser chromium google-chrome; do
  if command -v "$candidate" >/dev/null 2>&1; then
    chromium_bin="$candidate"
    break
  fi
done
if [[ -n "$chromium_bin" && -n "${DISPLAY:-}" && \
      -x "$(command -v xdotool 2>/dev/null || true)" ]]; then
  cat > "$root/chromium-harness.html" <<HTML
<!doctype html><meta charset="utf-8"><title>TiPE Chromium Harness</title>
<style>html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden}#input{box-sizing:border-box;width:100%;height:96px;padding:12px;font:32px sans-serif}</style>
<input id="input" autofocus aria-label="TiPE Chromium harness">
<script>
const input = document.getElementById("input");
let keydowns = 0;
let inputs = 0;
let clicks = 0;
function report() { document.title = "TiPE Chromium " + input.value + " kd" + keydowns + " inp" + inputs + " click" + clicks + " active" + (document.activeElement && document.activeElement.id || "body"); }
input.addEventListener("click", () => { clicks++; report(); });
input.addEventListener("keydown", () => { keydowns++; report(); });
input.addEventListener("input", () => { inputs++; report(); });
input.addEventListener("focus", report);
input.focus();
report();
</script>
HTML
  chromium_dir="$root/chromium-profile"
  chromium_ozone="x11"
  chromium_wayland=0
  if [[ "${TIPE_ISOLATED_CHROMIUM_WAYLAND:-0}" = 1 && -n "$chromium_wayland_socket" && -n "$chromium_wayland_runtime" ]] && command -v wtype >/dev/null 2>&1; then
    chromium_ozone="wayland"
    chromium_wayland=1
    export XDG_RUNTIME_DIR="$chromium_wayland_runtime" WAYLAND_DISPLAY="$chromium_wayland_socket"
  fi
  "$chromium_bin" --no-sandbox --disable-gpu --disable-software-rasterizer \
    --disable-first-run-ui --no-default-browser-check \
    --enable-features=UseOzonePlatform --ozone-platform="$chromium_ozone" \
    --user-data-dir="$chromium_dir" --app="file://$root/chromium-harness.html" \
    >"$root/chromium.log" 2>&1 &
  chromium_pid=$!
  if [[ "$chromium_wayland" = 1 ]]; then
    # A Wayland client has no X11 window id for xdotool. Its focused input is
    # autofocus, so drive it through the compositor and retain an honest
    # observation record when no page-title bridge is available.
    sleep 0.8
    wtype nihao 2>>"$root/chromium.log" || true
    wtype ' ' 2>>"$root/chromium.log" || true
    sleep 0.75
    printf "browser=chromium version=%s backend=wayland title=unobservable-via-x11
status=commit-not-observed
diagnostic=wayland-input-injected-with-wtype;renderer-title-channel-unavailable
" \
      "$($chromium_bin --version 2>&1 | head -n 1)" > "$root/out/chromium-smoke.log"
    [[ "$strict_apps" = 1 ]] && harness_rc=1
  else
  chromium_wid=""
  for _ in $(seq 1 80); do
    if ! kill -0 "$chromium_pid" 2>/dev/null; then break; fi
    chromium_wid="$(xdotool search --onlyvisible --name "^TiPE Chromium" 2>/dev/null | tail -n 1 || true)"
    [[ -n "$chromium_wid" ]] && break
    sleep 0.1
  done
  if [[ -n "$chromium_wid" ]]; then
    xdotool windowactivate --sync "$chromium_wid" 2>/dev/null || true
    xdotool windowfocus --sync "$chromium_wid" 2>/dev/null || true
    # Rootless Xwayland has no EWMH manager in this isolated display. Publish
    # the focused client as _NET_ACTIVE_WINDOW so Chromium's X11 event bridge
    # accepts XTest input even when windowactivate reports no WM support.
    if command -v xprop >/dev/null 2>&1; then
      xprop -root -f _NET_ACTIVE_WINDOW 32c -set _NET_ACTIVE_WINDOW "$chromium_wid" \
        2>>"$root/chromium.log" || true
    fi
    # The input deliberately fills the upper 96 px of the app viewport;
    # click inside that field rather than relying on the Chromium decoration
    # height (which differs between --app and regular windows).
    xdotool mousemove --window "$chromium_wid" 120 40 2>/dev/null || true
    xdotool click --window "$chromium_wid" 1 2>>"$root/chromium.log" || true
    sleep 0.50
    {
      printf "window=%s focus=" "$chromium_wid"
      xdotool getwindowfocus 2>&1 || true
      printf "active="
      xdotool getactivewindow 2>&1 || true
      xdotool getwindowgeometry --shell "$chromium_wid" 2>&1 || true
      xdotool getmouselocation 2>&1 || true
    } > "$root/chromium-focus.log"
    # On a Mutter headless compositor use native Wayland input when available.
    # wtype injects compositor events that Chromium's Wayland renderer accepts;
    # the X11 path below remains the diagnostic fallback.
    if [[ "$chromium_wayland" = 1 ]]; then
      wtype nihao 2>>"$root/chromium.log" || true
      wtype ' ' 2>>"$root/chromium.log" || true
    fi
    # Prefer the focused-window XSendEvent path used by the GTK/Qt harness.
    # Nested Xwayland without an EWMH window manager can drop that event for
    # Chromium, so retry through XTest when the page's keydown counter is
    # unchanged.  The title counter makes this fallback observable.
    chromium_probe="$(xdotool getwindowname "$chromium_wid" 2>/dev/null || true)"
    if [[ "$chromium_probe" == *"kd0"* ]]; then
      xdotool type --delay 120 nihao 2>>"$root/chromium.log" || true
      chromium_probe="$(xdotool getwindowname "$chromium_wid" 2>/dev/null || true)"
    else
      xdotool type --delay 120 --window "$chromium_wid" nihao 2>>"$root/chromium.log" || true
    fi
    if [[ "$chromium_probe" == *"kd0"* ]]; then
      xdotool type --delay 120 --window "$chromium_wid" nihao 2>>"$root/chromium.log" || true
    fi
    # xdotool type uses XSendEvent, which Chromium intentionally ignores on
    # some Xwayland paths. Send real XTEST key presses to the active client as
    # a final fallback; each key is observable through the renderer keydown
    # counter and remains bounded by this harness.
    chromium_probe="$(xdotool getwindowname "$chromium_wid" 2>/dev/null || true)"
    if [[ "$chromium_probe" == *"kd0"* ]]; then
      for key in n i h a o; do
        xdotool key --clearmodifiers "$key" 2>>"$root/chromium.log" || true
      done
    fi
    xdotool key --clearmodifiers --window "$chromium_wid" space 2>>"$root/chromium.log" || true
    sleep 0.75
    chromium_title="$(xdotool getwindowname "$chromium_wid" 2>/dev/null || true)"
    printf "browser=chromium version=%s window=%s title=%s\n" \
      "$("$chromium_bin" --version 2>&1 | head -n 1)" "$chromium_wid" \
      "$chromium_title" > "$root/out/chromium-smoke.log"
    if [[ "$chromium_title" == *"你好"* ]]; then
      printf "status=commit-ok text=你好\n" >> "$root/out/chromium-smoke.log"
    else
      printf "status=commit-not-observed\n" >> "$root/out/chromium-smoke.log"
      if [[ "$chromium_title" == *"kd0"* ]]; then
        printf "diagnostic=renderer-received-no-keydown-events despite activeElement=input; nested-Xwayland-focus-path\n" \
          >> "$root/out/chromium-smoke.log"
      fi
      [[ "$strict_apps" = 1 ]] && harness_rc=1
    fi
    if [[ "${TIPE_CHROMIUM_DEBUG_HOLD_SEC:-0}" =~ ^[0-9]+$ ]] &&
       (( ${TIPE_CHROMIUM_DEBUG_HOLD_SEC:-0} > 0 )); then
      sleep "${TIPE_CHROMIUM_DEBUG_HOLD_SEC:-0}"
    fi
  else
    printf "status=skipped reason=chromium-window-not-found\n" > "$root/out/chromium-smoke.log"
  fi
  fi
  kill "$chromium_pid" 2>/dev/null || true
  wait "$chromium_pid" 2>/dev/null || true
elif [[ -z "$chromium_bin" ]]; then
  printf "status=skipped reason=chromium-not-installed\n" > "$root/out/chromium-smoke.log"
else
  printf "status=skipped reason=requires-DISPLAY-and-xdotool\n" > "$root/out/chromium-smoke.log"
fi

# Foot is Wayland-native and cannot be driven on the nested Xwayland
# display. Try it when a Wayland socket is available, and retain an explicit
# skip record otherwise rather than claiming terminal coverage from GTK/Qt.
if command -v foot >/dev/null 2>&1 && [[ -n "$chromium_wayland_socket" && -n "$chromium_wayland_runtime" ]]; then
  foot_marker="$root/foot-commit.txt"
  rm -f "$foot_marker"
  foot_wid=""
  foot_pid=""
  XDG_RUNTIME_DIR="$chromium_wayland_runtime" WAYLAND_DISPLAY="$chromium_wayland_socket" \
  foot --title="TiPE Foot Harness" sh -c \
    "IFS= read -r line; printf \"%s\\n\" \"\$line\" > \"$foot_marker\"; sleep 1" \
    >"$root/foot.log" 2>&1 &
  foot_pid=$!
  foot_wid=""
  for _ in $(seq 1 80); do
    if ! kill -0 "$foot_pid" 2>/dev/null; then break; fi
    foot_wid="$(xdotool search --onlyvisible --name "^TiPE Foot Harness" 2>/dev/null | tail -n 1 || true)"
    [[ -n "$foot_wid" ]] && break
    sleep 0.1
  done
  if [[ -n "$foot_wid" ]]; then
    xdotool windowactivate --sync "$foot_wid" 2>/dev/null || true
    xdotool type --delay 120 --window "$foot_wid" nihao 2>>"$root/foot.log" || true
    xdotool key --window "$foot_wid" space 2>>"$root/foot.log" || true
    sleep 0.5
  elif command -v wtype >/dev/null 2>&1; then
    # A native Wayland terminal has no X11 window id. Its newly created
    # surface is focused by the compositor, so use compositor-level input.
    XDG_RUNTIME_DIR="$chromium_wayland_runtime" WAYLAND_DISPLAY="$chromium_wayland_socket" \
      wtype nihao 2>>"$root/foot.log" || true
    XDG_RUNTIME_DIR="$chromium_wayland_runtime" WAYLAND_DISPLAY="$chromium_wayland_socket" \
      wtype ' ' 2>>"$root/foot.log" || true
    sleep 0.5
  fi
  if [[ -f "$foot_marker" && "$(cat "$foot_marker")" == "你好" ]]; then
    printf "status=commit-ok text=你好 version=%s\n" \
      "$(foot --version 2>&1 | head -n 1)" > "$root/out/foot-smoke.log"
  elif [[ -n "$foot_wid" ]]; then
    printf "status=commit-not-observed version=%s\n" \
      "$(foot --version 2>&1 | head -n 1)" > "$root/out/foot-smoke.log"
    [[ "$strict_apps" = 1 ]] && harness_rc=1
  else
    printf "status=skipped reason=foot-window-not-found version=%s\n" \
      "$(foot --version 2>&1 | head -n 1)" > "$root/out/foot-smoke.log"
  fi
  kill "$foot_pid" 2>/dev/null || true
  wait "$foot_pid" 2>/dev/null || true
elif ! command -v foot >/dev/null 2>&1; then
  printf "status=skipped reason=foot-not-installed\n" > "$root/out/foot-smoke.log"
else
  printf "status=skipped reason=no-Wayland-display-for-foot\n" > "$root/out/foot-smoke.log"
fi

# Wine applications use the Win32/X11 bridge rather than GTK/Qt or a native
# Wayland text-input protocol.  Keep this probe deliberately separate from
# the toolkit harness: it uses a throw-away prefix and only reports success
# when the text can be read back through the nested compositor clipboard.
# A window title alone is not evidence of a commit, so every unobservable
# result is retained as an explicit skip/diagnostic record.
wine_log="$root/out/wine-smoke.log"
if ! command -v wine >/dev/null 2>&1; then
  printf "status=skipped reason=wine-not-installed\n" > "$wine_log"
elif [[ -z "${DISPLAY:-}" ]] || ! command -v xdotool >/dev/null 2>&1; then
  printf "status=skipped reason=requires-DISPLAY-and-xdotool version=%s\n" \
    "$(wine --version 2>&1 | head -n 1)" > "$wine_log"
else
  wine_prefix="$root/wineprefix"
  wine_out="$root/wine-notepad.log"
  wine_err="$root/wine-notepad.stderr"
  wine_pid=""
  wine_wid=""
  # WINEPREFIX is intentionally below the isolated root; no registry, drive
  # mapping, or user Wine process is reused.  WINEDEBUG=-all keeps the normal
  # smoke output small while wine_err preserves startup diagnostics.
  WINEPREFIX="$wine_prefix" WINEDEBUG=-all \
    WINEDLLOVERRIDES="mscoree,mshtml=" wine notepad \
    >"$wine_out" 2>"$wine_err" &
  wine_pid=$!
  for _ in $(seq 1 120); do
    if ! kill -0 "$wine_pid" 2>/dev/null; then break; fi
    wine_wid="$(xdotool search --onlyvisible --name '^Untitled - Notepad$' 2>/dev/null | tail -n 1 || true)"
    [[ -n "$wine_wid" ]] && break
    sleep 0.1
  done
  wine_version="$(wine --version 2>&1 | head -n 1)"
  if [[ -z "$wine_wid" ]]; then
    printf "status=skipped reason=wine-notepad-window-not-found version=%s\n" \
      "$wine_version" > "$wine_log"
  else
    # Rootless Xwayland has no reliable window manager focus.  Try the normal
    # activation path, then publish _NET_ACTIVE_WINDOW before sending input;
    # both paths are bounded and their failure is recorded below.
    wine_focus="$(xdotool windowactivate --sync "$wine_wid" 2>&1 || true)"
    xdotool windowfocus --sync "$wine_wid" >/dev/null 2>&1 || true
    if command -v xprop >/dev/null 2>&1; then
      xprop -root -f _NET_ACTIVE_WINDOW 32c -set _NET_ACTIVE_WINDOW "$wine_wid" \
        >>"$wine_err" 2>&1 || true
    fi
    xdotool mousemove --window "$wine_wid" 120 80 >/dev/null 2>&1 || true
    xdotool click --window "$wine_wid" 1 >>"$wine_err" 2>&1 || true
    # Keep this sequence identical to the other real-app probes.  It tests
    # the actual Fcitx XIM bridge, not a direct Unicode paste operation.
    xdotool type --delay 140 --window "$wine_wid" nihao >>"$wine_err" 2>&1 || true
    xdotool key --window "$wine_wid" space >>"$wine_err" 2>&1 || true
    sleep 0.6

    # Select and copy the document.  A nested Mutter session exposes the X11
    # clipboard on wayland-0; use it only when that socket exists.  Without a
    # readable clipboard, retaining a screenshot/diagnostic is safer than
    # treating key injection as a successful commit.
    xdotool key --window "$wine_wid" ctrl+a >>"$wine_err" 2>&1 || true
    xdotool key --window "$wine_wid" ctrl+c >>"$wine_err" 2>&1 || true
    wine_text=""
    if [[ -S "$XDG_RUNTIME_DIR/wayland-0" ]] && command -v wl-paste >/dev/null 2>&1; then
      wine_text="$(XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY=wayland-0 \
        timeout 2s wl-paste --no-newline --type text/plain 2>/dev/null || true)"
    fi
    # Record a small, non-sensitive screenshot when ImageMagick is available.
    # It is evidence for manual review only; it never upgrades a result to
    # commit-ok because OCR/font availability varies by host.
    if command -v import >/dev/null 2>&1; then
      import -window "$wine_wid" "$root/wine-notepad.png" >>"$wine_err" 2>&1 || true
    fi
    if [[ "$wine_text" == "你好" ]]; then
      printf "status=commit-ok text=你好 version=%s window=%s backend=wine-x11 observation=clipboard\n" \
        "$wine_version" "$wine_wid" > "$wine_log"
    else
      printf "status=commit-not-observed version=%s window=%s backend=wine-x11 observation=unavailable\n" \
        "$wine_version" "$wine_wid" > "$wine_log"
      printf "diagnostic=wine-notepad-text-readback-unavailable clipboard_bytes=%s focus=%s\n" \
        "${#wine_text}" "${wine_focus//$'\n'/ }" >> "$wine_log"
      [[ "$strict_apps" = 1 ]] && harness_rc=1
    fi
  fi
  # wineserver -k is scoped by WINEPREFIX and prevents a temporary prefix
  # from leaving a process behind after this probe.
  WINEPREFIX="$wine_prefix" wineserver -k >/dev/null 2>&1 || true
  if [[ -n "$wine_pid" ]]; then
    kill "$wine_pid" >/dev/null 2>&1 || true
    wait "$wine_pid" >/dev/null 2>&1 || true
  fi
  for wine_artifact in wine-notepad.log wine-notepad.stderr wine-notepad.png; do
    [[ -f "$root/$wine_artifact" ]] && cp "$root/$wine_artifact" "$root/out/$wine_artifact"
  done
fi
cp "$root/fcitx.log" "$root/out/isolated-fcitx.log" 2>/dev/null || true
printf "fcitx_name=org.fcitx.Fcitx5\n" > "$root/out/isolated-selection.txt"
printf "profile=%s\n" "$root/config/fcitx5/profile" >> "$root/out/isolated-selection.txt"
kill "$fcitx_pid" 2>/dev/null || true
wait "$fcitx_pid" 2>/dev/null || true
exit "$harness_rc"
