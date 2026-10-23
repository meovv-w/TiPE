#!/usr/bin/env python3
"""Remove the declared staging artifacts, including trainer and themes."""
import argparse
from pathlib import Path

from user_installation import CORE_RELATIVE

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("stage", nargs="?", default="/tmp/tipe-staging", type=Path)
args = parser.parse_args()
stage = args.stage.absolute()
if stage.resolve() in (Path("/"), Path("/usr"), Path("/usr/local"), Path.home()):
    parser.error("拒绝把真实用户或系统目录当成安装预演目录")
prefix = stage / "usr/local"
for relative in [*CORE_RELATIVE, "lib/fcitx5/tipe.so"]:
    target = prefix / relative
    if any(parent.is_symlink() for parent in target.parents):
        parser.error(f"预演目标的父目录是符号链接：{target}")
    if target.is_file() or target.is_symlink():
        target.unlink()
print("已清理预演目录中的全部 TiPE 安装文件。")
