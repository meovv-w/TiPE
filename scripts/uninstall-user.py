#!/usr/bin/env python3
"""Revert owned installation files; keep personal learning by default."""
import argparse
from pathlib import Path
import subprocess
import sys

from owned_files import OwnedFiles
from user_installation import STATE, allowed_core, reload_user_units
from client_ui_installation import DIRECTORY, allowed_client, legacy


def main():
    parser = argparse.ArgumentParser(description="卸载当前用户的 TiPE，保留学习数据和用户修改的文件")
    parser.add_argument("--dry-run", action="store_true", help="列出操作，不修改用户文件和输入法")
    args = parser.parse_args()
    core = OwnedFiles(STATE / "installation", allowed_core())
    clients = OwnedFiles(DIRECTORY, allowed_client())
    if not core.manifest.exists():
        print("没有核心安装清单；不会猜测删除旧文件。请先用 install-user.sh 安装并建立清单。", file=sys.stderr)
        return 2
    # Preflight all records before switching the active IM or deleting files.
    core.read()
    client_records = legacy(clients)
    if args.dry_run:
        clean_core = core.uninstall(dry_run=True)
        clean_client = clients.uninstall(dry_run=True, records=client_records)
        subprocess.run([sys.executable, str(Path(__file__).with_name("disable-live.py")), "--dry-run"], check=True)
        print("保留：学习模型、导出文件和 profile 备份。")
        return 0 if clean_core and clean_client else 3
    subprocess.run([sys.executable, str(Path(__file__).with_name("disable-live.py"))], check=True)
    with clients:
        if not clients.manifest.exists() and client_records["files"]:
            clients.save(client_records)
        clean_client = clients.uninstall()
    with core:
        clean_core = core.uninstall()
    reload_user_units()
    print("已卸载未修改的 TiPE 文件，学习数据和备份已保留。已运行应用的模块会在退出后释放。")
    if not clean_core or not clean_client:
        print("部分文件已被用户修改；已保留对应文件和原备份。", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"卸载未完成：{error}", file=sys.stderr)
        raise SystemExit(1)
