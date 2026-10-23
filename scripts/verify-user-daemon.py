#!/usr/bin/env python3
"""Verify installed module mappings and D-Bus ownership without activation."""
import os
from pathlib import Path
import subprocess
import sys


def verify_mappings(pid, paths):
    process = Path("/proc") / str(pid)
    if process.stat().st_uid != os.getuid():
        return False
    lines = process.joinpath("maps").read_text().splitlines()
    for path in paths:
        path = Path(path).absolute()
        inode = path.stat().st_ino
        # Btrfs st_dev need not match the superblock device in proc maps.
        if not any(len(parts := row.split(maxsplit=5)) == 6
                   and int(parts[4]) == inode and parts[5] == str(path)
                   for row in lines):
            return False
    return True


def main():
    if len(sys.argv) != 2 or not sys.argv[1].isdecimal():
        return 2
    pid = int(sys.argv[1])
    base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "fcitx5/addon"
    try:
        paths = [base / "libtipe.so", base / "libclassicui.so"]
        if (base / "libwaylandim.so").exists():
            paths.append(base / "libwaylandim.so")
        if not verify_mappings(pid, paths):
            return 1
        # Asking the bus daemon about an existing owner never starts Fcitx.
        result = subprocess.run(
            ["busctl", "--user", "call", "org.freedesktop.DBus",
             "/org/freedesktop/DBus", "org.freedesktop.DBus",
             "GetConnectionUnixProcessID", "s", "org.fcitx.Fcitx5"],
            capture_output=True, text=True, timeout=2, check=True)
        return 0 if result.stdout.split() == ["u", str(pid)] else 1
    except (OSError, ValueError, subprocess.SubprocessError):
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
