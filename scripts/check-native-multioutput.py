#!/usr/bin/env python3
"""Actual input-method-v2 popups on a private, headless dual-output Sway.

Uses installed TiPE modules and real foot text-input-v3 events. No live desktop
outputs, profile, learning data or physical keyboard devices are touched.
Sway/wlroots may be extracted locally; set TIPE_TEST_SWAY_ROOT to that root.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import socket
import subprocess
import sys
import tempfile
import time
from desktop_pointer import prepare_pointer

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('native_popup', ROOT/'scripts/check-native-popup-desktop.py')
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frontend',choices=['foot','wine'],default='foot')
    args = parser.parse_args()
    wine_mode = args.frontend=='wine'
    if os.environ.get('TIPE_PRIVATE_NATIVE_SESSION') != '1':
        # Do not activate desktop portals/keyrings from the user's service
        # directories. The fixture only needs a bus on which Fcitx can own its
        # name; it needs no external D-Bus services.
        with tempfile.TemporaryDirectory(prefix='tipe-private-bus-') as bus_directory:
            bus_config=Path(bus_directory)/'session.conf'
            bus_config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>'
                                  '<auth>EXTERNAL</auth><policy context="default">'
                                  '<allow send_destination="*"/><allow receive_sender="*"/>'
                                  '<allow own="*"/><allow user="*"/></policy></busconfig>')
            return subprocess.call(['dbus-run-session','--config-file',str(bus_config),'--',sys.executable,
                                    str(Path(__file__).resolve()),*sys.argv[1:]],
                                   env=dict(os.environ,TIPE_PRIVATE_NATIVE_SESSION='1'))
    real_home = Path.home()
    sway_root = Path(os.environ.get('TIPE_TEST_SWAY_ROOT', ROOT/'build/sway-private/root')).resolve()
    sway = str(sway_root/'usr/bin/sway') if sway_root.exists() else shutil.which('sway')
    swaymsg = str(sway_root/'usr/bin/swaymsg') if sway_root.exists() else shutil.which('swaymsg')
    assert sway and swaymsg, 'Install Sway or provide an extracted TIPE_TEST_SWAY_ROOT'
    installed = real_home/'.config/fcitx5/addon'
    pointer = prepare_pointer()
    out = ROOT/('build/xim-multioutput-regression' if wine_mode else 'build/native-multioutput-regression')
    out.mkdir(parents=True, exist_ok=True)
    processes, files, results = [], [], []
    control = None
    with tempfile.TemporaryDirectory(prefix='tipe-native-multi-') as temporary:
        private = Path(temporary)
        env = dict(os.environ, WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='2', WLR_RENDERER='pixman')
        for name, directory in [('HOME','home'), ('XDG_RUNTIME_DIR','runtime'), ('XDG_CONFIG_HOME','config'),
                                ('XDG_STATE_HOME','state'), ('XDG_DATA_HOME','data'), ('XDG_CACHE_HOME','cache')]:
            target = private/directory
            target.mkdir(mode=0o700)
            env[name] = str(target)
        for name in ['DISPLAY','WAYLAND_DISPLAY','SWAYSOCK','NIRI_SOCKET','GTK_IM_MODULE','QT_IM_MODULE']:
            env.pop(name, None)
        env['LD_LIBRARY_PATH'] = f'{installed}:{sway_root}/usr/lib64'
        config = private/'sway.conf'
        config.write_text('output HEADLESS-1 resolution 1920x1440 position 0,0 scale 1\n'
                          'output HEADLESS-2 resolution 2400x1500 position 1920,0 scale 2\n'
                          f'xwayland {"enable" if wine_mode else "disable"}\n'
                          'focus_follows_mouse no\nseat seat0 fallback true\n')

        def launch(command, logfile, **kwargs):
            handle = (out/logfile).open('w')
            files.append(handle)
            process = subprocess.Popen(command, env=env, stdout=handle, stderr=handle, **kwargs)
            processes.append(process)
            return process

        try:
            compositor = launch([sway,'--unsupported-gpu','-c',str(config)], 'sway.log')
            for _ in range(100):
                assert compositor.poll() is None, 'Private Sway exited'
                sockets = list((private/'runtime').glob('sway-ipc.*.sock'))
                displays = [p for p in (private/'runtime').glob('wayland-*') if not p.name.endswith('.lock')]
                if sockets and displays: break
                time.sleep(.1)
            assert sockets and displays
            env.update(WAYLAND_DISPLAY=displays[0].name, SWAYSOCK=str(sockets[0]))

            def ipc(*args):
                return json.loads(subprocess.check_output([swaymsg,'-r',*args], env=env, text=True))

            def command(text):
                response = ipc(text)
                assert all(row.get('success') for row in response), (text,response)

            if wine_mode:
                # Ask only our private compositor for its child DISPLAY. No
                # session-global environment import or live X server is used.
                export = private/'display.py'
                export.write_text('import os,pathlib;pathlib.Path('+repr(str(private/'display'))+
                                  ').write_text(os.environ.get("DISPLAY",""))')
                command('exec '+shlex.join([sys.executable,str(export)]))
                for _ in range(100):
                    if (private/'display').exists():break
                    time.sleep(.1)
                display=(private/'display').read_text()
                assert display.startswith(':'), ('No private Xwayland display',display)
                env.update(DISPLAY=display,WINEPREFIX=str(private/'wine-prefix'),WINEDEBUG='-all',
                           XMODIFIERS='@im=fcitx',WINEDLLOVERRIDES='winemenubuilder.exe=d',TIPE_HARNESS_CARET='1')
                for key,name,value in [(r'HKCU\Software\Wine\Drivers','Graphics','x11'),
                                       (r'HKCU\Software\Wine\X11 Driver','InputStyle','overthespot')]:
                    subprocess.run(['wine','reg','add',key,'/v',name,'/d',value,'/f'],env=env,
                                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True,timeout=90)

            fcitx_config = private/'config/fcitx5'
            fcitx_config.mkdir()
            (fcitx_config/'profile').write_text('[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=tipe\n'
                                               '\n[Groups/0/Items/0]\nName=tipe\nLayout=\n\n[GroupOrder]\n0=Default\n')
            data = private/'data/fcitx5'
            (data/'inputmethod').mkdir(parents=True)
            (data/'addon').mkdir()
            for source,dest in [('tipe.conf','inputmethod/tipe.conf'),('tipe-addon.conf','addon/tipe.conf')]:
                shutil.copy2(ROOT/'packaging'/source,data/dest)
            modules = ['libtipe.so','libclassicui.so','libwaylandim.so']
            module_paths={name:(Path(os.environ['TIPE_TEST_CLASSICUI_SO']).resolve()
                               if name=='libclassicui.so' and os.environ.get('TIPE_TEST_CLASSICUI_SO') else installed/name)
                          for name in modules}
            for name in modules:
                assert module_paths[name].is_file(), name
                (data/'addon'/name).symlink_to(module_paths[name])
            env.update(FCITX_ADDON_DIRS=f'{data}/addon:/usr/lib64/fcitx5', TIPE_CLASSIC_GRID='1',
                       TIPE_CLASSIC_LAYOUT='horizontal', TIPE_UI_X11='0', TIPE_CLOUD_ENABLED='0', WAYLAND_DEBUG='client')
            # Keep a real virtual keyboard/keymap alive before clients connect.
            # All input belongs to this private Wayland socket, never evdev.
            launch(['wtype','-k','Escape','-s','1800000'], 'keyboard.log')
            time.sleep(.3)
            daemon = launch(['fcitx5','--disable','notificationitem'], 'fcitx.log')
            for _ in range(100):
                assert daemon.poll() is None, 'Private Fcitx exited'
                if 'Loaded addon tipe' in (out/'fcitx.log').read_text(): break
                time.sleep(.1)
            mapped = Path(f'/proc/{daemon.pid}/maps').read_text().splitlines()
            runtime = {}
            for name in modules:
                inode = module_paths[name].stat().st_ino
                assert any(len(p:=row.split(maxsplit=5))==6 and int(p[4])==inode for row in mapped), name
                runtime[name] = dict(path=str(module_paths[name]),inode=inode)
            (out/'runtime.json').write_text(json.dumps(runtime,indent=2)+'\n')
            reader = private/'reader.py'
            reader.write_text(native.READER)
            server = str(private/'reader.sock')
            if wine_mode:
                executable=out/'input.exe'
                subprocess.run(['x86_64-w64-mingw32-gcc',str(ROOT/'tests/real_wine_harness.c'),
                                '-municode','-mwindows','-o',str(executable)],check=True)
                (out/'text.utf8').unlink(missing_ok=True)
                (out/'reset.request').unlink(missing_ok=True)
                foot=launch(['wine',str(executable)],'wine.log',cwd=out)
            else:
                control = socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
                control.bind(str(private/'control.sock'))
                control.settimeout(5)
                foot = launch(['foot','--app-id=tipe-owned-native-multi','--title=TiPE owned native multi',
                               'python3','-u',str(reader),str(out/'text.json'),server], 'foot.log')

            def nodes(node):
                yield node
                for child in node.get('nodes',[])+node.get('floating_nodes',[]): yield from nodes(child)

            def owner():
                matches = [n for n in nodes(ipc('-t','get_tree')) if
                           (n.get('name')=='TiPE Wine isolated regression' if wine_mode else n.get('pid')==foot.pid)]
                assert len(matches)==1, ('Owned foot missing',matches)
                return matches[0]

            for _ in range(100):
                assert foot.poll() is None, 'Owned foot exited'
                if ((out/'text.utf8').exists() if wine_mode else Path(server).exists()): break
                time.sleep(.1)
            wid = owner()['id']
            command(f'[con_id={wid}] floating enable, border none, resize set 640 300, focus')
            time.sleep(.3)
            subprocess.run(['fcitx5-remote','-s','tipe'],env=env,check=True)
            subprocess.run(['fcitx5-remote','-o'],env=env,check=True)

            def guard():
                assert compositor.poll() is None and daemon.poll() is None and foot.poll() is None
                current = owner()
                assert current['id']==wid and current['focused'], 'Owned fixture lost focus'
                return current

            def keys(*args):
                guard()
                if wine_mode:
                    from x11_fixture_keys import send
                    send(env['DISPLAY'],guard()['window'],args)
                else:
                    subprocess.run(['wtype',*args],env=dict(env,WAYLAND_DEBUG=''),check=True,timeout=10)
                time.sleep(.2)

            def value():
                return (out/'text.utf8').read_text() if wine_mode else json.loads((out/'text.json').read_text())

            def move(output):
                guard()
                if wine_mode:
                    command(f'[con_id={wid}] move absolute position '+
                            ('120 100' if output=='HEADLESS-1' else '2020 100'))
                else:
                    command(f'[con_id={wid}] move container to output {output}, move position '+
                            ('120 100' if output=='HEADLESS-1' else '100 100')+', focus')
                time.sleep(.4)
                guard()

            def shot(tag, output):
                guard()
                outputs = {o['name']:o for o in ipc('-t','get_outputs')}
                monitor = outputs[output]
                frame = guard()['rect']
                assert (monitor['rect']['x'] <= frame['x'] < monitor['rect']['x']+monitor['rect']['width']), (output,frame,monitor)
                if wine_mode:
                    (out/(tag+'-contexts.txt')).write_bytes(subprocess.check_output(
                        ['busctl','--user','call','org.fcitx.Fcitx5','/controller','org.fcitx.Fcitx.Controller1','DebugInfo'],env=env))
                    (out/(tag+'-x11.json')).write_bytes(subprocess.check_output(
                        [sys.executable,str(ROOT/'scripts/x11-window-tree.py')],env=env))
                filename = out/(tag+'.png')
                capture=lambda p:subprocess.run(['grim','-o',output,str(p)],env=dict(env,WAYLAND_DEBUG=''),check=True)
                try:
                    bounds, selected = native.popup(filename,capture)
                except AssertionError:
                    if wine_mode:
                        tree=json.loads((out/(tag+'-x11.json')).read_text())
                        panes=[w for w in tree if w['title']=='Fcitx5 Input Window' and w['map_state']==2]
                        if len(panes)==1:
                            subprocess.run(['import','-window',panes[0]['id'],str(out/(tag+'-drawable.png'))],env=env,timeout=10,check=True)
                        keys('-k','Right','-k','Left')
                        capture(out/(tag+'-after-redraw.png'))
                    raise
                scale = monitor['scale']
                rect = monitor['rect']
                logical = [bounds[0]/scale+rect['x'],bounds[1]/scale+rect['y'],
                           bounds[2]/scale+rect['x'],bounds[3]/scale+rect['y']]
                frame = guard()['rect']
                if wine_mode:
                    observation=json.loads((out/'caret.json').read_text())
                    assert observation['valid'],observation
                    caret=observation['screen_rect']
                    rectangle=observation['client_rect']
                else:
                    matches = re.findall(r'zwp_text_input_v3#\d+\.set_cursor_rectangle\((-?\d+), (-?\d+), (\d+), (\d+)\)',
                                         (out/'foot.log').read_text())
                    assert matches, 'No client text rectangle'
                    cx,cy,cw,ch = [int(v) for v in matches[-1]]
                    caret = [frame['x']+cx,frame['y']+cy,frame['x']+cx+cw,frame['y']+cy+ch]
                    rectangle=[cx,cy,cw,ch]
                assert logical[0]-24 <= caret[0] <= logical[2]+24, ('horizontal anchor',logical,caret)
                assert min(abs(logical[1]-caret[3]),abs(logical[3]-caret[1]))<=40, ('vertical anchor',logical,caret)
                return dict(output=output,scale=scale,bounds=bounds,logical=logical,selection=list(selected),
                            protocol_rectangle=rectangle,caret=caret)

            for scale in [2,1.25,1.5]:
                command(f'output HEADLESS-2 scale {scale}')
                time.sleep(.4)
                for spelling, expected in [('nihao','你好'),('zhonghuarenmingongheguo','中华人民共和国')]:
                    for method in ['keyboard','mouse']:
                        keys('-k','Escape')
                        move('HEADLESS-1')
                        if wine_mode:
                            request=out/'reset.request'
                            request.touch()
                            for _ in range(50):
                                if not request.exists() and value()=='':break
                                time.sleep(.05)
                        else:
                            control.sendto(b'top-left',server)
                            assert control.recv(10)==b'ok'
                        time.sleep(.2)
                        assert value()==''
                        tag=f'{scale}-{len(spelling)}-{method}'
                        keys(spelling)
                        compact1=shot(tag+'-compact1','HEADLESS-1')
                        move('HEADLESS-2')
                        compact2=shot(tag+'-compact2','HEADLESS-2')
                        keys('-k','Down','-k','Home')
                        expanded2=shot(tag+'-expanded2','HEADLESS-2')
                        move('HEADLESS-1')
                        expanded1=shot(tag+'-expanded1','HEADLESS-1')
                        width=lambda s:s['logical'][2]-s['logical'][0]
                        height=lambda s:s['logical'][3]-s['logical'][1]
                        assert max(map(width,[compact1,compact2,expanded2,expanded1]))-min(map(width,[compact1,compact2,expanded2,expanded1]))<=5, tag
                        assert height(expanded1)>height(compact1) and height(expanded2)>height(compact2), tag
                        if method=='keyboard': keys('-k','space')
                        else:
                            guard()
                            x,y=expanded1['selection']
                            outputs=ipc('-t','get_outputs')
                            total_width=max(o['rect']['x']+o['rect']['width'] for o in outputs)
                            total_height=max(o['rect']['y']+o['rect']['height'] for o in outputs)
                            subprocess.run([str(pointer),str(x),str(y),str(total_width),str(total_height),'click'],env=dict(env,WAYLAND_DEBUG=''),check=True,timeout=5)
                            time.sleep(.3)
                        assert value()==expected, (tag,value(),expected)
                        result=dict(frontend=args.frontend,scale=scale,spelling=spelling,method=method,actual=value(),
                                    observations=[compact1,compact2,expanded2,expanded1],passed=True)
                        results.append(result)
                        (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
                        print(tag,'PASS',flush=True)
            print('PASS',args.frontend,'mixed-scale migrations:',len(results),flush=True)
        finally:
            if control is not None: control.close()
            if wine_mode and (private/'wine-prefix').exists():
                subprocess.run(['wineserver','-k'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill();process.wait()
            for handle in files: handle.close()
    return 0


if __name__=='__main__': sys.exit(main())
