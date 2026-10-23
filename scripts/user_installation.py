"""Concrete paths and atomic installation plan for the current user."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
HOME = Path.home()
DATA = Path(os.environ.get("XDG_DATA_HOME") or HOME / ".local/share")
CONFIG = Path(os.environ.get("XDG_CONFIG_HOME") or HOME / ".config")
STATE = Path(os.environ.get("XDG_STATE_HOME") or HOME / ".local/state") / "tipe"
CLASSIC_DATA = Path(os.environ.get("FCITX_DATA_HOME") or DATA)
STARTUP_DROPIN = CONFIG / "systemd/user/fcitx5.service.d/60-tipe.conf"
CORE_RELATIVE = [
    "bin/tipe-demo", "bin/tipe-diagnose", "bin/tipe-trainer", "bin/tipe-control",
    "lib/fcitx5/libtipe.so", "lib/fcitx5/libtipe-ui.so",
    "share/applications/tipe.desktop", "share/fcitx5/inputmethod/tipe.conf",
    "share/fcitx5/addon/tipe.conf", "share/fcitx5/addon/tipe-ui.conf",
    "share/icons/hicolor/scalable/apps/tipe.svg",
    *["share/fcitx5/themes/MacOS-Dark/" + name for name in ("theme.conf", "panel.svg", "highlight.svg")]]


def destination(relative):
    return DATA / relative[6:] if relative.startswith("share/") else HOME / ".local" / relative


def allowed_core():
    return {destination(p) for p in CORE_RELATIVE} | {
        CONFIG / "fcitx5/addon" / p for p in ("libtipe.so", "libtipe-ui.so", "libclassicui.so",
                                              "tipe.conf", "tipe-ui.conf", "classicui.conf")} | {
        CONFIG / "fcitx5/inputmethod/tipe.conf", CONFIG / "fcitx5/conf/classicui.conf",
        CLASSIC_DATA / "fcitx5/addon/classicui.conf", CONFIG / "fcitx5/addon/libwaylandim.so",
        CONFIG / "fcitx5/addon/waylandim.conf", CLASSIC_DATA / "fcitx5/addon/waylandim.conf",
        STARTUP_DROPIN}


def startup_dropin():
    # Manager imports only last for the current login. Keep paths on the
    # Fcitx service itself; do not change other applications' loader paths.
    addons = f"{CONFIG}/fcitx5/addon:{HOME}/.local/lib/fcitx5:/usr/lib64/fcitx5"
    values = {
        "FCITX_ADDON_DIRS": addons,
        "LD_LIBRARY_PATH": addons,
        "XDG_DATA_DIRS": f"{DATA}:/usr/local/share:/usr/share",
        "TIPE_UI_X11": "0",
        "TIPE_CLASSIC_GRID": "1",
        "TIPE_CLASSIC_LAYOUT": "horizontal",
    }
    def quote(value):
        if any(c in value for c in "\n\r\x00"):
            raise RuntimeError("TiPE 安装路径不能包含换行或 NUL")
        return value.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%")
    return ("# TiPE: reversible service-local addon paths, including after login\n[Service]\n"
            + "".join(f'Environment="{quote(key + "=" + value)}"\n'
                      for key, value in values.items())).encode()


def reload_user_units():
    # Reload definitions only; installation must never restart the user's IM.
    if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        return
    try:
        result = subprocess.run(["systemctl", "--user", "daemon-reload"],
                                capture_output=True, timeout=5)
        if result.returncode:
            print("提示：用户服务定义尚未重载；下次登录生效。")
    except (OSError, subprocess.TimeoutExpired):
        print("提示：用户服务定义尚未重载；下次登录生效。")


def plan_core(build):
    entries = {}

    def add(target, source=None, data=None, mode=0o644):
        entries[str(target)] = (target, Path(source).read_bytes() if source else data, mode)

    add(STARTUP_DROPIN, data=startup_dropin())

    # Stage first: never truncate a library mapped by the running daemon.
    with tempfile.TemporaryDirectory(prefix="tipe-install-stage-") as temporary:
        env = dict(os.environ, DESTDIR=temporary)
        subprocess.run(["cmake", "--install", str(build), "--prefix", "/usr"], env=env, check=True)
        stage = Path(temporary) / "usr"
        actual = {str(p.relative_to(stage)) for p in stage.rglob("*") if p.is_file()}
        if actual - set(CORE_RELATIVE):
            raise RuntimeError(f"CMake 安装出现未声明文件：{sorted(actual - set(CORE_RELATIVE))}")
        for relative in sorted(actual):
            source = stage / relative
            add(destination(relative), source, mode=source.stat().st_mode & 0o777)
    for name in ("libtipe.so", "libtipe-ui.so"):
        if (build / name).exists():
            add(CONFIG / "fcitx5/addon" / name, build / name, mode=0o755)
    for name, source in (("tipe.conf", "tipe-addon.conf"), ("tipe-ui.conf", "tipe-ui.conf")):
        add(CONFIG / "fcitx5/addon" / name, ROOT / "packaging" / source)
    add(CONFIG / "fcitx5/inputmethod/tipe.conf", ROOT / "packaging/tipe.conf")
    classic = os.environ.get("TIPE_CLASSICUI_SO")
    wayland_im = os.environ.get("TIPE_WAYLANDIM_SO")
    if classic:
        source = Path(classic)
        add(CONFIG / "fcitx5/addon/libclassicui.so", source, mode=0o755)
        conf = source.parent / "classicui.conf"
        conf = conf if conf.is_file() else ROOT / "packaging/classicui.conf"
        data = re.sub(rb"(?m)^([ \t]*[0-9]+=core):[^\s]+$", rb"\1", conf.read_bytes())
        for path in (CONFIG / "fcitx5/addon/classicui.conf", CLASSIC_DATA / "fcitx5/addon/classicui.conf"):
            add(path, data=data)
        if not wayland_im and (source.parent / "libwaylandim.so").is_file():
            wayland_im = str(source.parent / "libwaylandim.so")
    if wayland_im:
        source = Path(wayland_im)
        add(CONFIG / "fcitx5/addon/libwaylandim.so", source, mode=0o755)
        # Keep the system frontend's metadata (including dependency versions)
        # byte-for-byte; this changes only the activation transaction.
        conf = Path("/usr/share/fcitx5/addon/waylandim.conf")
        if not conf.is_file():
            raise RuntimeError("找不到系统 waylandim 元数据")
        for path in (CONFIG / "fcitx5/addon/waylandim.conf", CLASSIC_DATA / "fcitx5/addon/waylandim.conf"):
            add(path, source=conf)
    conf = CONFIG / "fcitx5/conf/classicui.conf"
    old = conf.read_bytes() if conf.is_file() else b""
    if not re.search(rb"(?m)^[ \t]*Theme[ \t]*=", old):
        add(conf, data=old + b"\n# TiPE default visual style\nTheme=MacOS-Dark\n")
    return list(entries.values())


def dedicated_core():
    # Shared configs/themes always retain their preexisting contents.
    return {str(p) for p in allowed_core() if "classicui" not in str(p) and "waylandim" not in str(p)
            and "/themes/" not in str(p) and p != STARTUP_DROPIN}
