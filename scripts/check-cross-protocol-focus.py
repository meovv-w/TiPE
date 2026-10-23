#!/usr/bin/env python3
"""Owned native Chromium/foot/XIM fixtures, actual keys, no shell input."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np
from PIL import Image
from desktop_keys import send

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/cross-protocol-focus"
OUT.mkdir(parents=True, exist_ok=True)
TOKEN = str(time.monotonic_ns())


def windows():
    return json.loads(subprocess.check_output(["niri", "msg", "-j", "windows"]))


def action(*args):
    subprocess.run(["niri", "msg", "action", *args], check=True)


original = next((w["id"] for w in windows() if w["is_focused"]), None)
fixtures = []
results = []
profile = OUT / ("chromium-" + TOKEN)
wine_env = dict(os.environ, WINEPREFIX=str(OUT / "wine-prefix"), WINEDEBUG="-all",
                XMODIFIERS="@im=fcitx", WINEDLLOVERRIDES="winemenubuilder.exe=d")


def launch(name, command, env, position, title=None):
    fixture = dict(name=name, env=env)
    fixtures.append(fixture)
    before = {w["id"] for w in windows()}
    fixture["log"] = (OUT / (name + ".log")).open("w")
    p = subprocess.Popen(command, env=env, cwd=OUT, stdout=fixture["log"],
                         stderr=fixture["log"], start_new_session=True)
    fixture["process"] = p
    for _ in range(180):
        found = [w for w in windows() if w["id"] not in before and
                 (w["title"] == title if title else w["pid"] == p.pid)]
        if found:
            break
        if not title:
            assert p.poll() is None, (name, "process exited")
        time.sleep(.1)
    assert len(found) == 1, (name, found)
    fixture["id"] = found[0]["id"]
    action("move-window-to-floating", "--id", str(fixture["id"]))
    for axis, size in [("width", 640), ("height", 290)]:
        action("set-window-" + axis, "--id", str(fixture["id"]), str(size))
    action("move-floating-window", "--id", str(fixture["id"]),
           "-x", str(position[0]), "-y", str(position[1]))
    return fixture


def focus(f):
    action("focus-window", "--id", str(f["id"]))
    time.sleep(.3)


def keys(f, *args):
    send(None, args, window_id=f["id"])
    time.sleep(.25)


def cdp(expression):
    return json.loads(subprocess.check_output(
        ["node", str(ROOT / "scripts/browser-cdp.mjs"), str(profile), expression], text=True))


def value(f):
    if f["name"] == "browser":
        return cdp("document.querySelector('textarea').value")
    path = OUT / ("foot-text.json" if f["name"] == "foot" else "text.utf8")
    return json.loads(path.read_text()) if f["name"] == "foot" else path.read_text()


def visible(f, stage):
    path = OUT / (f["name"] + "-" + stage + ".png")
    subprocess.run(["grim", str(path)], check=True)
    a = np.array(Image.open(path).convert("RGB")).astype(int)
    mask = (abs(a[:, :, 0] - 10) < 3) & (abs(a[:, :, 1] - 132) < 3) & (a[:, :, 2] > 250)
    ys = np.where(mask.sum(axis=1) >= 12)[0]
    assert len(ys) >= 10, (f["name"], stage, "candidate missing")
    bands = np.split(ys, np.where(np.diff(ys) > 8)[0] + 1)
    assert len(bands) == 1, (f["name"], stage, "multiple candidate windows", len(bands))
    y = int(ys[len(ys) // 2])
    xs = np.where(mask[y])[0]
    runs = np.split(xs, np.where(np.diff(xs) > 1)[0] + 1)
    run = max(runs, key=len)
    x = int(run[len(run) // 2])
    outputs = json.loads(subprocess.check_output(["niri", "msg", "-j", "outputs"]))
    active = [o["logical"] for o in outputs.values() if o.get("logical")]
    assert len(active) == 1
    scale = active[0]["scale"]
    owner = next(w for w in windows() if w["id"] == f["id"])
    left, top = owner["layout"]["tile_pos_in_workspace_view"]
    width, height = owner["layout"]["window_size"]
    # An edge-flipped popup can extend left of a rightmost client. The
    # fixtures have distinct vertical anchors as well as horizontal positions.
    assert left - 180 <= x / scale <= left + width, (f["name"], x / scale, left)
    assert top <= y / scale <= top + height + 150, (f["name"], y / scale, top)
    return [x, y]


try:
    page = OUT / "input.html"
    page.write_text("""<!doctype html><meta charset=utf-8><title>TiPE owned browser focus test</title>
<style>body{padding:16px}textarea{font:24px sans-serif;width:90%;height:160px}</style>
<textarea autofocus></textarea><script>window.events=[];const e=document.querySelector('textarea');
for(const type of ['input','compositionstart','compositionupdate','compositionend'])
e.addEventListener(type,x=>events.push({type,data:x.data,value:e.value}));</script>""")
    browser = launch("browser", [shutil.which("chromium-browser") or "chromium",
        "--user-data-dir=" + str(profile), "--class=tipe-focus-browser-" + TOKEN,
        "--no-first-run", "--no-default-browser-check", "--disable-background-networking",
        "--disable-extensions", "--remote-debugging-port=0", "--ozone-platform=wayland",
        "--enable-wayland-ime", "--wayland-text-input-version=3", "--app=" + page.as_uri()],
        dict(os.environ), (20, 140))
    reader = OUT / "read-raw.py"
    reader.write_text("""import codecs,json,os,pathlib,sys,tty
