#!/usr/bin/env python3
"""Enable TiPE through the running daemon, preserving its in-memory groups."""
import os
from pathlib import Path
import shutil
import time
from gi.repository import Gio, GLib


def main():
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(method, args=None):
        return bus.call_sync(
            'org.fcitx.Fcitx5', '/controller', 'org.fcitx.Fcitx.Controller1',
            method, args, None, Gio.DBusCallFlags.NONE, 5000, None).unpack()

    methods = call('AvailableInputMethods')[0]
    if not any(item[0] == 'tipe' for item in methods):
        raise RuntimeError('当前 Fcitx5 没有加载 TiPE，不能在线启用。')
    group = call('CurrentInputMethodGroup')[0]
    layout, items = call('InputMethodGroupInfo', GLib.Variant('(s)', (group,)))
    original_items = list(items)
    previous = call('CurrentInputMethod')[0]
    profile = Path(os.environ.get('FCITX_PROFILE', Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'fcitx5/profile'))
    if profile.is_file() and not any(name == 'tipe' for name, _ in items):
        backup = profile.with_name(profile.name + '.tipe-backup.' + str(time.time_ns()))
        shutil.copy2(profile, backup)
        print(f'已备份 profile：{backup}')
    try:
        if not any(name == 'tipe' for name, _ in items):
            items.append(('tipe', ''))
            call('SetInputMethodGroupInfo', GLib.Variant('(ssa(ss))', (group, layout, items)))
        call('SetCurrentIM', GLib.Variant('(s)', ('tipe',)))
        if call('CurrentInputMethod')[0] != 'tipe':
            raise RuntimeError('当前输入法未切换到 TiPE。')
        call('Save')
    except Exception:
        call('SetInputMethodGroupInfo', GLib.Variant('(ssa(ss))', (group, layout, original_items)))
        call('SetCurrentIM', GLib.Variant('(s)', (previous,)))
        call('Save')
        raise
    print('TiPE 已通过运行中的 Fcitx5 启用。')


if __name__ == '__main__':
    main()
