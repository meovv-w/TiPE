#!/usr/bin/env python3
"""Real Wayland Chromium regression, aborting input on fixture focus loss."""
import json, os, pathlib, shutil, subprocess, time
from desktop_keys import send
root=pathlib.Path(__file__).resolve().parent.parent
out=root/'build/browser-regression'
out.mkdir(parents=True,exist_ok=True)
profile=out/'isolated-profile'
profile.mkdir(exist_ok=True)
page=out/'input.html'
page.write_text('''<!doctype html><meta charset=utf-8><title>TiPE browser regression</title>
<style>body{font:20px sans-serif;padding:24px}textarea{font:24px sans-serif;width:90%;height:220px}</style>
<h1>TiPE 浏览器输入回归</h1><textarea autofocus></textarea>
<script>window.events=[];const e=document.querySelector('textarea');
for(const t of ['input','compositionstart','compositionupdate','compositionend'])
e.addEventListener(t,x=>events.push({type:t,data:x.data,value:e.value}));</script>''')
def windows():return json.loads(subprocess.check_output(['niri','msg','-j','windows']))
original=next((w['id'] for w in windows() if w['is_focused']),None)
env=dict(os.environ,GTK_IM_MODULE='fcitx',XMODIFIERS='@im=fcitx')
log=open(out/'chromium.log','w')
process=subprocess.Popen([shutil.which('chromium-browser') or 'chromium',
 '--user-data-dir='+str(profile),'--no-first-run','--no-default-browser-check',
 '--disable-background-networking','--disable-extensions','--remote-debugging-port=0',
 '--ozone-platform=wayland','--enable-wayland-ime','--wayland-text-input-version=3',
 '--app='+page.as_uri()],env=env,stdout=log,stderr=log,start_new_session=True)
results=[]
def cdp(expression):
 return json.loads(subprocess.check_output(['node',str(root/'scripts/browser-cdp.mjs'),str(profile),expression],text=True))
def keys(*args):
 assert process.poll() is None,'Browser exited'
 focused=next((w for w in windows() if w['is_focused']),None)
 assert focused and focused['pid']==process.pid,'Focus changed; abort input'
 send(process.pid,args)
 time.sleep(.18)
def clear():
 keys('-k','Escape')
 keys('-M','ctrl','a','-m','ctrl','-k','BackSpace')
 cdp('events=[];true')
def check(name,expected):
 actual=cdp('document.querySelector("textarea").value')
 results.append(dict(name=name,expected=expected,actual=actual,passed=actual==expected,
                     events=cdp('events')))
 (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
 assert actual==expected,(name,actual,expected)
try:
 for _ in range(100):
  assert process.poll() is None,'Browser startup failed'
  matches=[w for w in windows() if w['pid']==process.pid]
  if matches and (profile/'DevToolsActivePort').exists():break
  time.sleep(.1)
 assert matches,'Owned browser window missing'
 subprocess.run(['niri','msg','action','focus-window','--id',str(matches[0]['id'])],check=True)
 subprocess.run(['fcitx5-remote','-s','tipe'],check=True)
 subprocess.run(['fcitx5-remote','-o'],check=True)
 cdp('document.querySelector("textarea").focus();true')
 clear();keys('-d','100','nihao')
 subprocess.run(['grim',str(out/'compact.png')],check=True)
 keys('-k','Down');subprocess.run(['grim',str(out/'expanded.png')],check=True)
 keys('-k','Return');check('Enter keeps original spelling','nihao')
 clear();keys('-d','100','nihao');keys('-k','space');check('Space commits Chinese','你好')
 clear();keys('-d','100','nihao');keys('-k','Shift_L');keys('-k','Shift_L')
 keys('-k','space');check('Mode tap roundtrip preserves composition','你好')
 clear();keys('-d','100','github');keys('-k','space');check('English literal','github')
 clear();keys('-d','100','nihao');keys('-k','Home');keys('-k','Delete');keys('-k','Return')
 check('Preedit caret editing','ihao')
 clear();keys('-d','100','nihao');keys('-k','space');keys('-k','BackSpace');keys('-k','Return')
 # Chromium versions may advertise surrounding text but omit its contents.
 # Restoration must then fail closed, leaving one ordinary client Backspace.
 # Record which capability was observed instead of silently accepting either.
 actual=cdp('document.querySelector("textarea").value')
 if actual=='nihao':check('Backspace restores editable spelling','nihao')
 else:check('Unavailable surrounding text: client Backspace fallback','你\n')
 print(json.dumps({'passed':len(results),'results':str(out/'results.json')},ensure_ascii=False))
finally:
 focused=next((w for w in windows() if w['is_focused']),None)
 restore=focused and focused['pid']==process.pid
 process.terminate()
 try:process.wait(timeout=5)
 except subprocess.TimeoutExpired:process.kill();process.wait()
 log.close()
 if restore and original is not None:
  subprocess.run(['niri','msg','action','focus-window','--id',str(original)],check=False)
