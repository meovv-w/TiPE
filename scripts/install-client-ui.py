#!/usr/bin/env python3
import argparse
import os
import sys

from owned_files import OwnedFiles
from client_ui_installation import DIRECTORY, ENV_FILE, WRAPPER, allowed_client, legacy, plan_client


def main():
    parser = argparse.ArgumentParser(description="原子安装/恢复 TiPE GTK/Qt 用户模块")
    parser.add_argument("--gtk-build", default=os.environ.get("TIPE_GTK_BUILD_DIR"))
    parser.add_argument("--qt-prefix", default=os.environ.get("TIPE_QT_PREFIX_SOURCE") or os.environ.get("TIPE_QT_BUILD_PREFIX"))
    parser.add_argument("--qt-module", default=os.environ.get("TIPE_QT_MODULE"))
    parser.add_argument("--force", action="store_true", help="更新已管理文件，仍保护用户后续修改")
    parser.add_argument("--uninstall", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    files = OwnedFiles(DIRECTORY, allowed_client())
    if args.dry_run:
        value = legacy(files)
        if args.uninstall:
            return 0 if files.uninstall(dry_run=True, records=value) else 3
        else:
            for path, _, _ in plan_client(args):
                print(f"安装：{path}")
        return 0
    with files:
        value = legacy(files)
        if not files.manifest.exists() and value["files"]:
            files.save(value)
        if args.uninstall:
            return 0 if files.uninstall() else 3
        if value["files"] and not args.force:
            raise RuntimeError("已有 client UI 安装；使用 --force 更新，或先 --uninstall")
        owned = {row["path"] for row in value["files"]}
        for path in (ENV_FILE, WRAPPER):
            if (path.exists() or path.is_symlink()) and str(path) not in owned:
                raise RuntimeError(f"保留已有配置，安装取消：{path}")
        files.install(plan_client(args))
    print(f"TiPE GTK/Qt 模块已安装。新应用可通过 {WRAPPER} command 启动。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
