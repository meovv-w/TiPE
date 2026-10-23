#!/usr/bin/env python3
"""Remove only TiPE from live Fcitx groups, or edit an offline profile."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

from owned_files import atomic_write


def remove_live(call, variant, dry_run=False):
    current_group = call("CurrentInputMethodGroup")[0]
    current_im = call("CurrentInputMethod")[0]
    snapshots = []
    for group in call("InputMethodGroups")[0]:
        layout, items = call("InputMethodGroupInfo", variant("(s)", (group,)))
        if not any(name == "tipe" for name, _ in items):
            continue
        full = call("FullInputMethodGroupInfo", variant("(s)", (group,)))
        default = full[1]
        remaining = [(name, item_layout) for name, item_layout in items if name != "tipe"]
        if not remaining:
            remaining = [("keyboard-" + (layout or "us"), "")]
        fallback = default if any(name == default for name, _ in remaining) else next(
            (name for name, _ in remaining if not name.startswith("keyboard-")), remaining[0][0])
        snapshots.append((group, layout, list(items), default, remaining, fallback))
    if dry_run:
        for group, *_ in snapshots:
            print(f"从输入法组移除 TiPE：{group}")
        return

    def set_group(group, layout, items, default):
        call("SetInputMethodGroupInfo", variant("(ssa(ss))", (group, layout, items)))
        call("SwitchInputMethodGroup", variant("(s)", (group,)))
        if default:
            call("SetCurrentIM", variant("(s)", (default,)))

    try:
        for group, layout, items, default, remaining, fallback in snapshots:
            set_group(group, layout, remaining, fallback)
        call("SwitchInputMethodGroup", variant("(s)", (current_group,)))
        if current_im and current_im != "tipe":
            call("SetCurrentIM", variant("(s)", (current_im,)))
        call("Save")
    except Exception:
        for group, layout, items, default, *_ in snapshots:
            set_group(group, layout, items, default)
        call("SwitchInputMethodGroup", variant("(s)", (current_group,)))
        call("SetCurrentIM", variant("(s)", (current_im,)))
        call("Save")
        raise
    print("已从运行中的 Fcitx5 移除 TiPE，其他输入法仍可使用。")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    profile = Path(os.environ.get("FCITX_PROFILE") or Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "fcitx5/profile")
    from gi.repository import Gio, GLib
    bus = None
    if os.environ.get("DBUS_SESSION_BUS_ADDRESS") or (Path(os.environ.get("XDG_RUNTIME_DIR", "/nonexistent")) / "bus").exists():
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    owner = bus and bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", GLib.Variant("(s)", ("org.fcitx.Fcitx5",)),
        None, Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
    if owner:
        def call(method, values=None):
            return bus.call_sync("org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1",
                method, values, None, Gio.DBusCallFlags.NO_AUTO_START, 5000, None).unpack()
        # Flush the daemon's actual in-memory groups before taking a private
        # recovery snapshot. Never restore a file behind a running daemon.
        if not args.dry_run:
            call("Save")
            if profile.is_file():
                backup = profile.with_name(profile.name + ".tipe-uninstall-backup." + str(time.time_ns()))
                atomic_write(backup, profile.read_bytes(), 0o600)
                print(f"已备份卸载前 profile：{backup}")
        remove_live(call, GLib.Variant, args.dry_run)
    else:
        if args.dry_run:
            print(f"离线移除 TiPE profile 条目：{profile}")
        else:
            if profile.is_file():
                backup = profile.with_name(profile.name + ".tipe-uninstall-backup." + str(time.time_ns()))
                atomic_write(backup, profile.read_bytes(), 0o600)
            subprocess.run([sys.executable, str(Path(__file__).with_name("fcitx-profile.py")), "disable", str(profile)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
