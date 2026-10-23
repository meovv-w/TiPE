"""Optional toolkit overlay paths, including the original TSV manifest."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from owned_files import digest
from user_installation import HOME, DATA, CONFIG, STATE

GTK = Path(os.environ.get("TIPE_GTK_PREFIX") or DATA / "tipe/gtk")
QT = Path(os.environ.get("TIPE_QT_PREFIX") or DATA / "tipe/qt")
DIRECTORY = STATE / "client-ui"
ENV_FILE = CONFIG / "environment.d/tipe-client-ui.conf"
WRAPPER = HOME / ".local/bin/tipe-run"
GTK4 = GTK / "lib/gtk-4.0/4.0.0/immodules/libim-fcitx5.so"
GTK3 = GTK / "lib/gtk-3.0/3.0.0/immodules/im-fcitx5.so"
CACHE = GTK / "lib/gtk-3.0/3.0.0/immodules.cache"
QT6 = QT / "lib/qt6/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so"


def allowed_client():
    return {GTK4, GTK3, CACHE, QT6, ENV_FILE, WRAPPER}


def legacy(files):
    if files.manifest.exists() or not (DIRECTORY / "manifest.tsv").exists():
        return files.read()
    rows = []
    for line in (DIRECTORY / "manifest.tsv").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")  # Preserve the empty backup column.
        if len(fields) != 3:
            raise RuntimeError("旧 client UI 清单格式无效")
        path, backup, sha = fields
        files.target(path)
        row = dict(path=path, sha256=sha, backup=backup or None,
                   backup_sha256=None, backup_mode=None)
        if backup:
            original = files.blob(backup)
            row.update(backup_sha256=digest(original.read_bytes()),
                       backup_mode=original.stat().st_mode & 0o777)
        rows.append(row)
    return dict(version=1, files=rows)


def plan_client(args):
    entries = []

    def add(target, source=None, data=None, mode=0o755):
        entries.append((target, Path(source).read_bytes() if source else data, mode))

    if args.gtk_build:
        build = Path(args.gtk_build)
        gtk4 = build / "gtk4/libim-fcitx5.so"
        gtk3 = next((p for p in (build / "gtk3/im-fcitx5.so", build / "gtk3/libim-fcitx5.so")
                     if p.is_file()), None)
        if not gtk4.is_file() and gtk3 is None:
            raise RuntimeError("GTK 构建目录中没有输入模块")
        if gtk4.is_file():
            add(GTK4, gtk4)
        if gtk3:
            add(GTK3, gtk3)
            query = shutil.which("gtk-query-immodules-3.0-64") or shutil.which("gtk-query-immodules-3.0")
            if query:
                cache = subprocess.check_output([query, str(gtk3.absolute())])
                cache = cache.replace(str(gtk3.absolute()).encode(), str(GTK3).encode())
                if not cache.strip():
                    raise RuntimeError("GTK3 模块缓存生成为空")
                add(CACHE, data=cache, mode=0o644)
    qt_module = args.qt_module
    if not qt_module and args.qt_prefix:
        qt_module = Path(args.qt_prefix) / "lib/qt6/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so"
    if qt_module:
        add(QT6, qt_module)
    if not entries:
        raise RuntimeError("请指定 GTK/Qt 构建产物")
    gtk_path = f"{GTK}/lib/gtk-4.0:{GTK}/lib/gtk-3.0"
    qt_path = f"{QT}/lib/qt6/plugins"
    if any(c in gtk_path + qt_path for c in "\n\r\x00"):
        raise RuntimeError("模块目录不能包含换行或 NUL")
    environment = ("# TiPE client UI (managed by install-client-ui.sh; user-owned)\n"
        f"GTK_PATH={json.dumps(gtk_path, ensure_ascii=False)}\nQT_PLUGIN_PATH={json.dumps(qt_path, ensure_ascii=False)}\n"
        "GTK_IM_MODULE=fcitx\nQT_IM_MODULE=fcitx\nXMODIFIERS=@im=fcitx\n")
    wrapper = ("#!/usr/bin/env bash\nset -euo pipefail\n"
        f"export GTK_PATH={shlex.quote(gtk_path)}\"${{GTK_PATH:+:$GTK_PATH}}\"\n"
        f"export QT_PLUGIN_PATH={shlex.quote(qt_path)}\"${{QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}}\"\n"
        'export GTK_IM_MODULE="${GTK_IM_MODULE:-fcitx}"\n'
        'export QT_IM_MODULE="${QT_IM_MODULE:-fcitx}"\n'
        'export XMODIFIERS="${XMODIFIERS:-@im=fcitx}"\nexec "$@"\n')
    add(ENV_FILE, data=environment.encode(), mode=0o644)
    add(WRAPPER, data=wrapper.encode())
    return entries
