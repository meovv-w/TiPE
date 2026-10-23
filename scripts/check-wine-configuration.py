#!/usr/bin/env python3
"""Real Wine registry transaction checks, restricted to a repository fixture prefix."""
import json,os,subprocess,tempfile
from pathlib import Path
repo=Path(__file__).resolve().parents[1]
prefix=repo/'build/wine-regression/prefix'
assert (prefix/'system.reg').is_file(), 'Run check-wine-desktop.py once to create the owned fixture prefix'
with tempfile.TemporaryDirectory(prefix='tipe-wine-config-test-') as t:
 env=dict(os.environ,WINEPREFIX=str(prefix),WINEDEBUG='-all',XDG_STATE_HOME=t)
 script=repo/'scripts/configure-wine-input.py';app='tipe-config-transaction-test.exe'
 key='HKCU\\Software\\Wine\\AppDefaults\\'+app+'\\X11 Driver'
 def reg(*args,check=True):return subprocess.run(['wine','reg',*args],env=env,capture_output=True,text=True,check=check)
 def cli(action,ok=True):
  p=subprocess.run(['python3',str(script),'--prefix',str(prefix),'--app',app,action],env=env,capture_output=True,text=True)
  assert (p.returncode==0)==ok,(action,p.stdout,p.stderr)
  return json.loads(p.stdout) if ok else p.stderr
 def value(k):
  p=reg('query',k,'/v','InputStyle',check=False)
  return p.stdout if p.returncode==0 else None
 global_key=r'HKCU\Software\Wine\X11 Driver';before_global=value(global_key)
 results=[]
 try:
  reg('delete',key,'/f',check=False)
  assert cli('--dry-run')['input_style'] is None and value(key) is None
  a=cli('--apply');assert a['input_style']=='overthespot'
  state=Path(a['recovery']);assert state.stat().st_mode&0o777==0o600
  assert cli('--apply')['input_style']=='overthespot'
  cli('--restore');assert value(key) is None
  results.append({'case':'absent original, preview, apply, repeat and restore','passed':True})
  original=' root '
  reg('add',key,'/v','InputStyle','/t','REG_SZ','/d',original,'/f')
  a=cli('--apply');assert json.loads(Path(a['recovery']).read_text())['before']==original
  cli('--restore');assert value(key).split('REG_SZ    ',1)[1].splitlines()[0]==original
  results.append({'case':'preserve exact original including whitespace','passed':True})
  cli('--apply');reg('add',key,'/v','InputStyle','/t','REG_SZ','/d','root','/f')
  assert '已被修改' in cli('--restore',False);assert value(key).split('REG_SZ    ',1)[1].splitlines()[0]=='root'
  results.append({'case':'preserve later user edit','passed':True})
  reg('add',key,'/v','InputStyle','/t','REG_SZ','/d','overthespot','/f')
  a=cli('--apply');state=Path(a['recovery']);r=json.loads(state.read_text());r['phase']='prepared';state.write_text(json.dumps(r))
  cli('--restore');assert value(key).split('REG_SZ    ',1)[1].splitlines()[0]==original
  results.append({'case':'recover prepared journal after registry write','passed':True})
  assert before_global==value(global_key)
  results.append({'case':'global Wine InputStyle unchanged throughout','passed':True})
 finally:reg('delete',key,'/f',check=False)
 (repo/'build/wine-input-config-test/results.json').write_text(json.dumps(results,indent=2)+'\n')
 print('PASS Wine app configuration transactions:',len(results))
