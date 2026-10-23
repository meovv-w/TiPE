#!/usr/bin/env python3
"""Enable Wine's standard position-capable XIM style for one executable.

Only AppDefaults/<exe>/X11 Driver/InputStyle is changed. Original values are
journaled before mutation and restored only if the current value is still ours.
No Wine global defaults, graphics backend, Fcitx profile or compositor settings
are changed. The caller explicitly chooses --apply or --restore.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from owned_files import atomic_write

GLOBAL_KEY = r'HKCU\Software\Wine\X11 Driver'


def read_value(env, key):
    result = subprocess.run(['wine','reg','query',key,'/v','InputStyle'],env=env,
                            capture_output=True,text=True,errors='replace',timeout=30)
    if result.returncode:
        message=result.stdout+result.stderr
        if result.returncode==1 and re.search(r'(unable to find|cannot find|not found)',message,re.I):
            return None
        raise RuntimeError('无法读取 Wine InputStyle，未修改配置：'+message.strip())
    # Wine reg.exe separates these fields with exactly four spaces. Preserve
    # every data character, including leading/trailing spaces; reject unknown
    # formatting or multiline values instead of making a lossy backup.
    lines=result.stdout.splitlines()
    matches=[(i,re.fullmatch(r'    InputStyle    (REG_\w+)    (.*)',line,re.I))
             for i,line in enumerate(lines)]
    matches=[(i,m) for i,m in matches if m]
    match=matches[0][1] if len(matches)==1 else None
    if not match or match[1]!='REG_SZ':
        raise RuntimeError('原 InputStyle 不是可完整备份的字符串，已保留配置')
    if any(line for line in lines[matches[0][0]+1:]):
        raise RuntimeError('原 InputStyle 包含多行内容，已保留配置')
    return match[2]


def write_value(env,key,value):
    command=['wine','reg']
    if value is None: command+=['delete',key,'/v','InputStyle','/f']
    else: command+=['add',key,'/v','InputStyle','/t','REG_SZ','/d',value,'/f']
    result=subprocess.run(command,env=env,capture_output=True,text=True,errors='replace',timeout=30)
    if result.returncode:raise RuntimeError('Wine 配置写入失败；恢复记录已保留：'+(result.stdout+result.stderr).strip())


def main():
    parser=argparse.ArgumentParser(description='为指定 Wine 应用启用跟随光标的输入模式，可恢复原值')
    parser.add_argument('--prefix',type=Path,required=True,help='已有 WINEPREFIX 目录')
    parser.add_argument('--app',required=True,help='应用可执行文件名，例如 notepad.exe')
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--apply',action='store_true')
    mode.add_argument('--restore',action='store_true')
    mode.add_argument('--dry-run',action='store_true')
    args=parser.parse_args()
    app=args.app
    if not re.fullmatch(r'[^\\/\x00-\x1f]+\.exe',app,re.I) or app in ('.exe','..exe'):
        raise RuntimeError('--app 只接受单个 .exe 文件名，不能包含路径或控制字符')
    prefix=args.prefix.expanduser().resolve()
    if not (prefix/'system.reg').is_file() or not (prefix/'user.reg').is_file():
        raise RuntimeError('请指定已有 Wine 配置目录；此工具不会自动创建或切换图形后端')
    env=dict(os.environ,WINEPREFIX=str(prefix),WINEDEBUG='-all',LC_ALL='C.UTF-8')
    key=r'HKCU\Software\Wine\AppDefaults'+'\\'+app+r'\X11 Driver'
    identity=hashlib.sha256((str(prefix)+'\0'+app.casefold()).encode()).hexdigest()
    directory=Path(os.environ.get('XDG_STATE_HOME') or Path.home()/'.local/state')/'tipe/wine-input'
    for parent in [directory,*directory.parents]:
        if parent.is_symlink():raise RuntimeError('配置恢复目录不能通过符号链接写入')
    directory.mkdir(parents=True,exist_ok=True,mode=0o700)
    os.chmod(directory,0o700)
    state=directory/(identity+'.json');lock=directory/(identity+'.lock')
    if state.is_symlink() or lock.is_symlink():raise RuntimeError('拒绝通过符号链接读取或锁定恢复记录')
    descriptor=os.open(lock,os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
    with os.fdopen(descriptor,'w') as handle:
        fcntl.flock(handle,fcntl.LOCK_EX)
        current=read_value(env,key)
        global_before=read_value(env,GLOBAL_KEY)
        record=json.loads(state.read_text()) if state.exists() else None
        if record and (record.get('prefix')!=str(prefix) or record.get('app','').casefold()!=app.casefold()):
            raise RuntimeError('恢复记录与目标应用不一致，未修改配置')
        if args.apply:
            if record and record['phase'] in ('prepared','applied'):
                if current not in (record['before'],'overthespot'):
                    raise RuntimeError('应用配置已被修改，保留当前值和恢复记录')
            else:
                record=dict(version=1,prefix=str(prefix),app=app,key=key,before=current,
                            global_before=global_before,phase='prepared')
                atomic_write(state,(json.dumps(record,ensure_ascii=False,indent=2)+'\n').encode())
            if current!='overthespot':write_value(env,key,'overthespot')
            if read_value(env,key)!='overthespot':raise RuntimeError('未读回目标值，恢复记录已保留')
            record['phase']='applied'
            atomic_write(state,(json.dumps(record,ensure_ascii=False,indent=2)+'\n').encode())
            action='已启用；关闭并重新打开此 Windows 应用后生效'
        elif args.restore:
            if not record or record['phase']=='restored':
                action='没有待恢复的配置'
            else:
                if current not in ('overthespot',record['before']):
                    raise RuntimeError('应用配置已被修改，未覆盖用户的新值；恢复记录已保留')
                if current!=record['before']:write_value(env,key,record['before'])
                if read_value(env,key)!=record['before']:raise RuntimeError('恢复后读回不一致，记录已保留')
                record['phase']='restored'
                atomic_write(state,(json.dumps(record,ensure_ascii=False,indent=2)+'\n').encode())
                action='已恢复原配置'
        else:
            action='预览：只会设置此应用的 InputStyle=overthespot' if args.dry_run else '当前状态'
        if read_value(env,GLOBAL_KEY)!=global_before:raise RuntimeError('运行期间 Wine 全局值变化，请检查其他配置操作；本工具未写全局值')
        print(json.dumps(dict(status=action,prefix=str(prefix),app=app,key=key,
                              input_style=read_value(env,key),global_unchanged=True,
                              recovery=str(state) if state.exists() else None),ensure_ascii=False,indent=2))
    return 0


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,ValueError,subprocess.TimeoutExpired) as exc:
        print(str(exc),file=sys.stderr);sys.exit(1)
