#!/usr/bin/env python3
"""Check actual GTK commit/deletion feedback against published learning data.

Only an owned fixture receives keys; the user's existing learned data is never
cleared. The distinct executable name scopes these synthetic choices.
"""
import json, os, pathlib, shutil, subprocess, time
from desktop_keys import send

root=pathlib.Path(__file__).resolve().parent.parent
out=root/'build/retained-regression';out.mkdir(exist_ok=True)
exe=out/'tipe-retained-harness';shutil.copy2(root/'build/tipe-gtk-harness',exe)
env=dict(os.environ,GTK_PATH=str(pathlib.Path.home()/'.local/share/tipe/gtk/lib/gtk-4.0'),
         GTK_IM_MODULE='fcitx',TIPE_HARNESS_TIMEOUT_MS='90000')
model=pathlib.Path(os.environ.get('XDG_STATE_HOME',str(pathlib.Path.home()/'.local/state')))/'tipe/model.current'
def rows():
    if not model.exists(): return []
    return [line.split('\t') for line in model.read_text().splitlines() if not line.startswith('#')]
def count():
    return sum(int(r[2]) for r in rows() if len(r)==4 and r[1]=='世界' and r[3]=='tipe-retained-harness')
def windows(): return json.loads(subprocess.check_output(['niri','msg','-j','windows']))
original=next((w['id'] for w in windows() if w['is_focused']),None)
log=open(out/'gtk.jsonl','w');err=open(out/'gtk.err','w')
p=subprocess.Popen([str(exe)],env=env,stdout=log,stderr=err,start_new_session=True)
results=[]
try:
    for _ in range(80):
        matches=[w for w in windows() if w['pid']==p.pid]
        if matches: break
        assert p.poll() is None,'GTK fixture exited';time.sleep(.1)
    assert len(matches)==1,'Owned GTK fixture missing'
    wid=matches[0]['id']
    subprocess.run(['niri','msg','action','focus-window','--id',str(wid)],check=True)
    subprocess.run(['fcitx5-remote','-s','tipe'],check=True)
    subprocess.run(['fcitx5-remote','-o'],check=True)
    def keys(*args): send(p.pid,args);time.sleep(.2)
    def text():
        values=[json.loads(s)['value'] for s in (out/'gtk.jsonl').read_text().splitlines()
                if s.startswith('{') and json.loads(s).get('event')=='text' and 'value' in json.loads(s)]
        return values[-1] if values else ''
    def clear():
        keys('-k','Escape','-M','ctrl','a','-m','ctrl','-k','BackSpace')
        assert text()=='',text()
    baseline=count()
    for _ in range(3):
        keys('-d','80','shijie','-k','space');assert text()=='世界',text()
        keys('-k','BackSpace');clear()
    time.sleep(1.5)
    deleted=count()
    results.append(dict(name='Immediately deleted commits do not train',before=baseline,after=deleted,passed=deleted==baseline))
    assert deleted==baseline,results[-1]
    for _ in range(3):
        keys('-d','80','shijie','-k','space');assert text()=='世界',text()
        # A following ordinary action confirms the client's actual suffix.
        keys('-k','space');assert text()=='世界 ',text()
        time.sleep(.8);clear()
    for _ in range(40):
        learned=count()
        if learned==baseline+3: break
        time.sleep(.1)
    results.append(dict(name='Retained commits reach automatic published model',before=baseline,after=learned,passed=learned==baseline+3))
    assert learned==baseline+3,results[-1]
    print('GTK retained learning: deletion discarded and 3 confirmed commits published')
finally:
    (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
    restore=any(w['pid']==p.pid and w['is_focused'] for w in windows())
    p.terminate()
    try:p.wait(timeout=5)
    except subprocess.TimeoutExpired:p.kill();p.wait()
    log.close();err.close()
    if restore and original is not None:subprocess.run(['niri','msg','action','focus-window','--id',str(original)],check=False)
