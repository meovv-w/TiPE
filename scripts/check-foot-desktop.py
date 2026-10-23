#!/usr/bin/env python3
"""Native foot IME regression. Input is read by Python, never a shell."""
import json, os, pathlib, subprocess, time
from desktop_keys import send
root=pathlib.Path(__file__).resolve().parent.parent
out=root/'build/foot-regression';out.mkdir(parents=True,exist_ok=True)
fixture=out/'read-lines.py'
fixture.write_text('''import sys,json,pathlib
target=pathlib.Path(sys.argv[1]);values=[]
print('TiPE terminal regression: all input is fixture data',flush=True)
for line in sys.stdin:
 values.append(line.rstrip('\\n'))
 target.write_text(json.dumps(values,ensure_ascii=False))
''')
data=out/'lines.json';data.unlink(missing_ok=True)
def windows():return json.loads(subprocess.check_output(['niri','msg','-j','windows']))
original=next((w['id'] for w in windows() if w['is_focused']),None)
log=open(out/'foot.log','w')
p=subprocess.Popen(['foot','--title=TiPE terminal regression','python3','-u',str(fixture),str(data)],stdout=log,stderr=log,start_new_session=True)
try:
 for _ in range(80):
  matches=[w for w in windows() if w['pid']==p.pid]
  if matches:break
  assert p.poll() is None,'foot exited';time.sleep(.1)
 assert matches,'foot window missing'
 subprocess.run(['niri','msg','action','focus-window','--id',str(matches[0]['id'])],check=True)
 subprocess.run(['fcitx5-remote','-s','tipe'],check=True);subprocess.run(['fcitx5-remote','-o'],check=True)
 def keys(*args):send(p.pid,args);time.sleep(.25)
 keys('-d','100','nihao');subprocess.run(['grim',str(out/'compact.png')],check=True)
 keys('-k','Down');subprocess.run(['grim',str(out/'expanded.png')],check=True)
 keys('-k','Return');keys('-k','Return')
 keys('-d','100','nihao');keys('-k','space');keys('-k','Return')
 keys('-d','100','nihao');keys('-k','Shift_L');keys('-k','Shift_L');keys('-k','space');keys('-k','Return')
 keys('-d','100','github');keys('-k','space');keys('-k','Return')
 actual=json.loads(data.read_text())
 expected=['nihao','你好','你好','github']
 (out/'results.json').write_text(json.dumps(dict(actual=actual,expected=expected,passed=actual==expected),ensure_ascii=False,indent=2))
 assert actual==expected,(actual,expected)
 print('foot: verified 4 input paths')
finally:
 focused=next((w for w in windows() if w['is_focused']),None)
 restore=focused and focused['pid']==p.pid
 p.terminate()
 try:p.wait(timeout=5)
 except subprocess.TimeoutExpired:p.kill();p.wait()
 log.close()
 if restore and original is not None:subprocess.run(['niri','msg','action','focus-window','--id',str(original)],check=False)
