#!/usr/bin/env python3
"""Real GTK/Qt popup edge and pointer regression, only on owned fixtures."""
import json, os, pathlib, re, shutil, subprocess, sys, time
import numpy as np
from PIL import Image
from desktop_keys import send
from desktop_pointer import point

root=pathlib.Path(__file__).resolve().parent.parent
toolkit=sys.argv[1] if len(sys.argv)>1 else 'gtk'
assert toolkit in ('gtk','qt')
full_matrix=os.environ.get('TIPE_POPUP_FULL_MATRIX')=='1'
out=root/('build/popup-full-regression' if full_matrix else 'build/popup-regression')/toolkit;out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,GTK_PATH=str(pathlib.Path.home()/'.local/share/tipe/gtk/lib/gtk-4.0'),
         GTK_IM_MODULE='fcitx',QT_IM_MODULE='fcitx',TIPE_HARNESS_TIMEOUT_MS='300000',TIPE_HARNESS_EDGE='1',
         WAYLAND_DEBUG='client')
if toolkit=='qt':env['QT_PLUGIN_PATH']=str(pathlib.Path.home()/'.local/share/tipe/qt/lib/qt6/plugins')
def windows():return json.loads(subprocess.check_output(['niri','msg','-j','windows']))
original=next((w['id'] for w in windows() if w['is_focused']),None)
outputs=json.loads(subprocess.check_output(['niri','msg','-j','outputs']))
active=[o['logical'] for o in outputs.values() if o.get('logical')]
assert len(active)==1 and active[0]['x']==active[0]['y']==0
output=active[0];scale=output['scale']
log=open(out/'client.jsonl','w');err=open(out/'client.err','w')
# A fresh application identity isolates earlier deliberate candidate choices.
# Each spelling is selected only twice, below the exact-preference threshold,
# so learning cannot reorder the second half of a mouse/keyboard comparison.
fixture=out/f'tipe-popup-{toolkit}-{time.monotonic_ns()}'
shutil.copy2(root/f'build/tipe-{toolkit}-harness',fixture)
p=subprocess.Popen([str(fixture)],env=env,stdout=log,stderr=err,start_new_session=True)
results=[]
try:
 for _ in range(80):
  matches=[w for w in windows() if w['pid']==p.pid]
  if matches:break
  assert p.poll() is None,'Fixture exited';time.sleep(.1)
 assert len(matches)==1,'Owned fixture missing'
 wid=matches[0]['id']
 subprocess.run(['niri','msg','action','focus-window','--id',str(wid)],check=True)
 subprocess.run(['niri','msg','action','move-window-to-floating','--id',str(wid)],check=True)
 # Tiling initially stretches the fixture to the output height. Floating
 # preserves that size, so explicitly size our own test window before moving
 # it: a caret outside the output cannot exercise popup edge constraints.
 for axis,size in [('width',640),('height',180)]:
  subprocess.run(['niri','msg','action','set-window-'+axis,'--id',str(wid),str(size)],check=True)
 subprocess.run(['fcitx5-remote','-s','tipe'],check=True);subprocess.run(['fcitx5-remote','-o'],check=True)
 def keys(*args):send(p.pid,args);time.sleep(.2)
 def value():
  rows=[json.loads(s) for s in (out/'client.jsonl').read_text().splitlines() if s.startswith('{')]
  values=[r['value'] for r in rows if r.get('event')=='text' and 'value' in r]
  return values[-1] if values else ''
 def clear():
  keys('-k','Escape','-M','ctrl','a','-m','ctrl','-k','BackSpace')
  assert value()=='',value()
 def park():
  owned=next(w for w in windows() if w['id']==wid)
  x,y=owned['layout']['tile_pos_in_workspace_view']
  width,height=owned['layout']['window_size']
  dx,dy=owned['layout']['window_offset_in_tile']
  point(wid,int(x+dx+30),int(y+dy+30),(x+dx,y+dy,width,height))
  time.sleep(.2)
 def capture(name):
  path=out/(name+'.png');time.sleep(.3);subprocess.run(['grim',str(path)],check=True)
  a=np.array(Image.open(path).convert('RGB')).astype(int)
  m=(abs(a[:,:,0]-10)<4)&(abs(a[:,:,1]-132)<4)&(a[:,:,2]>248)
  # Exclude isolated app titlebar pixels. A real selection contains a block
  # with several consecutive rows and at least 12 blue pixels on each row.
  counts=m.sum(axis=1);ys=np.where(counts>=12)[0]
  assert len(ys)>=10,'Candidate selection missing from compositor screenshot'
  y=int(ys[len(ys)//2]);xs=np.where(m[y])[0]
  # The fixture's selection dominates any unrelated single-pixel icons.
  runs=np.split(xs,np.where(np.diff(xs)>1)[0]+1);run=max(runs,key=len)
  x=int(run[len(run)//2])
  local=m[max(0,y-35):min(m.shape[0],y+36),max(0,x-100):min(m.shape[1],x+101)]
  yy,xx=np.where(local);bounds=[int(max(0,x-100)+xx.min()),int(max(0,y-35)+yy.min()),
                              int(max(0,x-100)+xx.max()),int(max(0,y-35)+yy.max())]
  assert 0<bounds[0]<bounds[2]<a.shape[1]-1 and 0<bounds[1]<bounds[3]<a.shape[0]-1
  return int(x/scale),int(y/scale),bounds
 def popup_geometry():
  rows=re.findall(r'xdg_popup#\d+\.configure\((-?\d+), (-?\d+), (\d+), (\d+)\)',
                  (out/'client.err').read_text())
  assert rows,'No native popup configure from compositor'
  return [int(v) for v in rows[-1]]
 # Move only the newly created fixture. Product popup placement still comes
 # exclusively from GTK/Qt's native protocol parent and cursor rectangle.
 cases=[('bottom-right',output['width']-660,output['height']-210,'nihao'),('top-left',20,20,'shijie')]
 if full_matrix:
  cases.extend([('bottom-left',20,output['height']-210,'tianqi'),
                ('top-right',output['width']-660,20,'xuexiao')])
  # Each spelling is compared twice in this fresh application context. Four
  # corners must not reuse it eight times: the third confirmed choice can
  # legitimately publish a new preference between keyboard and mouse paths.
  long_spellings=['zhonghuarenmingongheguo','woxiangzhidaoweishenme',
                  'beijingdaxuejisuanjixueyuan','jintiantianqizhenbucuo']
  cases.extend([(name+'-long',x,y,spelling) for (name,x,y,_),spelling
                in zip(list(cases),long_spellings)])
 for name,x,y,spelling in cases:
  subprocess.run(['niri','msg','action','move-floating-window','--id',str(wid),'-x',str(x),'-y',str(y)],check=True)
  time.sleep(.4);clear();park()
  keys('-d','80',spelling);capture(name+'-compact');compact=popup_geometry()
  keys('-k','Down');px,py,bounds=capture(name+'-expanded')
  expanded=popup_geometry()
  assert compact[2]==expanded[2] and compact[3]<expanded[3],(compact,expanded)
  assert expanded[2]<=output['width'] and expanded[3]<=output['height'],expanded
  if name.startswith('bottom-'):
   assert expanded[1]<compact[1],'Bottom edge did not flip popup above caret'
  keys('-k','space');expected=value();assert expected and expected!=spelling,expected
  clear();park();keys('-d','80',spelling,'-k','Down')
  px,py,bounds=capture(name+'-pointer')
  point(wid,px,py,(max(0,px-4),max(0,py-4),9,9))
  time.sleep(.4)
  hx,hy,hover_bounds=capture(name+'-hover')
  # The screenshot establishes this narrow selection rectangle is a popup
  # target; focus is checked again before the real pointer click.
  point(wid,px,py,(max(0,px-4),max(0,py-4),9,9),click=True)
  time.sleep(.4);actual=value()
  results.append(dict(name=name,expected=expected,actual=actual,selection_bounds=bounds,
                      target=[px,py],hover_bounds=hover_bounds,
                      compact_geometry=compact,expanded_geometry=expanded,
                      pointer_matches_keyboard=actual==expected,passed=actual==expected))
  (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
  assert actual==expected,results[-1]
 print(toolkit,'edge popup visible and mouse selection equals keyboard in',len(results),'corner/spelling cases')
finally:
 (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
 restore=any(w['pid']==p.pid and w['is_focused'] for w in windows())
 p.terminate()
 try:p.wait(timeout=5)
 except subprocess.TimeoutExpired:p.kill();p.wait()
 log.close();err.close()
 fixture.unlink(missing_ok=True)
 if restore and original is not None:subprocess.run(['niri','msg','action','focus-window','--id',str(original)],check=False)
