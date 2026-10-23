#!/usr/bin/env python3
"""Build, stage and atomically publish a reversible current-user install."""
import argparse
from pathlib import Path
import subprocess
import sys

from owned_files import OwnedFiles
from user_installation import STATE, allowed_core, dedicated_core, plan_core, reload_user_units


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", nargs="?", default="build", type=Path)
    parser.add_argument("--dry-run", action="store_true", help="仅检查安装计划，不写用户目录")
    args = parser.parse_args()
    build = args.build.absolute()
    if not args.dry_run:
        subprocess.run(["cmake", "--build", str(build), "-j4"], check=True)
    entries = plan_core(build)
    if args.dry_run:
        for target, data, mode in entries:
            print(f"安装：{target} ({len(data)} bytes, {mode:o})")
        return 0
    with OwnedFiles(STATE / "installation", allowed_core()) as files:
        files.install(entries, adopt_equal=dedicated_core())
    reload_user_units()
    print("TiPE 已原子安装；安装清单和原文件备份已保存。未修改 profile，未重启桌面程序。")
    print("启用/重启：scripts/activate-user.sh；卸载：scripts/uninstall-user.sh（默认保留学习数据）。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"安装失败：{error}", file=sys.stderr)
        raise SystemExit(1)
