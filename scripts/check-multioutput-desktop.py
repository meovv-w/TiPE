#!/usr/bin/env python3
"""Real GTK/Qt popup migration on private headless Mutter outputs.

This creates a private D-Bus session, HOME, runtime, state and compositor. All
keys/pointer events belong to that compositor's virtual seat, with a PID focus
check. It never calls niri, reads a physical keyboard, or changes live outputs.
Requires Mutter 18 introspection, PyGObject, Pillow, NumPy and built harnesses.
"""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def inside(out, private, real_home):
    import ctypes
    import hashlib
    import shutil
    import threading
    import time
    import traceback
    import gi
    gi.require_version('Meta', '18')
    gi.require_version('Clutter', '18')
    gi.require_version('Cogl', '18')
    gi.require_version('Mtk', '18')
    from gi.repository import Meta, Clutter, GLib, Gio, Mtk
    import numpy as np
    from PIL import Image, ImageDraw

    assert os.environ['DBUS_SESSION_BUS_ADDRESS'] != os.environ['TIPE_PARENT_BUS']
    assert Path(os.environ['XDG_RUNTIME_DIR']).is_relative_to(private)
    context = Meta.create_context('TiPE isolated multioutput regression')
    context.set_plugin_name('/usr/lib64/mutter-18/plugins/libdefault.so')
    context.configure(['tipe-multioutput', '--headless', '--virtual-monitor', '1920x1440',
                       '--virtual-monitor', '2400x1500', '--wayland-display', 'tipe-multioutput'])
    context.setup()
    context.start()
    context.notify_ready()
    loop = GLib.MainLoop()
    stage = context.get_backend().get_stage()
    display = context.get_display()
    seat = stage.get_context().get_backend().get_default_seat()
    keyboard = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE)
    pointer = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE)
    reader = ctypes.CDLL('/usr/lib64/mutter-18/libmutter-cogl-18.so.0').cogl_framebuffer_read_pixels
    reader.argtypes = [ctypes.c_void_p] + [ctypes.c_int]*5 + [ctypes.c_void_p]
    reader.restype = ctypes.c_int
    error = []
    (out/'results.json').write_text('[]\n')
    version=subprocess.check_output(['mutter','--version'],text=True).strip()
    runtime = dict(compositor=version+' headless', private_bus=True,
                   physical_input=False, client_modules={}, complete=False)
    (out/'runtime.json').write_text(json.dumps(runtime,indent=2)+'\n')

    def on_main(fn):
        done = threading.Event()
        result = []
        def invoke():
            try: result.append((True, fn()))
            except BaseException as exc: result.append((False, exc))
            done.set()
            return False
        GLib.idle_add(invoke)
        if not done.wait(15): raise TimeoutError('Isolated compositor stopped responding')
        ok, value = result[0]
        if not ok: raise value
        return value

    def work():
        processes, files, results = [], [], []
        try:
            # No private fcitx query before this process: avoid D-Bus activation.
            data = private/'data/fcitx5'
            (data/'inputmethod').mkdir(parents=True)
            (data/'addon').mkdir()
            for source, dest in [('tipe.conf', 'inputmethod/tipe.conf'),
                                 ('tipe-addon.conf', 'addon/tipe.conf')]:
                shutil.copy2(ROOT/'packaging'/source, data/dest)
            installed = real_home/'.config/fcitx5/addon'
            for name in ['libtipe.so', 'libclassicui.so', 'libwaylandim.so']:
                assert (installed/name).is_file(), f'Missing installed module: {name}'
                (data/'addon'/name).symlink_to(installed/name)
            config = private/'config/fcitx5';config.mkdir(parents=True)
            (config/'profile').write_text('[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=tipe\n\n'
                                         '[Groups/0/Items/0]\nName=tipe\nLayout=\n\n[GroupOrder]\n0=Default\n')
            env = dict(os.environ, WAYLAND_DISPLAY='tipe-multioutput',
                       FCITX_ADDON_DIRS=f'{data}/addon:/usr/lib64/fcitx5',
                       LD_LIBRARY_PATH=f'{installed}:/usr/lib64/mutter-18',
                       GTK_PATH=os.environ.get('TIPE_MULTI_GTK_PATH',str(real_home/'.local/share/tipe/gtk/lib/gtk-4.0')),
                       QT_PLUGIN_PATH=str(real_home/'.local/share/tipe/qt/lib/qt6/plugins'),
                       GTK_IM_MODULE='fcitx', QT_IM_MODULE='fcitx', GDK_BACKEND='wayland',
                       QT_QPA_PLATFORM='wayland', TIPE_HARNESS_TIMEOUT_MS='600000',
                       GTK_A11Y='none', NO_AT_BRIDGE='1',
                       TIPE_HARNESS_EDGE='1', TIPE_CLASSIC_GRID='1', TIPE_CLASSIC_LAYOUT='horizontal',
                       TIPE_CLOUD_ENABLED='0', TIPE_CLOUD_CONSENT='0', TIPE_UI_X11='0',
                       WAYLAND_DEBUG='client')
            daemon_log = (out/'fcitx.log').open('w');files.append(daemon_log)
            daemon = subprocess.Popen(['fcitx5', '--disable', 'notificationitem'], env=env,
                                      stdout=daemon_log, stderr=daemon_log)
            processes.append(daemon)
            for _ in range(100):
                assert daemon.poll() is None, 'Private Fcitx exited'
                if 'Loaded addon tipe' in (out/'fcitx.log').read_text():break
                time.sleep(.1)
            else: raise AssertionError('Private TiPE did not load')
            bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            def call(method, variant=None):
                return bus.call_sync('org.gnome.Mutter.DisplayConfig',
                    '/org/gnome/Mutter/DisplayConfig', 'org.gnome.Mutter.DisplayConfig',
                    method, variant, None, Gio.DBusCallFlags.NONE, 10000, None).unpack()
            def monitors(scale):
                state = call('GetCurrentState')
                modes = {m[0][0]: m[1][0] for m in state[1]}
                assert all(n in modes for n in ['Meta-0','Meta-1']), state
                assert scale in modes['Meta-1'][5], (scale,modes)
                spec = [(0,0,1.0,0,True,[('Meta-0',modes['Meta-0'][0],{})]),
                        (1920,0,float(scale),0,False,[('Meta-1',modes['Meta-1'][0],{})])]
                call('ApplyMonitorsConfig',GLib.Variant('(uua(iiduba(ssa{sv}))a{sv})',
                                                      (state[0],1,spec,{})))
                time.sleep(.6)
                current = call('GetCurrentState')
                (out/'monitors.json').write_text(json.dumps(current,indent=2)+'\n')
                assert {m[2] for m in current[2]} == {1.0,float(scale)}, current
                return current
            monitors(2.0)
            keymap = dict(zip('qwertyuiop',range(16,26)))
            keymap.update(zip('asdfghjkl',range(30,39)))
            keymap.update(zip('zxcvbnm',range(44,51)))
            keymap.update(Escape=1,Down=108,Home=102,Space=57,BackSpace=14,Ctrl=29)
            def owned(pid):
                candidates = [w for w in display.list_all_windows()
                              if w.get_pid()==pid and w.get_window_type()==Meta.WindowType.NORMAL]
                assert len(candidates)==1, ('Owned window missing',pid,len(candidates))
                return candidates[0]
            def guard(pid):
                focus = display.get_focus_window()
                assert focus is not None and focus.get_pid()==pid, 'Isolated fixture lost focus'
            def tap(pid,key):
                def event(state):
                    guard(pid)
                    keyboard.notify_key(GLib.get_monotonic_time(),keymap[key],state)
                on_main(lambda:event(Clutter.KeyState.PRESSED));time.sleep(.025)
                on_main(lambda:event(Clutter.KeyState.RELEASED));time.sleep(.09)
            def keys(pid,text):
                for char in text:tap(pid,char)
                time.sleep(.2)
            def clear(pid):
                tap(pid,'Escape')
                def ctrl(state):
                    guard(pid);keyboard.notify_key(GLib.get_monotonic_time(),29,state)
                on_main(lambda:ctrl(Clutter.KeyState.PRESSED))
                try:tap(pid,'a')
                finally:on_main(lambda:ctrl(Clutter.KeyState.RELEASED))
                tap(pid,'BackSpace');time.sleep(.2)
            def move(pid,number):
                def change():
                    window=owned(pid)
                    monitors={display.get_monitor_geometry(i).x:i for i in range(display.get_n_monitors())}
                    index=monitors[0 if number==0 else 1920]
                    rect=display.get_monitor_geometry(index)
                    window.move_to_monitor(index)
                    window.move_resize_frame(False,rect.x+rect.width-660,rect.y+rect.height-200,640,180)
                    window.activate(display.get_current_time_roundtrip())
                on_main(change);time.sleep(.7)
                on_main(lambda:guard(pid))
            def capture(pid,name,number):
                def read():
                    guard(pid)
                    def layout(view):
                        rect=Mtk.Rectangle()
                        view.get_layout(rect)
                        return rect
                    views=[v for v in stage.peek_stage_views() if layout(v).x==(0 if number==0 else 1920)]
                    assert len(views)==1
                    view=views[0];fb=view.get_framebuffer();w,h=fb.get_width(),fb.get_height()
                    pixels=ctypes.create_string_buffer(w*h*4)
                    assert reader(hash(fb),0,0,w,h,19,pixels)
                    rect=layout(view)
                    return Image.frombytes('RGBA',(w,h),pixels.raw),[rect.x,rect.y,rect.width,rect.height],view.get_scale()
                im,viewrect,scale=on_main(read)
                im.save(out/(name+'.png'))
                a=np.array(im.convert('RGB'))
                blue=(a[:,:,0]==10)&(a[:,:,1]==132)&(a[:,:,2]==255)
                yy,xx=np.where(blue)
                assert len(yy)>50, ('No candidate selection',name)
                selected=[int(xx.min()),int(yy.min()),int(xx.max())+1,int(yy.max())+1]
                # Find the pane's dark interior enclosing the blue cell. White
                # entry and desktop background are disconnected by its border.
                dark=((a[:,:,0]>=22)&(a[:,:,0]<=40)&(a[:,:,1]>=22)&(a[:,:,1]<=40)&
                      (a[:,:,2]>=24)&(a[:,:,2]<=44))
                x=int((selected[0]+selected[2])/2);y=int((selected[1]+selected[3])/2)
                pane=None
                for sx,sy in [(selected[0]+10,selected[1]-5),(selected[0]-4,y),(selected[2]+5,y)]:
                    if not(0<=sx<im.width and 0<=sy<im.height and dark[sy,sx]):continue
                    mask=Image.fromarray((dark*255).astype('uint8')).copy();ImageDraw.floodfill(mask,(sx,sy),128,thresh=0)
                    ys,xs=np.where(np.array(mask)==128)
                    if not len(ys):continue
                    # Fractional AA can make the outermost border pixel part
                    # of the dark interior. Bound reconstruction is accurate
                    # to one physical pixel, not an extra rounded-up pixel.
                    border=max(1,int(scale))
                    b=[int(xs.min())-border,int(ys.min())-border,
                       int(xs.max())+border+1,int(ys.max())+border+1]
                    if b[0]<selected[0] and b[2]>selected[2] and b[1]<selected[1] and b[3]>selected[3]:
                        pane=b;break
                assert pane is not None,('Pane border undetectable',name,selected)
                assert 0<=pane[0]<pane[2]<=im.width and 0<=pane[1]<pane[3]<=im.height,(name,pane,im.size)
                toolkit='gtk' if name.startswith('gtk-') else 'qt'
                anchors=re.findall(r'xdg_positioner#\d+\.set_anchor_rect\((-?\d+), (-?\d+), (\d+), (\d+)\)',
                                   (out/f'{toolkit}.err').read_text())
                assert anchors, 'Client did not supply an xdg_popup anchor'
                anchor=[int(v) for v in anchors[-1]]
                def origin():
                    rect=owned(pid).get_buffer_rect()
                    return rect.x,rect.y
                px,py=on_main(origin)
                caret=[px+anchor[0],py+anchor[1],px+anchor[0]+anchor[2],py+anchor[1]+anchor[3]]
                logical=[pane[0]/scale+viewrect[0],pane[1]/scale+viewrect[1],
                         pane[2]/scale+viewrect[0],pane[3]/scale+viewrect[1]]
                assert logical[0]-32<=caret[0]<=logical[2]+32,(name,'horizontal anchor',logical,caret)
                assert min(abs(logical[1]-caret[3]),abs(logical[3]-caret[1]))<=48,(name,'vertical anchor',logical,caret)
                return dict(pane=pane,selection=selected,view=viewrect,scale=scale,
                            physical_size=list(im.size),protocol_anchor=anchor,caret=caret)
            def click(pid,shot):
                x1,y1,x2,y2=shot['selection'];ox,oy,_,_=shot['view'];s=shot['scale']
                x,y=ox+(x1+x2)/2/s,oy+(y1+y2)/2/s
                def motion():
                    guard(pid);pointer.notify_absolute_motion(GLib.get_monotonic_time(),x,y)
                on_main(motion);time.sleep(.25)
                for state in [Clutter.ButtonState.PRESSED,Clutter.ButtonState.RELEASED]:
                    on_main(lambda: (guard(pid),pointer.notify_button(GLib.get_monotonic_time(),1,state)))
                    time.sleep(.1)
                time.sleep(.3)
            for toolkit in ['gtk','qt']:
                client_out=out/f'{toolkit}.jsonl';client_err=out/f'{toolkit}.err'
                fo=client_out.open('w');fe=client_err.open('w');files.extend([fo,fe])
                fixture=subprocess.Popen([str(ROOT/f'build/tipe-{toolkit}-harness')],env=env,stdout=fo,stderr=fe)
                processes.append(fixture)
                for _ in range(100):
                    assert fixture.poll() is None,'Owned harness exited'
                    if on_main(lambda:any(w.get_pid()==fixture.pid for w in display.list_all_windows())):break
                    time.sleep(.1)
                else:raise AssertionError('Owned harness window missing')
                move(fixture.pid,0)
                module = (Path(env['GTK_PATH'])/'4.0.0/immodules/libim-fcitx5.so' if toolkit=='gtk'
                          else Path(env['QT_PLUGIN_PATH'])/'platforminputcontexts/libfcitx5platforminputcontextplugin.so').resolve()
                maps = Path(f'/proc/{fixture.pid}/maps').read_text()
                assert str(module) in maps, ('Expected toolkit overlay is not mapped',module)
                runtime['client_modules'][toolkit] = dict(path=str(module),inode=module.stat().st_ino,
                    sha256=hashlib.sha256(module.read_bytes()).hexdigest())
                (out/'runtime.json').write_text(json.dumps(runtime,indent=2)+'\n')
                for _ in range(30):
                    r=subprocess.run(['fcitx5-remote','-s','tipe'],env=env,capture_output=True)
                    if r.returncode==0:break
                    time.sleep(.1)
                subprocess.run(['fcitx5-remote','-o'],env=env,check=True)
                def value():
                    rows=[json.loads(s) for s in client_out.read_text().splitlines() if s.startswith('{')]
                    values=[r['value'] for r in rows if r.get('event')=='text' and 'value' in r]
                    return values[-1] if values else ''
                for target_scale in [2.0,1.25,1.5]:
                    monitors(target_scale)
                    for spelling,expected in [('nihao','你好'),('zhonghuarenmingongheguo','中华人民共和国')]:
                        for method in ['keyboard','mouse']:
                            move(fixture.pid,0);clear(fixture.pid);assert value()==''
                            keys(fixture.pid,spelling)
                            prefix=f'{toolkit}-{target_scale}-{len(spelling)}-{method}'
                            first=capture(fixture.pid,prefix+'-source',0)
                            move(fixture.pid,1)
                            compact=capture(fixture.pid,prefix+'-target-compact',1)
                            tap(fixture.pid,'Down');tap(fixture.pid,'Home');time.sleep(.25)
                            expanded=capture(fixture.pid,prefix+'-target-expanded',1)
                            width=lambda s:(s['pane'][2]-s['pane'][0])/s['scale']
                            assert abs(width(compact)-width(expanded))<=4,(prefix,compact,expanded)
                            assert abs(width(first)-width(compact))<=4,(prefix,'logical width changed',first,compact)
                            assert expanded['pane'][3]-expanded['pane'][1]>compact['pane'][3]-compact['pane'][1]
                            assert abs(expanded['scale']-target_scale)<.01,(prefix,'unexpected render scale',expanded)
                            move(fixture.pid,0)
                            returned=capture(fixture.pid,prefix+'-return-expanded',0)
                            assert abs(width(returned)-width(expanded))<=4,(prefix,'return width changed',returned,expanded)
                            move(fixture.pid,1)
                            expanded=capture(fixture.pid,prefix+'-target-again-expanded',1)
                            if method=='keyboard':tap(fixture.pid,'Space')
                            else:click(fixture.pid,expanded)
                            time.sleep(.3);actual=value();assert actual==expected,(prefix,actual,expected)
                            result=dict(toolkit=toolkit,scale=target_scale,spelling=spelling,method=method,
                                        source=first,compact=compact,expanded=expanded,returned=returned,
                                        expected=expected,actual=actual,passed=True)
                            results.append(result)
                            (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
                            print(prefix,'PASS',flush=True)
                fixture.terminate();fixture.wait(timeout=5);processes.remove(fixture)
            print('PASS isolated mixed-scale popup migrations:',len(results),flush=True)
            runtime['complete']=True
            (out/'runtime.json').write_text(json.dumps(runtime,indent=2)+'\n')
        except BaseException:
            error.append(traceback.format_exc());print(error[-1],file=sys.stderr,flush=True)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:process.wait(timeout=5)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
            for f in files:f.close()
            GLib.idle_add(lambda:(loop.quit(),False)[1])
    worker=threading.Thread(target=work,name='isolated-popup-check');worker.start()
    loop.run();worker.join()
    # Let process teardown own Mutter's GObjects; explicit destroy while Python
    # wrappers remain can double-dispose the introspected renderer objects.
    return 1 if error else 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inside',nargs=3,metavar=('OUTPUT','PRIVATE','REAL_HOME'))
    parser.add_argument('--output',type=Path,default=ROOT/'build/multioutput-regression')
    args=parser.parse_args()
    if args.inside:
        return inside(*(Path(p) for p in args.inside))
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tipe-multioutput-') as temporary:
        private=Path(temporary);env=dict(os.environ)
        for key,dirname in [('HOME','home'),('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),
                            ('XDG_STATE_HOME','state'),('XDG_RUNTIME_DIR','runtime')]:
            p=private/dirname;p.mkdir(mode=0o700);env[key]=str(p)
        for key in ['DISPLAY','WAYLAND_DISPLAY','XAUTHORITY','NIRI_SOCKET','SESSION_MANAGER']:
            env.pop(key,None)
        env['GI_TYPELIB_PATH']='/usr/lib64/mutter-18'
        env['LD_LIBRARY_PATH']='/usr/lib64/mutter-18'
        env['TIPE_PARENT_BUS']=os.environ.get('DBUS_SESSION_BUS_ADDRESS','')
        result=subprocess.run(['dbus-run-session','--',sys.executable,str(Path(__file__).resolve()),
                               '--inside',str(out),str(private),str(Path.home())],env=env)
        return result.returncode


if __name__=='__main__':sys.exit(main())
