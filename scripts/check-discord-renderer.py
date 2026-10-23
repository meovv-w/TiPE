#!/usr/bin/env python3
"""Test an owned Discord bootstrap renderer with a local contenteditable.

This verifies Discord's installed Electron/Wayland input bridge. It does not
claim logged-in chat testing. No Discord messaging field is ever used.
"""
import json, os, pathlib, re, subprocess, time
from desktop_keys import send
root=pathlib.Path(__file__).resolve().parent.parent
out=root/'build/discord-regression';out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,DISCORD_USER_DATA_DIR=str(out/'profile'),TIPE_CDP_TARGET='splash/index.html')
def windows():return json.loads(subprocess.check_output(['niri','msg','-j','windows']))
original=next((w['id'] for w in windows() if w['is_focused']),None)
log=open(out/'discord.log','w')
p=subprocess.Popen([str(pathlib.Path.home()/'.config/discord/app-1.0.162/Discord'),
 '--remote-debugging-port=0','--ozone-platform=wayland','--enable-wayland-ime',
 '--wayland-text-input-version=3'],env=env,stdout=log,stderr=log,start_new_session=True)
results=[]
try:
 for _ in range(100):
  content=(out/'discord.log').read_text()
  port=re.search(r'DevTools listening on ws://127.0.0.1:(\d+)/',content)
  matches=[w for w in windows() if w['pid']==p.pid]
  if port and matches:break
  assert p.poll() is None,'Discord bootstrap exited';time.sleep(.1)
 assert port and matches,'Discord bootstrap renderer missing'
 (out/'DevToolsActivePort').write_text(port[1]+'\n')
 def cdp(expression):
  return json.loads(subprocess.check_output(['node',str(root/'scripts/browser-cdp.mjs'),str(out),expression],env=env,text=True))
 cdp('document.body.innerHTML="<h2>TiPE isolated Electron input fixture</h2><div id=fixture contenteditable=true style=background:white;color:black;min-height:120px;font-size:24px></div>";window.events=[];const e=document.getElementById("fixture");for(const t of ["input","compositionstart","compositionupdate","compositionend"])e.addEventListener(t,x=>events.push({type:t,data:x.data,value:e.innerText}));e.focus();true')
 subprocess.run(['niri','msg','action','focus-window','--id',str(matches[0]['id'])],check=True)
 subprocess.run(['fcitx5-remote','-s','tipe'],check=True);subprocess.run(['fcitx5-remote','-o'],check=True)
 cdp('document.getElementById("fixture").focus();true')
 def keys(*args):send(p.pid,args);time.sleep(.25)
 def check(name,expected):
  actual=cdp('document.getElementById("fixture").innerText')
  results.append(dict(name=name,expected=expected,actual=actual,passed=actual==expected,events=cdp('events')))
  (out/'results.json').write_text(json.dumps(dict(scope='Discord bootstrap Electron renderer; local contenteditable; no chat',results=results),ensure_ascii=False,indent=2))
  assert actual==expected,(name,actual,expected)
 def clear():keys('-k','Escape','-M','ctrl','a','-m','ctrl','-k','BackSpace');cdp('events=[];true')
 keys('-d','100','nihao');subprocess.run(['grim',str(out/'compact.png')],check=True)
 keys('-k','Down');subprocess.run(['grim',str(out/'expanded.png')],check=True)
 keys('-k','Return');check('Enter literal','nihao')
 clear();keys('-d','100','nihao');keys('-k','space');check('Chinese commit','你好')
 clear();keys('-d','100','nihao');keys('-k','Shift_L');keys('-k','Shift_L');keys('-k','space');check('Mode roundtrip','你好')
 clear();keys('-d','100','github');keys('-k','space');check('English literal','github')
 print('Discord bootstrap Electron renderer: verified 4 local input paths; no chat tested')
finally:
 focused=next((w for w in windows() if w['is_focused']),None)
 restore=focused and focused['pid']==p.pid
 p.terminate()
 try:p.wait(timeout=5)
 except subprocess.TimeoutExpired:p.kill();p.wait()
 log.close()
 if restore and original is not None:subprocess.run(['niri','msg','action','focus-window','--id',str(original)],check=False)
