#!/usr/bin/env python3
"""Native Wayland Chromium/foot popup corners; only owned windows get keys.

The compositor screenshot proves complete popup bounds. The client's own
text-input-v3 cursor rectangle proves the anchor; compositor IPC only moves
the newly created test window. Product positioning never uses that IPC.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time

import numpy as np
from PIL import Image, ImageDraw

from desktop_keys import send
from desktop_pointer import point

ROOT = Path(__file__).resolve().parent.parent


def windows():
    return json.loads(subprocess.check_output(['niri', 'msg', '-j', 'windows']))


def action(*args):
    subprocess.run(['niri', 'msg', 'action', *args], check=True)


def popup(path, capture=None):
    """Find the actual pane enclosing the single blue candidate selection."""
    if capture is None:
        subprocess.run(['grim', str(path)], check=True)
    else:
        capture(path)
    a = np.array(Image.open(path).convert('RGB'))
    blue = (a[:, :, 0] == 10) & (a[:, :, 1] == 132) & (a[:, :, 2] == 255)
    ys = np.where(blue.sum(axis=1) >= 12)[0]
    assert len(ys) >= 10, 'No visible candidate selection'
    assert len(np.split(ys, np.where(np.diff(ys) > 8)[0]+1)) == 1, 'Multiple candidate panes'
    y = int(ys[len(ys)//2])
    xs = np.where(blue[y])[0]
    run = max(np.split(xs, np.where(np.diff(xs) > 1)[0]+1), key=len)
    x = int(run[len(run)//2])
    # Background is translucent near RGB(26,26,28); the hairline boundary
    # separates it from client and desktop backgrounds. Find its connected
    # interior, rather than inferring the pane from a mapped surface alone.
    dark = ((a[:, :, 0] >= 22) & (a[:, :, 0] <= 40) &
            (a[:, :, 1] >= 22) & (a[:, :, 1] <= 40) &
            (a[:, :, 2] >= 24) & (a[:, :, 2] <= 44))
    bx, by = int(run[0]), int(ys[0])
    for sx, sy in [(bx+10, by-5), (bx-4, y), (int(run[-1])+5, y)]:
        if not (0 <= sx < a.shape[1] and 0 <= sy < a.shape[0] and dark[sy, sx]):
            continue
        mask = Image.fromarray((dark*255).astype('uint8')).copy()
        ImageDraw.floodfill(mask, (sx, sy), 128, thresh=0)
        yy, xx = np.where(np.array(mask) == 128)
        if not len(yy):
            continue
        bounds = [int(xx.min())-2, int(yy.min())-2, int(xx.max())+3, int(yy.max())+3]
        if bounds[0] < run[0] and bounds[2] > run[-1] and bounds[1] < ys[0] and bounds[3] > ys[-1]:
            assert 0 <= bounds[0] < bounds[2] <= a.shape[1], bounds
            assert 0 <= bounds[1] < bounds[3] <= a.shape[0], bounds
            return bounds, (x, y)
    raise AssertionError('Cannot establish complete candidate pane from screenshot')


READER = '''import codecs,json,os,pathlib,select,socket,sys,termios,tty
target=pathlib.Path(sys.argv[1]);server=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
server.bind(sys.argv[2]);original=termios.tcgetattr(0);tty.setraw(0)
decoder=codecs.getincrementaldecoder('utf-8')();text=''
def publish():
 temp=target.with_suffix('.tmp');temp.write_text(json.dumps(text,ensure_ascii=False));temp.replace(target)
publish()
try:
 while True:
  ready,_,_=select.select([0,server],[],[])
  if server in ready:
   data,peer=server.recvfrom(100);corner=data.decode();text='';decoder.reset();publish()
   columns,rows=os.get_terminal_size(0)
   col=columns if 'right' in corner else 1;row=rows if 'bottom' in corner else 1
   os.write(1,('\x1b[2J\x1b[%d;%dH'%(row,col)).encode());server.sendto(b'ok',peer)
  if 0 in ready:
   data=os.read(0,256)
   if not data:break
   text+=decoder.decode(data);publish()
finally:
 termios.tcsetattr(0,termios.TCSANOW,original);server.close()
'''


def run(name, out, output, results):
    directory = out/name
    directory.mkdir(parents=True, exist_ok=True)
    token = str(time.monotonic_ns())
    original = next((w['id'] for w in windows() if w['is_focused']), None)
    profile = directory/('profile-'+token)
    log = (directory/'client.log').open('w')
    process = None
    wid = None
    control = None
    with tempfile.TemporaryDirectory(prefix='tipe-native-popup-') as private:
        server = str(Path(private)/'reader.sock')
        if name == 'browser':
            page = directory/'input.html'
            page.write_text('''<!doctype html><meta charset=utf-8><title>TiPE owned native popup</title>
<style>body{background:white}input{position:fixed;width:88px;height:32px;font:22px sans-serif}</style>
<input autofocus><script>window.events=[];const e=document.querySelector('input');
for(const t of ['input','compositionstart','compositionupdate','compositionend'])
e.addEventListener(t,x=>events.push({type:t,data:x.data,value:e.value}));</script>''')
            command = [shutil.which('chromium-browser') or 'chromium',
                       '--user-data-dir='+str(profile), '--class=tipe-native-popup-'+token,
                       '--no-first-run', '--no-default-browser-check', '--disable-extensions',
                       '--disable-background-networking', '--remote-debugging-port=0',
                       '--ozone-platform=wayland', '--enable-wayland-ime',
                       '--wayland-text-input-version=3', '--app='+page.as_uri()]
        else:
            reader = directory/'reader.py';reader.write_text(READER)
            command = ['foot', '--app-id=tipe-native-popup-'+token,
                       '--title=TiPE owned native popup', 'python3', '-u', str(reader),
                       str(directory/'text.json'), server]
            control = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            control.bind(str(Path(private)/'controller.sock'));control.settimeout(5)
        try:
            process = subprocess.Popen(command, stdout=log, stderr=log, start_new_session=True,
                                       env=dict(os.environ, WAYLAND_DEBUG='client'))
            for _ in range(150):
                assert process.poll() is None, 'Owned fixture exited'
                matches = [w for w in windows() if w['pid'] == process.pid]
                ready = (profile/'DevToolsActivePort').exists() if name == 'browser' else Path(server).exists()
                if len(matches) == 1 and ready:break
                time.sleep(.1)
            assert len(matches) == 1 and ready, 'Owned native fixture missing'
            wid = matches[0]['id']
            action('focus-window', '--id', str(wid))
            action('move-window-to-floating', '--id', str(wid))
            for axis, size in [('width', 640), ('height', 300)]:
                action('set-window-'+axis, '--id', str(wid), str(size))
            subprocess.run(['fcitx5-remote', '-s', 'tipe'], check=True)
            subprocess.run(['fcitx5-remote', '-o'], check=True)

            def keys(*args):
                send(process.pid, args, window_id=wid);time.sleep(.2)

            def cdp(expression):
                return json.loads(subprocess.check_output(
                    ['node', str(ROOT/'scripts/browser-cdp.mjs'), str(profile), expression], text=True))

            def value():
                return (cdp('document.querySelector("input").value') if name == 'browser'
                        else json.loads((directory/'text.json').read_text()))

            sizes = {}
            for corner in ['top-left', 'top-right', 'bottom-left', 'bottom-right']:
                x = 12 if 'left' in corner else output['width']-652
                y = 12 if 'top' in corner else output['height']-312
                action('move-floating-window', '--id', str(wid), '-x', str(x), '-y', str(y))
                time.sleep(.3)
                for spelling, expected in [('nihao', '你好'), ('zhonghuarenmingongheguo', '中华人民共和国')]:
                    records = []
                    for method in ['keyboard', 'mouse']:
                        if name == 'browser':
                            keys('-k', 'Escape', '-M', 'ctrl', 'a', '-m', 'ctrl', '-k', 'BackSpace')
                            side = ('left' if 'left' in corner else 'right')
                            vertical = ('top' if 'top' in corner else 'bottom')
                            cdp("(()=>{const e=document.querySelector('input');e.style.cssText="+
                                json.dumps(side+':8px;'+vertical+':8px;')+";e.focus();return true})()")
                        else:
                            control.sendto(corner.encode(), server)
                            assert control.recv(10) == b'ok'
                            time.sleep(.15)
                        assert value() == '', (name, 'fixture reset failed')
                        keys(spelling)
                        prefix = corner+'-'+('long' if len(spelling)>10 else 'short')+'-'+method
                        compact, _ = popup(directory/(prefix+'-compact.png'))
                        keys('-k', 'Down', '-k', 'Home')
                        expanded, selected = popup(directory/(prefix+'-expanded.png'))
                        width = lambda b:b[2]-b[0]
                        height = lambda b:b[3]-b[1]
                        assert abs(width(compact)-width(expanded)) <= 4, (compact, expanded)
                        assert height(expanded) > height(compact), (compact, expanded)
                        size = (width(expanded), height(expanded))
                        if spelling in sizes:
                            assert all(abs(a-b)<=4 for a,b in zip(size,sizes[spelling])), ('Clipped or unstable pane',size,sizes[spelling])
                        sizes[spelling] = size
                        matches = re.findall(r'zwp_text_input_v3#\d+\.set_cursor_rectangle\((-?\d+), (-?\d+), (\d+), (\d+)\)',
                                             (directory/'client.log').read_text())
                        assert matches, 'Client never supplied a real text rectangle'
                        rectangle = [int(v) for v in matches[-1]]
                        owner = next(w for w in windows() if w['id'] == wid)
                        ox, oy = owner['layout']['tile_pos_in_workspace_view']
                        dx, dy = owner['layout']['window_offset_in_tile']
                        cx, cy, cw, ch = rectangle
                        caret = [ox+dx+cx, oy+dy+cy, ox+dx+cx+cw, oy+dy+cy+ch]
                        pane = [v/output['scale'] for v in expanded]
                        assert pane[0]-24 <= caret[0] <= pane[2]+24, (corner,'horizontal anchor',pane,caret)
                        assert min(abs(pane[1]-caret[3]),abs(pane[3]-caret[1])) <= 40, (corner,'vertical anchor',pane,caret)
                        if method == 'keyboard':keys('-k', 'space')
                        else:
                            px,py = (int(v/output['scale']) for v in selected)
                            point(wid, px, py, (px-3,py-3,7,7), click=True);time.sleep(.3)
                        actual = value()
                        assert actual == expected, (name,corner,method,actual,expected)
                        records.append(dict(method=method, compact=compact, expanded=expanded,
                                            protocol_rectangle=rectangle, caret=caret, actual=actual))
                    result = dict(frontend=name,corner=corner,spelling=spelling,expected=expected,
                                  observations=records,passed=True)
                    results.append(result)
                    (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
                    print(name, corner, spelling, 'PASS', flush=True)
            if name == 'browser':
                text = (directory/'client.log').read_text()
                assert 'The selection range for surrounding text is invalid.' not in text
                assert 'The deletion range needs to cover whole selection range.' not in text
        finally:
            restore = wid is not None and any(w['id']==wid and w['is_focused'] for w in windows())
            if process is not None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()
            log.close()
            if control is not None:control.close()
            if profile.exists():shutil.rmtree(profile,ignore_errors=True)
            if restore and original is not None:action('focus-window','--id',str(original))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('frontends',nargs='*',choices=['browser','foot'])
    args=parser.parse_args()
    outputs=json.loads(subprocess.check_output(['niri','msg','-j','outputs']))
    active=[o['logical'] for o in outputs.values() if o.get('logical')]
    assert len(active)==1 and active[0]['x']==active[0]['y']==0, 'Live fixture requires a single output at origin'
    out=ROOT/'build/native-popup-regression';out.mkdir(parents=True,exist_ok=True)
    results=[]
    for name in args.frontends or ['browser','foot']:run(name,out,active[0],results)
    print('PASS native popup corner cases:',len(results))


if __name__=='__main__':main()
