#!/usr/bin/env bash
set -euo pipefail

# Switch the current user session to TiPE without the profile/daemon race that
# occurs when fcitx5 is asked to reload an offline-edited profile.  This owns
# one deliberate daemon replacement and never touches niri configuration.
root_dir="$(cd "$(dirname "$0")/.." && pwd)"
profile="${FCITX_PROFILE:-${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/profile}"
log_file="${TIPE_FCITX_LOG:-/tmp/tipe-fcitx.log}"

# D-Bus may auto-start Fcitx while the old instance is being replaced. Keep
# the manager's activation environment in sync so that fallback activation
# loads the same user addons instead of a bare system daemon.
export FCITX_ADDON_DIRS="${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/addon:$HOME/.local/lib/fcitx5:/usr/lib64/fcitx5"
export LD_LIBRARY_PATH="${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/addon:$HOME/.local/lib/fcitx5:${LD_LIBRARY_PATH:-}"
export XDG_DATA_DIRS="${XDG_DATA_HOME:-$HOME/.local/share}:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export TIPE_UI_X11="${TIPE_UI_X11:-0}"
export TIPE_CLASSIC_LAYOUT="${TIPE_CLASSIC_LAYOUT:-horizontal}"
export TIPE_CLASSIC_GRID="${TIPE_CLASSIC_GRID:-1}"
if command -v systemctl >/dev/null 2>&1; then
  systemctl --user import-environment FCITX_ADDON_DIRS LD_LIBRARY_PATH XDG_DATA_DIRS TIPE_CLASSIC_LAYOUT TIPE_UI_X11 TIPE_CLASSIC_GRID 2>/dev/null || true
fi
if command -v dbus-update-activation-environment >/dev/null 2>&1; then
  dbus-update-activation-environment --systemd FCITX_ADDON_DIRS LD_LIBRARY_PATH XDG_DATA_DIRS TIPE_CLASSIC_LAYOUT TIPE_UI_X11 TIPE_CLASSIC_GRID 2>/dev/null || true
fi

# Keep a byte-for-byte snapshot until the replacement daemon has selected TiPE.
# fcitx5 rewrites profile on exit, so rollback is only safe while our daemon is
# stopped.  This also makes a failed activation transactional for callers.
snapshot_dir=""
profile_existed=0
had_daemon=0
systemd_active=0
started_pid=""
rollback_done=0

cleanup_snapshot() {
  [[ -n "$snapshot_dir" ]] && rm -rf -- "$snapshot_dir"
}

