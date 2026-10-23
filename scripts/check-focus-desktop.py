#!/usr/bin/env python3
"""Switch only between owned GTK/Qt fixtures; preserve independent composition."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import numpy as np
from PIL import Image
from desktop_keys import send

root = Path(__file__).resolve().parent.parent
out = root / "build/focus-regression"
out.mkdir(exist_ok=True)

def windows():
    return json.loads(subprocess.check_output(["niri", "msg", "-j", "windows"]))

original = next((w["id"] for w in windows() if w["is_focused"]), None)
fixtures = []
results = []
try:
    for toolkit, x in [("gtk", 20), ("qt", 820)]:
        fixture = {"toolkit": toolkit}
        fixtures.append(fixture)
        binary = out / f"tipe-focus-{toolkit}-{time.monotonic_ns()}"
        shutil.copy2(root / f"build/tipe-{toolkit}-harness", binary)
        fixture["binary"] = binary
        fixture["log"] = open(out / f"{toolkit}.jsonl", "w")
        fixture["err"] = open(out / f"{toolkit}.err", "w")
        env = dict(os.environ, GTK_IM_MODULE="fcitx", QT_IM_MODULE="fcitx",
                   GTK_PATH=str(Path.home()/".local/share/tipe/gtk/lib/gtk-4.0"),
                   QT_PLUGIN_PATH=str(Path.home()/".local/share/tipe/qt/lib/qt6/plugins"),
                   WAYLAND_DEBUG="client", TIPE_HARNESS_TIMEOUT_MS="120000")
        p = subprocess.Popen([str(binary)], env=env, stdout=fixture["log"],
                             stderr=fixture["err"], start_new_session=True)
        fixture["process"] = p
        for _ in range(100):
            matches = [w for w in windows() if w["pid"] == p.pid]
            if matches:
                break
            assert p.poll() is None, f"{toolkit} exited"
            time.sleep(.1)
        assert len(matches) == 1
        fixture["id"] = matches[0]["id"]
        subprocess.run(["niri", "msg", "action", "move-window-to-floating",
                        "--id", str(fixture["id"])], check=True)
        for axis, size in [("width", 560), ("height", 180)]:
            subprocess.run(["niri", "msg", "action", "set-window-"+axis,
                            "--id", str(fixture["id"]), str(size)], check=True)
        subprocess.run(["niri", "msg", "action", "move-floating-window",
                        "--id", str(fixture["id"]), "-x", str(x), "-y", "220"], check=True)

    def focus(f):
        subprocess.run(["niri", "msg", "action", "focus-window", "--id", str(f["id"])], check=True)
        time.sleep(.25)

    def keys(f, *args):
        send(f["process"].pid, args)
        time.sleep(.2)

    def rows(f):
        return [json.loads(line) for line in (out / (f["toolkit"]+".jsonl")).read_text().splitlines()
                if line.startswith("{")]

    def value(f):
        return next((r["value"] for r in reversed(rows(f)) if r.get("event") == "text"), "")

    def preedit(f):
        for r in reversed(rows(f)):
            if f["toolkit"] == "gtk" and r.get("event") == "preedit":
                return r["text"]
            if f["toolkit"] == "qt" and r.get("event") == "input_method":
                return r["preedit"]
        return ""

    def visible(f, name):
        path = out / (name+".png")
        subprocess.run(["grim", str(path)], check=True)
        a = np.array(Image.open(path).convert("RGB")).astype(int)
        m = (abs(a[:,:,0]-10)<3) & (abs(a[:,:,1]-132)<3) & (a[:,:,2]>250)
        ys = np.where(m.sum(1)>=12)[0]
        assert len(ys)>=10, "No visible candidate after focus switch"
        y = int(ys[len(ys)//2])
        xs = np.where(m[y])[0]
        runs = np.split(xs, np.where(np.diff(xs)>1)[0]+1)
        run = max(runs, key=len)
        px = int(run[len(run)//2])
        # The two fixture windows are widely separated. Compare the native
        # popup's actual selection to the focused fixture, not its old owner.
        outputs = json.loads(subprocess.check_output(["niri","msg","-j","outputs"]))
        active = [o["logical"] for o in outputs.values() if o.get("logical")]
        assert len(active)==1
        scale = active[0]["scale"]
        owner = next(w for w in windows() if w["id"]==f["id"])
        x0,y0 = owner["layout"]["tile_pos_in_workspace_view"]
        width,height = owner["layout"]["window_size"]
        assert x0 <= px/scale <= x0+width, (f["toolkit"], px/scale, x0, width)
        assert y0 <= y/scale <= y0+height+330, (f["toolkit"], y/scale, y0, height)
        return [px, y]

    a,b = fixtures
    for f in fixtures:
        focus(f)
        subprocess.run(["fcitx5-remote","-s","tipe"],check=True)
        subprocess.run(["fcitx5-remote","-o"],check=True)
    focus(a)
    keys(a, "-d", "80", "nihao", "-k", "Down")
    assert preedit(a)=="nihao"
    visible(a, "gtk-initial")
    focus(b)
    keys(b, "-d", "80", "shijie", "-k", "Down")
    assert preedit(b)=="shijie"
    visible(b, "qt-initial")
    for f, spelling in [(a,"nihao"),(b,"shijie")]:
        focus(f)
        before = {"committed":value(f),"preedit":preedit(f)}
        # Preserve the spelling either as client preedit or a lossless literal
        # flush at the toolkit's focus-out boundary; never accept empty loss.
        assert before["preedit"]==spelling or before["committed"]==spelling, before
        if before["preedit"]:
            keys(f, "-k", "Down")
            location = visible(f, f["toolkit"]+"-returned")
            keys(f, "-k", "Return")
        else:
            location = None
        actual = value(f)
        result = dict(toolkit=f["toolkit"], before=before, expected=spelling,
                      actual=actual, selection=location, passed=actual==spelling)
        results.append(result)
        assert result["passed"], result
        # First popup after returning must anchor to the newly focused
        # client, including when the old composition was flushed locally.
        keys(f, "nihao", "-k", "Down")
        location = visible(f, f["toolkit"]+"-new-composition")
        keys(f, "-k", "Return")
        expected = actual + "nihao"
        assert value(f) == expected, (f["toolkit"], value(f), expected)
        results.append(dict(toolkit=f["toolkit"], check="popup-after-focus",
                            selection=location, passed=True))

        # An explicit switch to the English keyboard is distinct from focus
        # loss: TiPE still must flush unfinished spelling exactly once.
        keys(f, "nihao", "-M", "ctrl", "-k", "space", "-m", "ctrl")
        expected += "nihao"
        assert value(f) == expected, (f["toolkit"], "switch-out", value(f), expected)
        keys(f, "abc")
        expected += "abc"
        assert value(f) == expected, (f["toolkit"], "english", value(f), expected)
        keys(f, "-M", "ctrl", "-k", "space", "-m", "ctrl")
        keys(f, "shijie", "-k", "Return")
        expected += "shijie"
        assert value(f) == expected, (f["toolkit"], "switch-back", value(f), expected)
        results.append(dict(toolkit=f["toolkit"], check="english-switch-roundtrip",
                            actual=value(f), expected=expected, passed=True))
    print("GTK/Qt focus roundtrip preserved each spelling without cross-client leakage")
finally:
    (out/"results.json").write_text(json.dumps(results,ensure_ascii=False,indent=2)+"\n")
    owned_ids = {f.get("id") for f in fixtures}
    restore = any(w["id"] in owned_ids and w["is_focused"] for w in windows())
    for f in reversed(fixtures):
        if p := f.get("process"):
            p.terminate()
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait()
        for key in ["log","err"]:
            if stream := f.get(key):stream.close()
        if binary := f.get("binary"):binary.unlink(missing_ok=True)
    if restore and original is not None:
        subprocess.run(["niri","msg","action","focus-window","--id",str(original)],check=False)
