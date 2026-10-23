#!/usr/bin/env python3
"""Win32 EDIT fixture in a private WINEPREFIX, with direct file observations."""
import json, os, pathlib, subprocess, time
from desktop_keys import send
from desktop_pointer import point
root=pathlib.Path(__file__).resolve().parent.parent
backend=os.environ.get('TIPE_WINE_BACKEND','x11')
assert backend in ('x11','wayland')
out=root/('build/wine-regression' if backend=='x11' else 'build/wine-wayland-regression');out.mkdir(parents=True,exist_ok=True)
prefix=out/'prefix';prefix.mkdir(exist_ok=True)
exe=out/'input.exe'
subprocess.run(['x86_64-w64-mingw32-gcc',str(root/'tests/real_wine_harness.c'),'-municode','-mwindows','-o',str(exe)],check=True)
env=dict(os.environ,WINEPREFIX=str(prefix),WINEDEBUG=os.environ.get('TIPE_WINE_DEBUG','-all'),XMODIFIERS='@im=fcitx',WINEDLLOVERRIDES='winemenubuilder.exe=d')
# Force Wine's supported X11 bridge in this private prefix only.
subprocess.run(['wine','reg','add',r'HKCU\Software\Wine\Drivers','/v','Graphics','/d',backend,'/f'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=60,check=True)
# A protocol-style experiment only changes this fixture's private prefix.
input_style=os.environ.get('TIPE_WINE_INPUT_STYLE','default')
assert input_style in ('default','overthespot','root')
if input_style=='default':
 subprocess.run(['wine','reg','delete',r'HKCU\Software\Wine\X11 Driver','/v','InputStyle','/f'],env=env,
                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=60,check=False)
else:
 subprocess.run(['wine','reg','add',r'HKCU\Software\Wine\X11 Driver','/v','InputStyle','/d',input_style,'/f'],env=env,
                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=60,check=True)
if backend=='wayland':env.pop('DISPLAY',None)
def windows():return json.loads(subprocess.check_output(['niri','msg','-j','windows']))
original=next((w['id'] for w in windows() if w['is_focused']),None)
log=open(out/'wine.log','w');textfile=out/'text.utf8';textfile.unlink(missing_ok=True)
p=subprocess.Popen(['wine',str(exe)],cwd=out,env=env,stdout=log,stderr=log,start_new_session=True)
results=[]
try:
 for _ in range(150):
  matches=[w for w in windows() if w['title']=='TiPE Wine isolated regression']
  if matches and textfile.exists():break
  time.sleep(.1)
 assert len(matches)==1,'Owned Wine input window missing'
 wid=matches[0]['id']
 subprocess.run(['niri','msg','action','focus-window','--id',str(wid)],check=True)
 if os.environ.get('TIPE_WINE_POINTER_CHECK')=='1':
  # Only this script's newly created fixture is moved. Establish a real
  # pointer enter to distinguish Xwayland's hovered-parent fallback.
  subprocess.run(['niri','msg','action','move-window-to-floating','--id',str(wid)],check=True)
  subprocess.run(['niri','msg','action','move-floating-window','--id',str(wid),'-x','100','-y','200'],check=True)
  time.sleep(.4)
  owned=next(w for w in windows() if w['id']==wid)
  x,y=owned['layout']['tile_pos_in_workspace_view'];ww,hh=owned['layout']['window_size']
  point(wid,x+40,y+50,(x,y,ww,hh))
 subprocess.run(['fcitx5-remote','-s','tipe'],check=True);subprocess.run(['fcitx5-remote','-o'],check=True)
 def keys(*args):send(None,args,window_id=wid);time.sleep(.25)
 def check(name,expected):
  actual=textfile.read_text();results.append(dict(name=name,expected=expected,actual=actual,passed=actual==expected))
  (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2));assert actual==expected,(name,actual,expected)
 def clear():
  if os.environ.get('TIPE_HARNESS_CARET')=='1':
   keys('-k','Escape')
   request=out/'reset.request';request.touch()
   for _ in range(50):
    if not request.exists() and textfile.read_text()=='':break
    time.sleep(.05)
  else:
   keys('-k','Escape','-k','End','-M','shift','-k','Home','-m','shift','-k','BackSpace')
  assert textfile.read_text()=='',('Fixture selection did not clear document',textfile.read_text())
 if os.environ.get('TIPE_HARNESS_CARET')=='1':
  observations=[]
  for name,prefix_text in [('origin',''),('after-text','abcdef'),('new-line','abcdefghij\n')]:
   clear()
   if prefix_text:
    subprocess.run(['fcitx5-remote','-c'],check=True)
    for line in prefix_text.splitlines(keepends=True):
     keys(line.rstrip('\n'))
     if line.endswith('\n'):keys('-k','Return')
    subprocess.run(['fcitx5-remote','-o'],check=True)
   keys('nihao');time.sleep(.3)
   caret=json.loads((out/'caret.json').read_text());assert caret['valid'],caret
   tree=json.loads(subprocess.check_output(['python3',str(root/'scripts/x11-window-tree.py')]))
   owners=[r for r in tree if r['title']=='TiPE Wine isolated regression'];assert len(owners)==1
   candidates=[r for r in tree if r['title']=='Fcitx5 Input Window' and r['map_state']==2];assert len(candidates)==1
   popup=candidates[0]
   if input_style=='overthespot' or os.environ.get('TIPE_WINE_EXPECT_CARET')=='1':
    # XIM Position gives a client-relative spot. Compare it with Win32's
    # independently observed client-relative caret, not guessed desktop IPC.
    rect=caret['client_rect']
    assert abs(popup['x']-rect[0])<=3 and abs(popup['y']-rect[1]-1)<=3, (name,caret,popup)
   observations.append(dict(name=name,input_style=input_style,caret=caret,owner=owners[0],popup=popup))
   keys('-k','Escape')
  (out/'caret-results.json').write_text(json.dumps(observations,ensure_ascii=False,indent=2)+'\n')
  clear()
 keys('-d','120','nihao');subprocess.run(['grim',str(out/'compact.png')],check=True)
 keys('-k','Down');subprocess.run(['grim',str(out/'expanded.png')],check=True)
 (out/'input-contexts.txt').write_bytes(subprocess.check_output(['busctl','--user','call','org.fcitx.Fcitx5','/controller','org.fcitx.Fcitx.Controller1','DebugInfo']))
 (out/'x11-windows.json').write_bytes(subprocess.check_output(['python3',str(root/'scripts/x11-window-tree.py')]))
 for win in json.loads((out/'x11-windows.json').read_text()):
  if win['title']=='Fcitx5 Input Window':
   (out/'popup-properties.txt').write_bytes(subprocess.check_output(['xprop','-id',win['id'],'WM_TRANSIENT_FOR','_NET_WM_WINDOW_TYPE']))
   # Compare the X drawable with the compositor screenshot: a mapped X
   # window alone does not establish that its pixels reached the desktop.
   subprocess.run(['import','-window',win['id'],str(out/'popup-drawable.png')],check=True,timeout=10)
 # A visible drawable must actually reach the compositor. Detect its unique
 # selection color in a connected region, excluding isolated titlebar pixels.
 from PIL import Image
 import numpy as np
 a=np.array(Image.open(out/'expanded.png').convert('RGB')).astype(int)
 blue=(abs(a[:,:,0]-10)<3)&(abs(a[:,:,1]-132)<3)&(a[:,:,2]>250)
 count=int(blue.sum())
 results.append(dict(name='Expanded candidate visible on desktop',blue_pixels=count,passed=count>=200))
 (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
 assert count>=200,'X11 popup drawable did not reach compositor'
 keys('-k','Return');check('Enter literal','nihao')
 clear();keys('-d','120','nihao');keys('-k','space')
 # Repeated manual test runs may deliberately have taught raw spelling. The
 # visible second candidate must let the user revoke that exact preference.
 if textfile.read_text()=='nihao':
  check('Learned raw habit remains explicit','nihao')
  clear();keys('-d','120','nihao');keys('2');check('Chinese choice revokes raw habit','你好')
  time.sleep(.5);clear();keys('-d','120','nihao');keys('-k','space')
 check('Chinese commit','你好')
 clear();keys('-d','120','nihao');keys('-k','Shift_L');keys('-k','Shift_L');keys('-k','space');check('Mode roundtrip','你好')
 clear();keys('-d','120','github');keys('-k','space');check('English literal','github')
 # Use a spelling absent from earlier fixture runs: an explicit selection
 # can itself train the next run and legitimately change candidate order.
 corpus=['tianqi','xingqi','jinri','shijie','chengshi','xuexiao','gongzuo','shenghuo',
         'wenjian','jisuanji','shurufa','zhongwen','yuyan','pingmu','chuangkou',
         'renmin','tushuguan','huanying','xianzai','mingtian','zuotian','jintian',
         'xuexi','kexue','daxue','shijian','xinwen','xinxi','jingyan','fangfa']
 history=out/'mouse-spellings.json'
 used=json.loads(history.read_text()) if history.exists() else []
 spelling=next((word for word in corpus if word not in used),None)
 assert spelling,'Mouse comparison corpus exhausted; preserve learning data and extend corpus'
 used.append(spelling);history.write_text(json.dumps(used))
 clear();keys('-d','120',spelling,'-k','Down','-k','space')
 for _ in range(4):
  if textfile.read_text():break
  # A lower candidate can consume only a prefix. Complete the same remaining
  # pinyin chain on both paths before comparing committed Win32 text.
  keys('-k','space')
 expected=textfile.read_text();assert expected and expected!=spelling,(spelling,expected)
 clear();keys('-d','120',spelling,'-k','Down')
 subprocess.run(['grim',str(out/'pointer.png')],check=True)
 a=np.array(Image.open(out/'pointer.png').convert('RGB')).astype(int)
 m=(abs(a[:,:,0]-10)<3)&(abs(a[:,:,1]-132)<3)&(a[:,:,2]>250)
 ys=np.where(m.sum(axis=1)>=12)[0];assert len(ys)>=10
 y=int(ys[len(ys)//2]);xs=np.where(m[y])[0]
 runs=np.split(xs,np.where(np.diff(xs)>1)[0]+1);run=max(runs,key=len)
 x=int(run[len(run)//2])
 outputs=json.loads(subprocess.check_output(['niri','msg','-j','outputs']))
 active=[o['logical'] for o in outputs.values() if o.get('logical')]
 assert len(active)==1
 scale=active[0]['scale'];px,py=int(x/scale),int(y/scale)
 point(wid,px,py,(px-4,py-4,9,9),click=True);time.sleep(.4)
 for _ in range(4):
  if textfile.read_text():break
  keys('-k','space')
 check('Mouse choice equals keyboard choice',expected)
 print('Wine standard EDIT: verified',len(results),'input paths')
finally:
 focused=next((w for w in windows() if w['is_focused']),None)
 restore=focused and focused['title']=='TiPE Wine isolated regression'
 subprocess.run(['wineserver','-k'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=False)
 try:p.wait(timeout=5)
 except subprocess.TimeoutExpired:p.kill();p.wait()
 log.close()
 if restore and original is not None:subprocess.run(['niri','msg','action','focus-window','--id',str(original)],check=False)