find_tipe_daemon() {
  local candidate
  for candidate in $(pgrep -u "$(id -u)" -x fcitx5 2>/dev/null || true); do
    if python3 "$root_dir/scripts/verify-user-daemon.py" "$candidate"; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

stop_started_daemon() {
  # systemd may briefly report MainPID=0 during activation. Never interpret
  # that sentinel as a process group when recovering a failed start.
  [[ "$started_pid" =~ ^[0-9]+$ && "$started_pid" -gt 1 ]] || return 0
  if kill -0 "$started_pid" 2>/dev/null; then
    kill "$started_pid" 2>/dev/null || true
    for _ in $(seq 1 100); do
      kill -0 "$started_pid" 2>/dev/null || break
      sleep 0.1
    done
    kill -9 "$started_pid" 2>/dev/null || true
  fi
  started_pid=""
}

restart_previous_daemon() {
  [[ "$had_daemon" == 1 ]] || return 0
  # A timeout may have left the original process alive; never start a second
  # daemon in that case.  The existing process still owns the session.
  if command -v pgrep >/dev/null 2>&1 && pgrep -u "$(id -u)" -x fcitx5 >/dev/null 2>&1; then
    return 0
  fi
  if [[ "$systemd_active" == 1 ]] && command -v systemctl >/dev/null 2>&1; then
    systemctl --user start fcitx5.service >/dev/null 2>&1 || true
  elif [[ -x "$root_dir/scripts/run-user-fcitx.sh" ]]; then
    nohup "$root_dir/scripts/run-user-fcitx.sh" >"${log_file}.rollback" 2>&1 &
  fi
}

rollback() {
  local status=$?
  [[ "$status" == 0 || "$rollback_done" == 1 ]] && return "$status"
  rollback_done=1
  # If shutdown itself failed, no profile snapshot exists yet. Restore the
  # previously running daemon without touching its profile.
  if [[ -z "$snapshot_dir" ]]; then
    restart_previous_daemon
    return "$status"
  fi
  # Do not let a failed activation leave a daemon writing the new profile.
  if [[ -n "$started_pid" ]]; then
    stop_started_daemon
    if command -v fcitx5-remote >/dev/null 2>&1; then
      fcitx5-remote -e >/dev/null 2>&1 || true
    fi
  fi
  if [[ "$profile_existed" == 1 ]]; then
    cp -p -- "$snapshot_dir/profile" "$profile" 2>/dev/null || {
      echo "激活失败，且无法恢复 profile：$profile" >&2
    }
  else
    rm -f -- "$profile"
  fi
  if [[ -f "$snapshot_dir/state" ]]; then
    cp -p -- "$snapshot_dir/state" "${profile}.tipe-state" 2>/dev/null || true
  else
    rm -f -- "${profile}.tipe-state"
  fi
  # Restore the session only when one existed before activation.
  restart_previous_daemon
  cleanup_snapshot
  return "$status"
}
trap rollback EXIT

initial_pids="$(pgrep -u "$(id -u)" -x fcitx5 2>/dev/null || true)"

if command -v systemctl >/dev/null 2>&1 &&
   systemctl --user is-active --quiet fcitx5.service 2>/dev/null; then
  had_daemon=1
  systemd_active=1
  systemctl --user stop fcitx5.service
fi
if command -v fcitx5-remote >/dev/null 2>&1 &&
   pgrep -u "$(id -u)" -x fcitx5 >/dev/null 2>&1; then
  had_daemon=1
  fcitx5-remote -e || true
fi

for _ in $(seq 1 100); do
  if ! pgrep -u "$(id -u)" -x fcitx5 >/dev/null 2>&1; then break; fi
  sleep 0.1
done
if pgrep -u "$(id -u)" -x fcitx5 >/dev/null 2>&1; then
  # Toolkit clients may D-Bus-activate a new daemon immediately after exit.
  # Adopt it only when it is a NEW process mapping the exact installed
  # module inodes; never accept a stale running engine.
  replacement="$(find_tipe_daemon || true)"
  if [[ -n "$replacement" ]] && ! grep -qx "$replacement" <<<"$initial_pids";
  then
    python3 "$root_dir/scripts/enable-live.py"
    echo "TiPE 已启用，已接管自动启动的新版 Fcitx5（PID $replacement）。"
    trap - EXIT
    exit 0
  fi
  echo "无法在 10 秒内等待旧 Fcitx5 退出；未修改 profile。" >&2
  exit 1
fi

# Snapshot only after the old daemon has flushed its in-memory state on exit.
# Taking this before stopping it would discard any recent configuration edits
# if a later step failed and rolled back.
mkdir -p "$(dirname "$profile")"
snapshot_dir="$(mktemp -d "${TMPDIR:-/tmp}/tipe-activate.XXXXXX")"
if [[ -f "$profile" ]]; then profile_existed=1; cp -p -- "$profile" "$snapshot_dir/profile"; else : >"$snapshot_dir/profile"; fi
if [[ -f "${profile}.tipe-state" ]]; then cp -p -- "${profile}.tipe-state" "$snapshot_dir/state"; fi

"$root_dir/scripts/enable-user.sh"
# Prefer the user's systemd unit so the daemon is not killed with the shell
# that invoked this script. Its manager environment was populated above with
# the user addon paths, so D-Bus activation and the unit use the same binary.
if command -v systemctl >/dev/null 2>&1 &&
   systemctl --user cat fcitx5.service >/dev/null 2>&1; then
  systemctl --user restart fcitx5.service
  daemon_pid="$(systemctl --user show -p MainPID --value fcitx5.service)"
else
  setsid "$root_dir/scripts/run-user-fcitx.sh" >"$log_file" 2>&1 < /dev/null &
  daemon_pid=$!
fi
started_pid="$daemon_pid"

ready=0
for _ in $(seq 1 100); do
  # Do not call fcitx5-remote during startup. If our process has not acquired
  # the bus yet, that command can trigger D-Bus activation of the stock
  # /usr/bin/fcitx5; a successful check would then refer to the wrong daemon.
  # D-Bus activation can win the race with our launcher, or systemd can
  # replace its initial MainPID before both addons are ready. Wait for the
  # verified bus owner instead of treating the first launcher's exit as a
  # failed installation. No controller call is needed during this wait.
  ready_pid="$(find_tipe_daemon || true)"
  if [[ -n "$ready_pid" ]]; then
    started_pid="$ready_pid"
    ready=1
    sleep 0.2
    break
  fi
  sleep 0.1
done
if [[ "$ready" != 1 ]]; then
  echo "Fcitx5 未能启动；日志：$log_file" >&2
  exit 1
fi
# The daemon must own the bus and map both exact installed user modules.
# Environment variables alone cannot prove that the right code is loaded.
tipe_pid="$(find_tipe_daemon || true)"
if [[ -z "$tipe_pid" ]]; then
  echo "Fcitx5 已被其他实例接管；未启用用户 TiPE 插件。日志：$log_file" >&2
  exit 1
fi

fcitx5-remote -s tipe
for _ in $(seq 1 50); do
  [[ "$(fcitx5-remote -n 2>/dev/null || true)" == "tipe" ]] && break
  sleep 0.1
done
if [[ "$(fcitx5-remote -n 2>/dev/null || true)" != "tipe" ]]; then
  echo "Fcitx5 已启动但当前输入法不是 TiPE；日志：$log_file" >&2
  exit 1
fi
echo "TiPE 已启用，当前输入法：tipe；日志：$log_file"
started_pid=""
cleanup_snapshot
trap - EXIT