target=pathlib.Path(sys.argv[1]);target.write_text(json.dumps(''))
tty.setcbreak(sys.stdin.fileno());decoder=codecs.getincrementaldecoder('utf-8')();text=''
print('TiPE owned input reader (all keys are data)',flush=True)
while True:
 data=os.read(sys.stdin.fileno(),256)
 if not data:break
 text+=decoder.decode(data);temp=target.with_suffix('.tmp')
 temp.write_text(json.dumps(text,ensure_ascii=False));temp.replace(target)
""")
    foot = launch("foot", ["foot", "--app-id=tipe-focus-foot-" + TOKEN,
        "--title=TiPE owned terminal focus test", "python3", "-u", str(reader),
        str(OUT / "foot-text.json")], dict(os.environ), (850, 140))
    subprocess.run(["x86_64-w64-mingw32-gcc", str(ROOT / "tests/real_wine_harness.c"),
                    "-municode", "-mwindows", "-o", str(OUT / "input.exe")], check=True)
    subprocess.run(["wine", "reg", "add", r"HKCU\Software\Wine\Drivers", "/v",
                    "Graphics", "/d", "x11", "/f"], env=wine_env, check=True, timeout=60,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (OUT / "text.utf8").unlink(missing_ok=True)
    wine = launch("wine", ["wine", str(OUT / "input.exe")], wine_env, (450, 620),
                  title="TiPE Wine isolated regression")
    for f in fixtures:
        focus(f)
        subprocess.run(["fcitx5-remote", "-s", "tipe"], check=True)
        subprocess.run(["fcitx5-remote", "-o"], check=True)
        if f is browser:
            cdp("document.querySelector('textarea').focus();true")
    spellings = {"browser": "nihao", "foot": "shijie", "wine": "tianqi"}
    for f in fixtures:
        focus(f)
        assert value(f) == "", (f["name"], "initial text", value(f))
        keys(f, spellings[f["name"]], "-k", "Down")
        visible(f, "initial")
        (OUT / (f["name"] + "-initial-contexts.txt")).write_bytes(
            subprocess.check_output(["busctl", "--user", "call", "org.fcitx.Fcitx5",
                                     "/controller", "org.fcitx.Fcitx.Controller1", "DebugInfo"]))
    for f in fixtures:
        focus(f)
        spelling = spellings[f["name"]]
        before = value(f)
        (OUT / (f["name"] + "-returned-contexts.txt")).write_bytes(
            subprocess.check_output(["busctl", "--user", "call", "org.fcitx.Fcitx5",
                                     "/controller", "org.fcitx.Fcitx.Controller1", "DebugInfo"]))
        # A terminal has no DOM/preedit introspection. If the client retained
        # composition on focus-out, Enter must flush it here. Otherwise the
        # original spelling must already have reached the fixture once.
        if before == "":
            keys(f, "-k", "Return")
        actual = value(f)
        results.append(dict(client=f["name"], check="focus-loss-and-return",
                            before=before, actual=actual, expected=spelling,
                            passed=actual == spelling))
        assert actual == spelling, results[-1]
        keys(f, "gongzuo", "-k", "Down")
        location = visible(f, "returned")
        keys(f, "-k", "Return")
        expected = spelling + "gongzuo"
        assert value(f) == expected, (f["name"], value(f), expected)
        results.append(dict(client=f["name"], check="first-popup-after-return",
                            selection=location, passed=True))
        keys(f, "xuexi", "-M", "ctrl", "-k", "space", "-m", "ctrl")
        expected += "xuexi"
        assert value(f) == expected, (f["name"], "switch-out", value(f), expected)
        keys(f, "abc", "-M", "ctrl", "-k", "space", "-m", "ctrl")
        keys(f, "mingtian", "-k", "Return")
        expected += "abcmingtian"
        assert value(f) == expected, (f["name"], "english-roundtrip", value(f), expected)
        results.append(dict(client=f["name"], check="english-roundtrip",
                            actual=value(f), expected=expected, passed=True))
        f["baseline"] = expected
    # Repeat after every client has changed purpose/capabilities. This also
    # catches stale terminal flags accidentally making a browser restore an
    # already client-committed composition on a subsequent focus roundtrip.
    for f in fixtures:
        focus(f)
        keys(f, "zhongwen", "-k", "Down")
        visible(f, "second-cycle")
    for f in fixtures:
        focus(f)
        before = value(f)
        if before == f["baseline"]:
            keys(f, "-k", "Return")
        expected = f["baseline"] + "zhongwen"
        actual = value(f)
        results.append(dict(client=f["name"], check="second-focus-cycle",
                            before=before, actual=actual, expected=expected,
                            passed=actual == expected))
        assert actual == expected, results[-1]
    (OUT / "browser-events.json").write_text(json.dumps(cdp("events"), ensure_ascii=False, indent=2))
    print("PASS Chromium/foot/Wine:", len(results), "cross-protocol focus and switch checks")
finally:
    (OUT / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
    restore = any(w["is_focused"] and w["id"] in {f.get("id") for f in fixtures} for w in windows())
    for f in reversed(fixtures):
        if f["name"] == "wine":
            subprocess.run(["wineserver", "-k"], env=wine_env, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if p := f.get("process"):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()
        if log := f.get("log"):
            log.close()
    shutil.rmtree(profile, ignore_errors=True)
    if restore and original is not None:
        action("focus-window", "--id", str(original))
