#!/usr/bin/env python3
"""Exercise editing contracts through installed GTK/Qt, Wayland, and XIM.

Only owned fixture windows receive physical keys. Expected strings describe
user-visible editing, independent of candidate implementation. No learning
files are cleared, and browser DOM access only observes/focuses the fixture.
Run this sequentially with other desktop checks.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from desktop_keys import send

ROOT = Path(__file__).resolve().parent.parent
KEY = lambda name: ('-k', name)
CTRL_A = ('-M', 'ctrl', 'a', '-m', 'ctrl')
SHIFT_TAB = ('-M', 'shift', '-k', 'Tab', '-m', 'shift')

# Each row states a concrete editing contract and final client string. All
# composition input and client editing go through the physical keyboard path.
CASES = [
    ('Enter preserves pinyin', ('nihao', *KEY('Return')), 'nihao'),
    ('Space selects Chinese', ('nihao', *KEY('space')), '你好'),
    ('Escape cancels without committing', ('nihao', *KEY('Escape')), ''),
    ('Insert inside preedit', ('nihao', *KEY('Home'), *KEY('Right'), 'x', *KEY('Return')), 'nxihao'),
    ('Delete at preedit beginning', ('nihao', *KEY('Home'), *KEY('Delete'), *KEY('Return')), 'ihao'),
    ('Backspace at preedit end', ('nihao', *KEY('Home'), *KEY('End'), *KEY('BackSpace'), *KEY('Return')), 'niha'),
    ('Backspace inside preedit', ('nihao', *KEY('Home'), *KEY('Right'), *KEY('BackSpace'), *KEY('Return')), 'ihao'),
    ('Delete at preedit end is harmless', ('nihao', *KEY('Delete'), *KEY('Return')), 'nihao'),
    ('Shift roundtrip retains insertion position', ('nihao', *KEY('Home'), *KEY('Right'), *KEY('Shift_L'), *KEY('Shift_L'), 'x', *KEY('Return')), 'nxihao'),
    ('Shift roundtrip keeps Chinese selection', ('nihao', *KEY('Shift_L'), *KEY('Shift_L'), *KEY('space')), '你好'),
    ('Tab expansion collapses to editable pinyin', ('nihao', *KEY('Tab'), *SHIFT_TAB, *KEY('Home'), *KEY('Delete'), *KEY('Return')), 'ihao'),
    ('Expanded arrows do not change Enter spelling', ('nihao', *KEY('Tab'), *KEY('Right'), *KEY('Down'), *KEY('Up'), *KEY('Return')), 'nihao'),
    ('Pagination does not change Enter spelling', ('nihao', *KEY('Tab'), *KEY('Page_Down'), *KEY('Page_Up'), *KEY('Return')), 'nihao'),
    ('Numeric first candidate selection', ('nihao', '1'), '你好'),
    ('Long spelling survives expand and collapse', ('zhonghuarenmingongheguo', *KEY('Tab'), *SHIFT_TAB, *KEY('Return')), 'zhonghuarenmingongheguo'),
    ('Selected client text replaced by Chinese', ('nihao', *KEY('Return'), *CTRL_A, 'shijie', *KEY('space')), '世界'),
    ('Chinese inserts at client caret', ('nihao', *KEY('Return'), *KEY('Home'), *KEY('Right'), *KEY('Right'), 'shijie', *KEY('space')), 'ni世界hao'),
    ('Moved client caret invalidates restoration', ('nihao', *KEY('space'), *KEY('Left'), *KEY('BackSpace')), '好'),
    ('Committed text restores to editable pinyin', ('nihao', *KEY('space'), *KEY('BackSpace'), *KEY('Return')), 'nihao'),
    ('Consecutive Chinese chunks restore only last', ('nihao', *KEY('space'), 'shijie', *KEY('space'), *KEY('BackSpace'), *KEY('Return')), '你好shijie'),
    ('New composition after cancellation', ('nihao', *KEY('Tab'), *KEY('Escape'), 'shijie', *KEY('space')), '世界'),
]


def windows():
    return json.loads(subprocess.check_output(['niri', 'msg', '-j', 'windows']))


def run(toolkit, out, results, matrix='editing'):
    directory = out / toolkit
    directory.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, GTK_IM_MODULE='fcitx', QT_IM_MODULE='fcitx',
               GTK_PATH=str(Path.home()/'.local/share/tipe/gtk/lib/gtk-4.0'),
               QT_PLUGIN_PATH=str(Path.home()/'.local/share/tipe/qt/lib/qt6/plugins'),
               TIPE_HARNESS_TIMEOUT_MS='300000')
    original = next((w['id'] for w in windows() if w['is_focused']), None)
    log = open(directory/'events.jsonl', 'w')
    err = open(directory/'stderr.log', 'w')
    profile = None
    binary = None
    process = None
    window_id = None
    wine_env = None
    text_file = directory/'text.utf8'
    lines_file = directory/'lines.json'
    existing_ids = {w['id'] for w in windows()}
    try:
        if toolkit == 'browser':
            profile = Path(tempfile.mkdtemp(prefix='profile-', dir=directory))
            page = directory/'input.html'
            page.write_text('''<!doctype html><meta charset=utf-8>
<title>TiPE isolated editing regression</title><textarea autofocus style="width:90%;height:200px;font:24px sans-serif"></textarea>
<script>window.events=[];const e=document.querySelector('textarea');
for(const t of ['input','compositionstart','compositionupdate','compositionend'])
e.addEventListener(t,x=>events.push({type:t,data:x.data,value:e.value}));</script>''')
            command = [shutil.which('chromium-browser') or 'chromium',
                       '--user-data-dir='+str(profile), '--no-first-run',
                       '--no-default-browser-check', '--disable-background-networking',
                       '--disable-extensions', '--remote-debugging-port=0',
                       '--ozone-platform=wayland', '--enable-wayland-ime',
                       '--wayland-text-input-version=3', '--app='+page.as_uri()]
        elif toolkit == 'wine':
            binary = directory/'input.exe'
            subprocess.run(['x86_64-w64-mingw32-gcc', str(ROOT/'tests/real_wine_harness.c'),
                            '-municode', '-mwindows', '-o', str(binary)], check=True)
            prefix = directory/'prefix'
            prefix.mkdir(exist_ok=True)
            env.update(WINEPREFIX=str(prefix), WINEDEBUG='-all', XMODIFIERS='@im=fcitx',
                       WINEDLLOVERRIDES='winemenubuilder.exe=d')
            wine_env = env
            subprocess.run(['wine', 'reg', 'add', r'HKCU\Software\Wine\Drivers', '/v',
                            'Graphics', '/d', 'x11', '/f'], env=env, check=True,
                           stdout=err, stderr=err, timeout=60)
            style = os.environ.get('TIPE_WINE_INPUT_STYLE')
            if style is not None:
                assert style in ('default','overthespot','root')
                args = (['delete',r'HKCU\Software\Wine\X11 Driver','/v','InputStyle','/f']
                        if style=='default' else
                        ['add',r'HKCU\Software\Wine\X11 Driver','/v','InputStyle','/d',style,'/f'])
                subprocess.run(['wine','reg',*args],env=env,check=style!='default',
                               stdout=err,stderr=err,timeout=60)
            text_file.unlink(missing_ok=True)
            command = ['wine', str(binary)]
        elif toolkit == 'foot':
            reader = directory/'reader.py'
            reader.write_text('''import json,pathlib,sys
target=pathlib.Path(sys.argv[1]);lines=[]
print('TiPE isolated editing regression; input is data',flush=True)
for line in sys.stdin:
 lines.append(line.rstrip('\\n'))
 target.write_text(json.dumps(lines,ensure_ascii=False))
''')
            lines_file.unlink(missing_ok=True)
            command = ['foot', '--title=TiPE isolated editing regression',
                       'python3', '-u', str(reader), str(lines_file)]
        else:
            binary = directory/f'tipe-editing-{toolkit}-{time.monotonic_ns()}'
            shutil.copy2(ROOT/f'build/tipe-{toolkit}-harness', binary)
            command = [str(binary)]
        process = subprocess.Popen(command, env=env, cwd=directory, stdout=log, stderr=err,
                                   start_new_session=True)
        for _ in range(100):
            assert process.poll() is None, f'{toolkit} fixture exited'
            matches = [w for w in windows() if w['id'] not in existing_ids and
                       (w['pid'] == process.pid if toolkit != 'wine'
                        else w['title'] == 'TiPE Wine isolated regression')]
            if matches and (profile is None or (profile/'DevToolsActivePort').exists()) and (toolkit != 'wine' or text_file.exists()):
                break
            time.sleep(.1)
        assert len(matches) == 1, f'{toolkit}: owned fixture missing'
        window_id = matches[0]['id']
        subprocess.run(['niri','msg','action','focus-window','--id',str(window_id)], check=True)
        subprocess.run(['fcitx5-remote','-s','tipe'], check=True)
        subprocess.run(['fcitx5-remote','-o'], check=True)
        time.sleep(.3)

        def cdp(expression):
            return json.loads(subprocess.check_output(
                ['node', str(ROOT/'scripts/browser-cdp.mjs'), str(profile), expression], text=True))

        def rows():
            if toolkit == 'browser':
                return cdp('events')
            if toolkit in ('wine', 'foot'):
                return []
            return [json.loads(line) for line in (directory/'events.jsonl').read_text().splitlines()
                    if line.startswith('{')]

        def value():
            if toolkit == 'browser':
                return cdp('document.querySelector("textarea").value')
            if toolkit == 'wine':
                return text_file.read_text()
            if toolkit == 'foot':
                lines = json.loads(lines_file.read_text()) if lines_file.exists() else []
                return lines[-1] if lines else ''
            return next((r['value'] for r in reversed(rows()) if r.get('event') == 'text'), '')

        def keys(args):
            assert process.poll() is None, 'Fixture exited'
            send(process.pid if toolkit != 'wine' else None, args, window_id=window_id)
            time.sleep(.18)

        if toolkit == 'browser':
            cdp('document.querySelector("textarea").focus();true')
        elif toolkit in ('gtk', 'qt'):
            # Verify the client really loaded the installed plugin, including
            # inode identity, rather than a stale staged test library.
            module = (Path.home()/'.local/share/tipe/gtk/lib/gtk-4.0/4.0.0/immodules/libim-fcitx5.so'
                      if toolkit == 'gtk' else
                      Path.home()/'.local/share/tipe/qt/lib/qt6/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so')
            inode = module.stat().st_ino
            mapped = Path(f'/proc/{process.pid}/maps').read_text().splitlines()
            assert any(len(p := row.split(maxsplit=5)) == 6 and int(p[4]) == inode
                       and p[5] == str(module) for row in mapped), f'{toolkit}: installed plugin not mapped'

        def clear():
            if toolkit == 'wine':
                keys((*KEY('Escape'), *KEY('End'), '-M', 'shift', *KEY('Home'), '-m', 'shift', *KEY('BackSpace')))
            elif toolkit != 'foot':
                keys((*KEY('Escape'), *CTRL_A, *KEY('BackSpace')))
            if toolkit != 'foot':
                assert value() == '', f'{toolkit}: cannot clear owned fixture'

        def record(result):
            results.append(result)
            (out/'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
            print(f'{toolkit}: {result["name"]}: {"PASS" if result["passed"] else "FAIL"}', flush=True)

        def verify_protocol():
            if toolkit != 'browser':
                return
            errors = [line for line in (directory/'stderr.log').read_text().splitlines()
                      if 'The selection range for surrounding text is invalid.' in line
                      or 'The deletion range needs to cover whole selection range.' in line]
            for result in results:
                if result['toolkit'] == 'browser':
                    result['protocol_errors'] = errors
                    result['passed'] = result['passed'] and not errors
            (out/'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
            assert not errors, ('Browser rejected a Wayland deletion transaction', errors)

        if matrix in ('segmented', 'chain-learning'):
            from segmented_cases import execute, execute_learning

            def checkpoint(committed, remaining):
                actual = value()
                if toolkit == 'foot':
                    # Canonical terminal input is delivered after Return.
                    # This frontend has final-line evidence only.
                    return dict(passed=True, scope='final-line-only')
                if toolkit == 'wine':
                    return dict(expected=committed, actual=actual, passed=actual == committed,
                                scope='client-committed-only')
                if toolkit == 'browser':
                    events = rows()
                    composition = next((r for r in reversed(events)
                                        if r['type'] in ('compositionstart', 'compositionupdate', 'compositionend')), None)
                    preedit = composition.get('data') or '' if composition and composition['type'] != 'compositionend' else ''
                    expected_value = committed + remaining
                else:
                    preedit = next((r.get('text', '') if toolkit == 'gtk' else r.get('preedit', '')
                                    for r in reversed(rows()) if r.get('event') ==
                                    ('preedit' if toolkit == 'gtk' else 'input_method')), '')
                    expected_value = committed
                return dict(expected_value=expected_value, actual_value=actual,
                            expected_preedit=remaining, actual_preedit=preedit,
                            passed=actual == expected_value and preedit == remaining)

            def finish():
                if toolkit == 'foot':
                    keys(KEY('Return'))
                return value()

            if matrix == 'chain-learning':
                execute_learning(toolkit, binary.name, keys, clear, checkpoint, finish, record)
            else:
                execute(toolkit, keys, clear, checkpoint, finish, record)
            verify_protocol()
            return

        cases = CASES
        if toolkit == 'foot':
            # The terminal owns canonical line editing. Test the IME's
            # composition contracts without inventing GUI selection support.
            cases = CASES[:15] + [CASES[-1]]
        elif toolkit == 'wine':
            # Standard XIM has no surrounding-text reply. It must leave a
            # Backspace to EDIT rather than attempt unverified restoration.
            cases = CASES[:15] + [CASES[16], CASES[17], CASES[-1],
                ('XIM unavailable surrounding text uses client deletion',
                 ('nihao', *KEY('space'), *KEY('BackSpace')), '你')]
        for name, sequence, expected in cases:
            clear()
            start = len(rows())
            keys(sequence)
            if toolkit == 'foot':
                # One additional Return delivers the resulting literal line
                # to Python; it is never interpreted as a shell command.
                keys(KEY('Return'))
            actual = value()
            result = dict(toolkit=toolkit, name=name, expected=expected, actual=actual,
                          passed=actual == expected, events=rows()[start:])
            if toolkit=='wine': result['input_style']=os.environ.get('TIPE_WINE_INPUT_STYLE','prefix-existing')
            record(result)
            assert result['passed'], (toolkit, name, actual, expected)
        verify_protocol()
    finally:
        restore = window_id is not None and any(w['id'] == window_id and w['is_focused'] for w in windows())
        if process is not None:
            if wine_env is not None:
                subprocess.run(['wineserver', '-k'], env=wine_env, check=False,
                               stdout=err, stderr=err)
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        log.close()
        err.close()
        if binary is not None:
            binary.unlink(missing_ok=True)
        if profile is not None:
            shutil.rmtree(profile, ignore_errors=True)
        if restore and original is not None:
            subprocess.run(['niri','msg','action','focus-window','--id',str(original)], check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--matrix', choices=['editing', 'segmented', 'chain-learning'], default='editing')
    parser.add_argument('toolkits', nargs='*', choices=['gtk', 'qt', 'browser', 'wine', 'foot'])
    args = parser.parse_args()
    if args.matrix == 'chain-learning' and any(t not in ('gtk', 'qt') for t in args.toolkits):
        parser.error('chain-learning requires a GTK or Qt fixture with a unique application identity')
    out = (args.output or ROOT/('build/'+args.matrix+'-regression')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = []
    defaults = ['gtk', 'qt'] if args.matrix == 'chain-learning' else ['gtk', 'qt', 'browser', 'wine', 'foot']
    for toolkit in args.toolkits or defaults:
        run(toolkit, out, results, args.matrix)
    print(json.dumps(dict(passed=len(results), results=str(out/'results.json')), ensure_ascii=False))


if __name__ == '__main__':
    main()
