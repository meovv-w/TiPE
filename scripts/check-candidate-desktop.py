#!/usr/bin/env python3
"""Real desktop candidate regression. Run sequentially; input aborts on focus change.
Usage: python3 scripts/check-candidate-desktop.py gtk4-sdfas sdfas
The fixture is the only window receiving injected keys. Screenshots and client
commit/preedit logs go to build/desktop-acceptance.
"""
import subprocess,json,os,time,pathlib,sys
from desktop_keys import send
root=pathlib.Path(__file__).resolve().parent.parent
outdir=root/'build/desktop-acceptance'
outdir.mkdir(parents=True,exist_ok=True)
name=sys.argv[1] if len(sys.argv)>1 else 'gtk4'
qt=name.startswith('qt')
spelling=sys.argv[2] if len(sys.argv)>2 else 'nihao'
gtk_prefix = pathlib.Path.home()/'.local/share/tipe/gtk'
if not gtk_prefix.is_dir():
    gtk_prefix = outdir/'gtkpath'
# GTK_PATH contains toolkit-specific module roots (the directory that holds
# the `4.0.0/immodules` or `3.0.0/immodules` child), rather than the staging
# prefix itself. Pointing it at the prefix silently makes GTK fall back to the
# system Fcitx module, which bypasses TiPE's shared painter.
gtk_path = gtk_prefix/'lib/gtk-4.0'
if not gtk_path.is_dir():
    gtk_path = gtk_prefix
env=dict(os.environ,GTK_PATH=str(gtk_path),GTK_IM_MODULE='fcitx',QT_IM_MODULE='fcitx',TIPE_HARNESS_TIMEOUT_MS='60000',TIPE_HARNESS_STRETCHED=str(int('stretched' in name)))
if qt:
    # Exercise the delivered module, not a newer staging-only plugin.
    qt_prefix = pathlib.Path.home()/'.local/share/tipe/qt'
    qt_plugin_dir = qt_prefix/'lib/qt6/plugins'
    if not qt_plugin_dir.is_dir():
        qt_plugin_dir = qt_prefix/'lib64/qt6/plugins'
    if not qt_plugin_dir.is_dir():
        raise SystemExit(f'Qt plugin directory not found under {qt_prefix}')
    env['QT_PLUGIN_PATH']=str(qt_plugin_dir)
original_windows=json.loads(subprocess.check_output(['niri','msg','-j','windows']))
original_focus=next((w['id'] for w in original_windows if w['is_focused']),None)
out=open(outdir/(name+'.jsonl'),'w');err=open(outdir/(name+'.err'),'w')
p=subprocess.Popen([str(root/('build/tipe-qt-harness' if qt else 'build/tipe-gtk-harness'))],env=env,stdout=out,stderr=err,start_new_session=True)
try:
 for _ in range(80):
  assert p.poll() is None, (p.returncode,(outdir/(name+'.err')).read_text())
  wins=json.loads(subprocess.check_output(['niri','msg','-j','windows'])); matches=[w for w in wins if w['pid']==p.pid]
  if matches: break
  time.sleep(.1)
 assert matches, 'Harness window not mapped'
 w=matches[0]
 subprocess.run(['niri','msg','action','focus-window','--id',str(w['id'])],check=True)
 subprocess.run(['fcitx5-remote','-s','tipe'],check=True);subprocess.run(['fcitx5-remote','-o'],check=True)
 time.sleep(.2)
 def keys(*args):
  assert p.poll() is None
  focused=next(w for w in json.loads(subprocess.check_output(['niri','msg','-j','windows'])) if w['is_focused'])
  assert focused['pid']==p.pid, 'Focus changed; abort input'
  send(p.pid,args)
 def capture(state):
  time.sleep(.4);subprocess.run(['grim',str(outdir/(name+'-'+state+'.png'))],check=True)
 keys('-d','100',spelling);capture('compact')
 keys('-k','Down');capture('expanded')
 keys('-k','Right');capture('moved')
 keys('-k','Return');time.sleep(.2)
 keys('-d','100',spelling);keys('-k','space');time.sleep(.2)
 print(name,'pid',p.pid,'completed')
finally:
 focused=next((w for w in json.loads(subprocess.check_output(['niri','msg','-j','windows'])) if w['is_focused']),None)
 restore=focused is not None and focused['pid']==p.pid
 p.terminate();p.wait(timeout=5)
 out.close();err.close()
 if restore and original_focus is not None:
  subprocess.run(['niri','msg','action','focus-window','--id',str(original_focus)],check=False)
