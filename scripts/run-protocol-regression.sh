#!/usr/bin/env bash
set -euo pipefail

# Run the deterministic fcitx5 testfrontend protocol matrix and record the
# application/runtime versions available on this host.  The matrix uses the
# same capability flags reported by GTK, Qt and terminal frontends; it does
# not pretend to be a visual test of a compositor or a particular application.
build_dir="${1:-build}"
out_file="${2:-${build_dir}/protocol-regression.log}"

cmake --build "$build_dir" --target tipe-protocol-tests
mkdir -p "$(dirname "$out_file")"
{
  echo "TiPE protocol regression"
  echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "wayland_display=${WAYLAND_DISPLAY:-unavailable}"
  echo "x11_display=${DISPLAY:-unavailable}"
  echo "-- Wayland registry capability probe --"
  # Keep this read-only probe alongside the protocol matrix. It records a
  # compositor manager global when available, while explicitly distinguishing
  # that fact from a popup surface created by Fcitx's private waylandim UI.
  probe_dir="${build_dir}/wayland-capability-probe"
  "$(dirname "$0")/probe-wayland-capabilities.sh" "$probe_dir"
  echo "wayland_capabilities=${probe_dir}/capabilities.json"
  if command -v fcitx5 >/dev/null 2>&1; then fcitx5 --version 2>&1 | head -n 1; fi
  if command -v fcitx5-remote >/dev/null 2>&1; then
    im="$(fcitx5-remote -m 2>/dev/null || true)"
    echo "fcitx_im=${im:-unavailable}"
  fi
  if command -v pkg-config >/dev/null 2>&1; then
    echo "gtk3=$(pkg-config --modversion gtk+-3.0 2>/dev/null || echo unavailable)"
    echo "gtk4=$(pkg-config --modversion gtk4 2>/dev/null || echo unavailable)"
    echo "qt6=$(pkg-config --modversion Qt6Widgets 2>/dev/null || echo unavailable)"
  fi
  if command -v qmake >/dev/null 2>&1; then qmake --version 2>&1 | head -n 2; fi
  if command -v foot >/dev/null 2>&1; then foot --version 2>&1 | head -n 1; fi
  if test -x "$build_dir/tipe-fcitx-tests"; then
    cmake --build "$build_dir" --target tipe-fcitx-tests
    echo "-- independent fcitx5 testfrontend --"
    ctest --test-dir "$build_dir" --output-on-failure -R '^tipe-fcitx-tests$' 2>&1
  else
    echo "SKIP independent fcitx5 testfrontend (module unavailable at configure time)"
  fi
  echo "-- GTK/Qt/terminal capability matrix --"
  "$build_dir/tipe-protocol-tests" 2>&1
} | tee "$out_file"
echo "protocol regression log: $out_file"
