#!/usr/bin/env python3
"""Installed niri on a private headless Sway: actual IME keys and scale changes.

The outer virtual keyboard becomes a Winit keyboard event in niri, traversing
its real input-method grab. No physical devices or session environment changes.
This is a single niri output with live scale changes, not physical multioutput.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from desktop_pointer import prepare_pointer
from desktop_keys import letters, special

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('native_popup', ROOT/'scripts/check-native-popup-desktop.py')
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


def prepare_keyboard():
    directory = ROOT/'build/desktop-keyboard'
    directory.mkdir(exist_ok=True)
    protocol = ROOT/'integration/protocols/virtual-keyboard-unstable-v1.xml'
    source = ROOT/'tests/desktop_keyboard.c'
    binary = directory/'keyboard'
    if not binary.exists() or binary.stat().st_mtime < max(source.stat().st_mtime, protocol.stat().st_mtime):
        subprocess.run(['wayland-scanner', 'client-header', str(protocol),
                        str(directory/'tipe-virtual-keyboard-client.h')], check=True)
        subprocess.run(['wayland-scanner', 'private-code', str(protocol), str(directory/'protocol.c')], check=True)
        flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'wayland-client', 'xkbcommon'], text=True).split()
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-I'+str(directory), str(source),
                        str(directory/'protocol.c'), '-o', str(binary), *flags], check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frontends', nargs='+', choices=['gtk', 'qt', 'foot', 'browser'],
                        default=['gtk', 'qt', 'foot', 'browser'])
    parser.add_argument('--scales', nargs='+', type=float, default=[1, 2, 1.25, 1.5])
    parser.add_argument('--matrix', choices=['candidates', 'arrows'], default='candidates')
    parser.add_argument('--probe-discord', action='store_true',
                        help='Observe the unmodified Discord UI with an empty profile; do not type')
    parser.add_argument('--discord-probe-samples', type=int, default=18,
                        help='Number of five-second Discord UI observations (1 to 36)')
    args = parser.parse_args()
    if not 1 <= args.discord_probe_samples <= 36:
        parser.error('--discord-probe-samples must be between 1 and 36')
    if os.environ.get('TIPE_PRIVATE_NIRI_SESSION') != '1':
        with tempfile.TemporaryDirectory(prefix='tipe-niri-bus-') as directory:
            config = Path(directory)/'bus.conf'
            config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>'
                              '<auth>EXTERNAL</auth><policy context="default">'
                              '<allow send_destination="*"/><allow receive_sender="*"/>'
                              '<allow own="*"/><allow user="*"/></policy></busconfig>')
            return subprocess.call(['dbus-run-session', '--config-file', str(config), '--',
                                    sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]],
                                   env=dict(os.environ, TIPE_PRIVATE_NIRI_SESSION='1'))

    real_home = Path.home()
    # Software rendering belongs to the fixture, not the typing workload.
    # Keep every child below interactive desktop priority.
    os.nice(10)
    installed = real_home/'.config/fcitx5/addon'
    sway_root = Path(os.environ.get('TIPE_TEST_SWAY_ROOT', ROOT/'build/sway-private/root')).resolve()
    sway = str(sway_root/'usr/bin/sway') if sway_root.exists() else shutil.which('sway')
    swaymsg = str(sway_root/'usr/bin/swaymsg') if sway_root.exists() else shutil.which('swaymsg')
    assert sway and swaymsg
    pointer = prepare_pointer()
    keyboard_binary = prepare_keyboard()
    out = ROOT/('build/discord-native-probe' if args.probe_discord else
                'build/niri-isolated-arrows' if args.matrix == 'arrows' else 'build/niri-isolated-regression')
    out.mkdir(parents=True, exist_ok=True)
    results, processes, files = [], [], []
    runtime = dict(compositor=subprocess.check_output(['niri', '--version'], text=True).strip(),
                   private_session=True, physical_input=False, multioutput=False, complete=False,
                   modules={})
    (out/'results.json').write_text('[]\n')
    (out/'runtime.json').write_text(json.dumps(runtime, indent=2)+'\n')

    with tempfile.TemporaryDirectory(prefix='tipe-niri-isolated-') as directory:
        private = Path(directory)
        env = dict(os.environ)
        for name, sub in [('HOME', 'home'), ('XDG_RUNTIME_DIR', 'runtime'), ('XDG_CONFIG_HOME', 'config'),
                          ('XDG_STATE_HOME', 'state'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache')]:
            target = private/sub
            target.mkdir(mode=0o700)
            env[name] = str(target)
        for name in ['DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'NIRI_SOCKET', 'SWAYSOCK',
                     'NOTIFY_SOCKET', 'NOTIFY_FD', 'GTK_IM_MODULE', 'QT_IM_MODULE', 'NIRI_CONFIG']:
            env.pop(name, None)
        env.update(LD_LIBRARY_PATH=f'{installed}:{sway_root}/usr/lib64',
                   WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1', WLR_RENDERER='pixman',
                   LIBGL_ALWAYS_SOFTWARE='1', LP_NUM_THREADS='2', NIRI_DISABLE_SYSTEM_MANAGER_NOTIFY='1')

        def launch(command, name, child_env=None, stdout=None):
            handle = (out/name).open('w')
            files.append(handle)
            process = subprocess.Popen(command, env=child_env or env,
                                       stdout=stdout or handle, stderr=handle)
            processes.append(process)
            return process

        def wait_for(check, process, description):
            for _ in range(150):
                assert process.poll() is None, description+' exited'
                value = check()
                if value:
                    return value
                time.sleep(.1)
            raise AssertionError(description+' not ready')

        try:
            sway_config = private/'sway.conf'
            sway_config.write_text('output HEADLESS-1 resolution 1920x1440 scale 1\n'
                                   'xwayland disable\nfocus_follows_mouse no\nseat seat0 fallback true\n')
            outer = launch([sway, '--unsupported-gpu', '-c', str(sway_config)], 'sway.log')
            outer_socket = wait_for(lambda: list((private/'runtime').glob('sway-ipc.*.sock')), outer, 'Sway')[0]
            displays = [p for p in (private/'runtime').glob('wayland-*') if not p.name.endswith('.lock')]
            assert len(displays) == 1
            outer_env = dict(env, SWAYSOCK=str(outer_socket), WAYLAND_DISPLAY=displays[0].name)

            def outer_ipc(*command):
                return json.loads(subprocess.check_output([swaymsg, '-r', *command], env=outer_env, text=True))

            def nodes(node):
                yield node
                for child in node.get('nodes', [])+node.get('floating_nodes', []):
                    yield from nodes(child)

            niri_config = private/'niri.kdl'
            niri_config.write_text('animations { off; }\nxwayland-satellite { off; }\n'
                                   'hotkey-overlay { skip-at-startup; }\n'
                                   'prefer-no-csd\nlayout { gaps 0; border { off; }; focus-ring { off; }; }\n')
            subprocess.run(['niri', 'validate', '-c', str(niri_config)], env=env, check=True)
            compositor = launch(['niri', '-c', str(niri_config)], 'niri.log', outer_env)
            niri_socket = wait_for(lambda: list((private/'runtime').glob(f'niri.*.{compositor.pid}.sock')),
                                   compositor, 'niri')[0]
            inner_displays = [p for p in (private/'runtime').glob('wayland-*')
                              if not p.name.endswith('.lock') and p.name != outer_env['WAYLAND_DISPLAY']]
            assert len(inner_displays) == 1
            env.update(NIRI_SOCKET=str(niri_socket), WAYLAND_DISPLAY=inner_displays[0].name)
            owner = wait_for(lambda: [n for n in nodes(outer_ipc('-t', 'get_tree'))
                                     if n.get('pid') == compositor.pid], compositor, 'Nested niri window')[0]
            outer_id = owner['id']
            response = outer_ipc(f'[con_id={outer_id}] fullscreen enable, focus')
            assert all(r.get('success') for r in response), response
            time.sleep(.5)

            def inner_ipc(*command):
                return json.loads(subprocess.check_output(['niri', 'msg', '-j', *command], env=env, text=True))

            def action(*command):
                subprocess.run(['niri', 'msg', 'action', *command], env=env, check=True,
                               stdout=subprocess.DEVNULL)

            # Winit uses physical scancodes; wtype's dynamic scratch keycodes
            # would turn its letters into unrelated US keys in the nested seat.
            keyboard = subprocess.Popen([str(keyboard_binary)], env=outer_env,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            processes.append(keyboard)
            def keyboard_reply(expected):
                assert select.select([keyboard.stdout], [], [], 5)[0], 'Private keyboard response timed out'
                assert keyboard.stdout.readline().strip() == expected
            keyboard_reply('ready')
            data = private/'data/fcitx5'
            (data/'addon').mkdir(parents=True)
            (data/'inputmethod').mkdir()
            for source, target in [('tipe-addon.conf', 'addon/tipe.conf'), ('tipe.conf', 'inputmethod/tipe.conf')]:
                shutil.copy2(ROOT/'packaging'/source, data/target)
            for name in ['libtipe.so', 'libclassicui.so', 'libwaylandim.so']:
                (data/'addon'/name).symlink_to(installed/name)
            config = private/'config/fcitx5'
            config.mkdir()
            (config/'profile').write_text('[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=tipe\n'
                                         '\n[Groups/0/Items/0]\nName=tipe\nLayout=\n\n[GroupOrder]\n0=Default\n')
            env.update(FCITX_ADDON_DIRS=f'{data}/addon:/usr/lib64/fcitx5', TIPE_CLASSIC_GRID='1',
                       TIPE_CLASSIC_LAYOUT='horizontal', TIPE_UI_X11='0', TIPE_CLOUD_ENABLED='0',
                       TIPE_CLOUD_CONSENT='0', GTK_A11Y='none', NO_AT_BRIDGE='1',
                       GTK_PATH=str(real_home/'.local/share/tipe/gtk/lib/gtk-4.0'),
                       QT_PLUGIN_PATH=str(real_home/'.local/share/tipe/qt/lib/qt6/plugins'),
                       GDK_BACKEND='wayland', QT_QPA_PLATFORM='wayland', TIPE_HARNESS_TIMEOUT_MS='600000')
            if args.matrix == 'arrows':
                # These assertions use the base dictionary order. Repeated
                # selections must not train the private fixture mid-matrix.
                env['TIPE_LEARNER_READ_ONLY'] = '1'
            daemon = launch(['fcitx5', '--disable', 'notificationitem'], 'fcitx.log')
            # The engine loads before the frontend/UI. Wait for all three
            # installed modules before inspecting mappings or sending keys.
            def modules_ready():
                maps = Path(f'/proc/{daemon.pid}/maps').read_text()
                return all(any(len(parts := line.split(maxsplit=5)) == 6 and
                               int(parts[4]) == (installed/name).stat().st_ino
                               for line in maps.splitlines())
                           for name in ['libtipe.so', 'libclassicui.so', 'libwaylandim.so'])
            wait_for(modules_ready, daemon, 'Fcitx modules')
            maps = Path(f'/proc/{daemon.pid}/maps').read_text()
            for name in ['libtipe.so', 'libclassicui.so', 'libwaylandim.so']:
                path = installed/name
                assert any(len(parts := line.split(maxsplit=5)) == 6 and int(parts[4]) == path.stat().st_ino
                           for line in maps.splitlines()), name
                runtime['modules'][name] = dict(path=str(path), inode=path.stat().st_ino,
                                               sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            (out/'runtime.json').write_text(json.dumps(runtime, indent=2)+'\n')

            if args.probe_discord:
                from discord_native_probe import probe
                observed = probe(ROOT, real_home, private, out, env, launch,
                                 lambda: outer.poll() is None and compositor.poll() is None,
                                 samples=args.discord_probe_samples)
                runtime['probe_complete'] = True
                runtime['discord_real_ui_ready'] = observed['real_ui_ready']
                (out/'runtime.json').write_text(json.dumps(runtime, indent=2)+'\n')
                return 0

            for frontend in args.frontends:
                client_env = dict(env, WAYLAND_DEBUG='client')
                control = None
                client_out = out/(frontend+'.jsonl')
                if frontend in ['gtk', 'qt']:
                    client_env.update(GTK_IM_MODULE='fcitx', QT_IM_MODULE='fcitx')
                    handle = client_out.open('w')
                    files.append(handle)
                    fixture = launch([str(ROOT/f'build/tipe-{frontend}-harness')], frontend+'.log', client_env, handle)
                elif frontend == 'foot':
                    reader = private/'reader.py'
                    reader.write_text(native.READER)
                    server = str(private/'reader.sock')
                    control = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
                    control.bind(str(private/'control.sock'))
                    control.settimeout(5)
                    fixture = launch(['foot', '--app-id=tipe-owned-niri', 'python3', '-u', str(reader),
                                      str(out/'foot-text.json'), server], frontend+'.log', client_env)
                else:
                    profile = private/'browser-profile'
                    page = out/'input.html'
                    page.write_text('<!doctype html><meta charset=utf-8><title>TiPE owned isolated niri</title>'
                                    '<style>body{background:white}input{font:24px sans-serif;width:85%}</style>'
                                    '<input autofocus><script>window.events=[];let e=document.querySelector("input");'
                                    'for(let t of ["input","compositionstart","compositionupdate","compositionend"])'
                                    'e.addEventListener(t,x=>events.push({type:t,data:x.data,value:e.value}));</script>')
                    fixture = launch([shutil.which('chromium-browser') or 'chromium',
                                      '--user-data-dir='+str(profile), '--no-first-run', '--no-default-browser-check',
                                      '--disable-background-networking', '--disable-extensions', '--remote-debugging-port=0',
                                      '--ozone-platform=wayland', '--enable-wayland-ime', '--wayland-text-input-version=3',
                                      '--app='+page.as_uri()], frontend+'.log', client_env)

                try:
                    window = wait_for(lambda: [w for w in inner_ipc('windows') if w.get('pid') == fixture.pid],
                                      fixture, frontend+' window')[0]
                    wid = window['id']
                    action('focus-window', '--id', str(wid))
                    action('move-window-to-floating', '--id', str(wid))
                    action('set-window-width', '--id', str(wid), '640')
                    action('set-window-height', '--id', str(wid), '300')
                    action('move-floating-window', '--id', str(wid), '-x', '80', '-y', '70')
                    subprocess.run(['fcitx5-remote', '-s', 'tipe'], env=env, check=True)
                    subprocess.run(['fcitx5-remote', '-o'], env=env, check=True)
                    if frontend == 'foot':
                        wait_for(lambda: Path(server).exists(), fixture, 'foot reader')
                    elif frontend == 'browser':
                        wait_for(lambda: (profile/'DevToolsActivePort').exists(), fixture, 'browser CDP')
                    else:
                        module = ((Path(env['GTK_PATH'])/'4.0.0/immodules/libim-fcitx5.so') if frontend == 'gtk' else
                                  Path(env['QT_PLUGIN_PATH'])/'platforminputcontexts/libfcitx5platforminputcontextplugin.so')
                        assert str(module.resolve()) in Path(f'/proc/{fixture.pid}/maps').read_text(), module
                        runtime['modules'][frontend] = dict(path=str(module.resolve()),
                            sha256=hashlib.sha256(module.read_bytes()).hexdigest())

                    def guard():
                        assert all(p.poll() is None for p in [outer, compositor, daemon, fixture])
                        outer_windows = [n for n in nodes(outer_ipc('-t', 'get_tree')) if n.get('id') == outer_id]
                        assert len(outer_windows) == 1 and outer_windows[0].get('focused'), 'Outer focus changed'
                        inner = inner_ipc('focused-window')
                        assert inner and inner['id'] == wid and inner['pid'] == fixture.pid, 'Inner focus changed'

                    def keys(*keys):
                        def event(code, state):
                            guard()
                            assert keyboard.poll() is None, 'Private keyboard exited'
                            keyboard.stdin.write(f'{code} {state}\n')
                            keyboard.stdin.flush()
                            keyboard_reply('ok')
                        def tap(code):
                            event(code, 1)
                            try:
                                time.sleep(.025)
                            finally:
                                event(code, 0)
                            time.sleep(.075)
                        index = 0
                        while index < len(keys):
                            item = keys[index]
                            index += 1
                            if item == '-k':
                                tap(special[keys[index]])
                                index += 1
                            elif item in ['-M', '-m']:
                                event(special[keys[index]], int(item == '-M'))
                                index += 1
                            else:
                                for char in item:
                                    tap(letters[char])
                        time.sleep(.25)

                    def cdp(expression):
                        return json.loads(subprocess.check_output(['node', str(ROOT/'scripts/browser-cdp.mjs'),
                                           str(profile), expression], env=env, text=True))

                    def value():
                        if frontend == 'browser':
                            return cdp('document.querySelector("input").value')
                        if frontend == 'foot':
                            return json.loads((out/'foot-text.json').read_text())
                        rows = [json.loads(s) for s in client_out.read_text().splitlines() if s.startswith('{')]
                        texts = [r['value'] for r in rows if r.get('event') == 'text' and 'value' in r]
                        return texts[-1] if texts else ''

                    def clear():
                        keys('-k', 'Escape')
                        if frontend == 'foot':
                            control.sendto(b'top-left', server)
                            assert control.recv(10) == b'ok'
                        else:
                            keys('-M', 'ctrl', 'a', '-m', 'ctrl', '-k', 'BackSpace')
                        assert value() == '', value()

                    def shot(tag, scale):
                        guard()
                        bounds, selection = native.popup(out/(tag+'.png'), lambda filename:
                            subprocess.run(['grim', '-o', 'winit', str(filename)], env=env, check=True))
                        text = (out/(frontend+'.log')).read_text()
                        pattern = (r'zwp_text_input_v3#\d+\.set_cursor_rectangle\((-?\d+), (-?\d+), (\d+), (\d+)\)'
                                   if frontend in ['foot', 'browser'] else
                                   r'xdg_positioner#\d+\.set_anchor_rect\((-?\d+), (-?\d+), (\d+), (\d+)\)')
                        matches = re.findall(pattern, text)
                        assert matches, 'No client-supplied caret/anchor rectangle'
                        rectangle = [int(v) for v in matches[-1]]
                        layout = inner_ipc('focused-window')['layout']
                        ox, oy = layout['tile_pos_in_workspace_view']
                        dx, dy = layout['window_offset_in_tile']
                        cx, cy, cw, ch = rectangle
                        caret = [ox+dx+cx, oy+dy+cy, ox+dx+cx+cw, oy+dy+cy+ch]
                        pane = [v/scale for v in bounds]
                        assert pane[0]-24 <= caret[0] <= pane[2]+24, ('horizontal anchor', pane, caret)
                        assert min(abs(pane[1]-caret[3]), abs(pane[3]-caret[1])) <= 40, ('vertical anchor', pane, caret)
                        return dict(bounds=bounds, selection=selection, protocol_rectangle=rectangle, caret=caret,
                                    width=(bounds[2]-bounds[0])/scale, height=(bounds[3]-bounds[1])/scale)

                    if args.matrix == 'arrows':
                        from arrow_cases import CASES
                        for name, sequence, expected in CASES:
                            clear()
                            keys(*sequence)
                            actual = value()
                            result = dict(frontend=frontend, name=name, expected=expected,
                                          actual=actual, passed=actual == expected)
                            results.append(result)
                            (out/'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
                            assert result['passed'], result
                            print(frontend, name, 'PASS', flush=True)
                        clear()
                        keys('s')
                        compact = shot(frontend+'-compact', 1)
                        keys('-k', 'Right')
                        right = shot(frontend+'-right', 1)
                        keys('-k', 'Left')
                        left = shot(frontend+'-left', 1)
                        assert right['selection'][0] > left['selection'][0], (right, left)
                        assert abs(right['selection'][1] - left['selection'][1]) <= 2, (right, left)
                        results.append(dict(frontend=frontend, name='Visible horizontal highlight movement',
                                            compact=compact, right=right, left=left, passed=True))
                        (out/'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
                        continue

                    for scale in args.scales:
                        for spelling, expected in [('nihao', '你好'), ('zhonghuarenmingongheguo', '中华人民共和国')]:
                            for method in ['keyboard', 'mouse']:
                                clear()
                                subprocess.run(['niri', 'msg', 'output', 'winit', 'scale', '1'], env=env, check=True,
                                               stdout=subprocess.DEVNULL)
                                time.sleep(.3)
                                keys(spelling)
                                tag = f'{frontend}-{scale}-{len(spelling)}-{method}'
                                initial = shot(tag+'-initial', 1)
                                subprocess.run(['niri', 'msg', 'output', 'winit', 'scale', str(scale)], env=env, check=True,
                                               stdout=subprocess.DEVNULL)
                                time.sleep(.5)
                                output = inner_ipc('outputs')['winit']
                                assert abs(output['logical']['scale']-scale) < .01, output
                                compact = shot(tag+'-compact', scale)
                                keys('-k', 'Down', '-k', 'Home')
                                expanded = shot(tag+'-expanded', scale)
                                assert abs(compact['width']-expanded['width']) <= 5, (compact, expanded)
                                assert abs(compact['width']-initial['width']) <= 5, (initial, compact)
                                assert expanded['height'] > compact['height'], (compact, expanded)
                                if method == 'keyboard':
                                    keys('-k', 'space')
                                else:
                                    guard()
                                    x, y = expanded['selection']
                                    subprocess.run([str(pointer), str(int(x/scale)), str(int(y/scale)),
                                                    str(output['logical']['width']), str(output['logical']['height']),
                                                    'click'], env=env, check=True, timeout=5)
                                    time.sleep(.3)
                                actual = value()
                                assert actual == expected, (tag, actual, expected)
                                results.append(dict(frontend=frontend, scale=scale, spelling=spelling, method=method,
                                                    initial=initial, compact=compact, expanded=expanded,
                                                    actual=actual, passed=True))
                                (out/'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
                                print(tag, 'PASS', flush=True)
                finally:
                    if control is not None:
                        control.close()
                    if fixture.poll() is None:
                        fixture.terminate()
                        try:
                            fixture.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            fixture.kill()
                            fixture.wait()
            runtime['complete'] = True
            runtime['frontends'] = args.frontends
            runtime['scales'] = args.scales
            runtime['passed'] = len(results)
            (out/'runtime.json').write_text(json.dumps(runtime, indent=2)+'\n')
            print('PASS isolated niri cases:', len(results), flush=True)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            for handle in files:
                handle.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
