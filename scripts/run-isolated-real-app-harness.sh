#!/usr/bin/env bash
set -euo pipefail

# Run the real GTK4/Qt6 harness against a private Fcitx5 instance.  The
# user's profile and the running desktop daemon are left untouched.  A nested
# Xwayland display is preferred because this image does not always provide
# Xvfb; Xvfb is used when available and no Wayland display exists.

build_dir="${1:-build}"
out_dir="${2:-${build_dir}/isolated-real-app-harness}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
source_root="$(cd "$script_dir/.." && pwd)"
if [[ "$build_dir" = /* ]]; then
  build_path="$build_dir"
else
  build_path="$source_root/$build_dir"
fi
if [[ "$out_dir" = /* ]]; then
  out_path="$out_dir"
else
  out_path="$source_root/$out_dir"
fi
mkdir -p "$out_path"
: > "$out_path/isolated-session.log"

record() { printf '%s\n' "$*" | tee -a "$out_path/isolated-session.log"; }
fail() { record "status=failed reason=$*"; exit 1; }

if [[ ! -x "$build_path/tipe-gtk-harness" && ! -x "$build_path/tipe-qt-harness" ]]; then
  record "status=skipped reason=no-gtk4-or-qt6-harness-built"
  exit 0
fi
# A partial build may leave a harness executable without the Fcitx addon. Fail
# early with a precise diagnostic instead of creating a broken symlink and
# reporting only fcitx-did-not-load-tipe later.
if [[ ! -f "$build_path/libtipe.so" ]]; then
  record "status=skipped reason=no-tipe-addon-library path=$build_path/libtipe.so"
  exit 0
fi
command -v dbus-run-session >/dev/null 2>&1 || fail "dbus-run-session-unavailable"

root="$(mktemp -d "${TMPDIR:-/tmp}/tipe-isolated.XXXXXX")"
xpid=""
cleanup() {
  if [[ -n "$xpid" ]]; then kill "$xpid" 2>/dev/null || true; fi
  # Chromium/Wine can create large caches in the throw-away session. Always
  # remove the private root on normal exit and on an interrupted probe so
  # repeated diagnostics cannot accumulate unbounded /tmp data.
  if [[ -n "${root:-}" && -d "$root" ]]; then
    rm -rf -- "$root"
  fi
}
trap cleanup EXIT INT TERM

mkdir -p "$root/config/fcitx5" "$root/data/fcitx5/inputmethod" \
         "$root/data/fcitx5/addon" "$root/runtime" "$root/home" "$root/state"
cp "$source_root/packaging/tipe.conf" \
   "$root/data/fcitx5/inputmethod/tipe.conf"
cp "$source_root/packaging/tipe-addon.conf" \
   "$root/data/fcitx5/addon/tipe.conf"
if [[ -f "$source_root/packaging/tipe-ui.conf" ]]; then
  cp "$source_root/packaging/tipe-ui.conf" \
     "$root/data/fcitx5/addon/tipe-ui.conf"
fi
ln -s "$build_path/libtipe.so" "$root/data/fcitx5/addon/libtipe.so"
if [[ -f "$build_path/libtipe-ui.so" ]]; then
  ln -s "$build_path/libtipe-ui.so" \
        "$root/data/fcitx5/addon/libtipe-ui.so"
fi
cat > "$root/config/fcitx5/profile" <<'EOF'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=tipe

[Groups/0/Items/0]
Name=tipe
Layout=

[GroupOrder]
0=Default
EOF

display_fd="$root/displayfd"
exec 9>"$display_fd"
if command -v Xvfb >/dev/null 2>&1; then
  Xvfb -displayfd 9 -screen 0 1280x720x24 >"$root/xserver.log" 2>&1 &
  xpid=$!
  display_kind=xvfb
elif [[ "${TIPE_ISOLATED_X11_WM:-0}" = 1 ]] && command -v mutter >/dev/null 2>&1; then
  # Mutter's headless backend owns a nested Xwayland and a real EWMH window
  # manager. This is opt-in because it needs a render node on some hosts.
  # Keep its runtime/auth files inside the throw-away session.
  mkdir -p "$root/runtime"
  chmod 700 "$root/runtime"
  XDG_RUNTIME_DIR="$root/runtime" mutter --headless --virtual-monitor 1280x720 >"$root/xserver.log" 2>&1 &
  xpid=$!
  display_kind=mutter-headless
elif command -v Xwayland >/dev/null 2>&1 && [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
  Xwayland -rootless -terminate -displayfd 9 >"$root/xserver.log" 2>&1 &
  xpid=$!
  display_kind=xwayland
else
  record "status=skipped reason=no-Xvfb-and-no-Wayland-for-nested-Xwayland"
  exit 0
fi
if [[ "$display_kind" = mutter-headless ]]; then
  # Mutter prints the public X11 display once its Xwayland child is ready.
  for _ in $(seq 1 100); do
    display="$(sed -n 's/.*public X11 display \(:[0-9]*\).*/\1/p' "$root/xserver.log" | head -n 1)"
    [[ -n "$display" ]] && break
    sleep 0.1
  done
  [[ -n "${display:-}" ]] || fail "mutter-x11-display-did-not-start"
  auth="$(find "$root/runtime" -maxdepth 1 -name '.mutter-Xwaylandauth.*' -type f -print -quit 2>/dev/null || true)"
  [[ -n "$auth" ]] || auth="$(find "${XDG_RUNTIME_DIR:-/run/user/$UID}" -maxdepth 1 -name '.mutter-Xwaylandauth.*' -type f -print -quit 2>/dev/null || true)"
  [[ -n "$auth" ]] && export XAUTHORITY="$auth"
else
  for _ in $(seq 1 100); do
    [[ -s "$display_fd" ]] && break
    sleep 0.1
  done
  [[ -s "$display_fd" ]] || fail "nested-display-did-not-start"
  display=":$(head -n 1 "$display_fd")"
fi
record "isolated_root=$root"
record "display_kind=$display_kind display=$display"
record "profile=$root/config/fcitx5/profile"
if [[ "$display_kind" = mutter-headless && "${TIPE_ISOLATED_CHROMIUM_WAYLAND:-0}" = 1 ]]; then
  # The Wayland Chromium probe is opt-in; GTK/Qt and the isolated Fcitx
  # daemon remain on X11. Pass the compositor socket as a probe-only value;
  # exporting WAYLAND_DISPLAY for the whole session makes X11 toolkit
  # clients accidentally bind Fcitx's wayland frontend.
  export TIPE_CHROMIUM_WAYLAND_RUNTIME="$root/runtime"
  export TIPE_CHROMIUM_WAYLAND_DISPLAY=wayland-0
fi

set +e
inner_script="$script_dir/isolated-session.sh"
[[ -x "$inner_script" ]] || chmod +x "$inner_script"
iso_source_root="$source_root"
set +e
wayland_probe_env=()
if [[ "$display_kind" = mutter-headless ]]; then
  # Mutter's headless backend provides a private wayland-0 socket alongside
  # its nested Xwayland display. Pass it only to the isolated child session;
  # the user's compositor/socket remains untouched.
  wayland_probe_env=(TIPE_ISOLATED_WAYLAND=1
                    TIPE_ISOLATED_WAYLAND_RUNTIME="$root/runtime"
                    TIPE_ISOLATED_WAYLAND_DISPLAY=wayland-0)
fi
env "${wayland_probe_env[@]}" \
  TIPE_ISO_ROOT="$root" TIPE_ISO_SOURCE_ROOT="$iso_source_root" TIPE_ISO_BUILD_DIR="$build_path" TIPE_ISO_DISPLAY="$display" \
  dbus-run-session -- "$inner_script" \
  >"$out_path/isolated-dbus-session.log" 2>&1
session_rc=$?
set -e

cp "$root/xserver.log" "$out_path/isolated-xserver.log" 2>/dev/null || true
cp "$root/out"/* "$out_path/" 2>/dev/null || true
record "session_rc=$session_rc"

# Require an actual TiPE commit from every toolkit that was built during the
# normal probe.  Password probes intentionally use a sensitive field and
# verify hidden/redacted handling plus an unchanged model instead of a Chinese
# commit, so they must not be rejected by this ordinary-field assertion.
password_probe=0
if [[ "${TIPE_HARNESS_PASSWORD:-0}" = 1 ||
      "${TIPE_HARNESS_SENSITIVE:-0}" = 1 ||
      "${TIPE_HARNESS_PASSWORD_ECHO:-0}" = 1 ]]; then
  password_probe=1
fi
if [[ "$password_probe" = 0 ]]; then
  if [[ -x "$build_path/tipe-gtk-harness" &&
        "${TIPE_REAL_APP_SCENARIO:-nihao}" != surrounding-fallback ]] &&
     ! grep -q '"event":"commit","text":"你好"' "$out_path/gtk4.jsonl"; then
    fail "gtk4-did-not-commit-你好"
  fi
  if [[ -x "$build_path/tipe-qt-harness" ]] &&
     ! grep -q '"commit":"你好"' "$out_path/qt6.jsonl"; then
    fail "qt6-did-not-commit-你好"
  fi
else
  [[ -f "$out_path/password-learning.log" ]] ||
    fail "password-learning-record-missing"
  grep -q 'status=verified' "$out_path/password-learning.log" ||
    fail "password-learning-check-failed"
fi
[[ "$session_rc" = 0 ]] || fail "isolated-session-exit-$session_rc"
record "status=complete isolated_tipe_commit=true"
