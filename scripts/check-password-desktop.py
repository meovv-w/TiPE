#!/usr/bin/env python3
"""Synthetic password fixtures: real protocols, redacted output, local model check."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from desktop_keys import send

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/password-regression"
OUT.mkdir(parents=True, exist_ok=True)
TOKEN = str(time.monotonic_ns())
profile = OUT / ("browser-" + TOKEN)
results = []
current = None
streams = []
wine_env = None
restore_original = False


def windows():
    return json.loads(subprocess.check_output(["niri", "msg", "-j", "windows"]))


def contexts():
    response = subprocess.check_output(["busctl", "--user", "--json=short", "call",
        "org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1", "DebugInfo"], text=True)
    text = json.loads(response)["data"][0]
    return [dict(uuid=m[0], program=m[1], frontend=m[2], capabilities=int(m[3], 16),
                 focused=m[4] == "1") for m in re.findall(
                     r"IC \[([0-9a-f]+)\] program:(.*?) frontend:(\S+) cap:([0-9a-f]+) focus:([01])", text)]


def cdp(expression):
    return json.loads(subprocess.check_output(["node", str(ROOT / "scripts/browser-cdp.mjs"),
                                              str(profile), expression], text=True))


def state_dir():
    answer = subprocess.check_output(["busctl", "--user", "call", "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixProcessID",
        "s", "org.fcitx.Fcitx5"], text=True)
    pid = int(answer.split()[1])
    # Read only the two path variables; never print the daemon environment.
    items = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
    paths = {k.decode(): v.decode() for item in items if b"=" in item
             for k, v in [item.split(b"=", 1)] if k in (b"HOME", b"XDG_STATE_HOME")}
    return Path(paths.get("XDG_STATE_HOME") or str(Path(paths["HOME"]) / ".local/state")) / "tipe"


STATE = state_dir()


def observed():
    fields = (STATE / "state.current").read_text().split("\t")
    assert fields[8:10] == ["0", "1"], "Learning must be enabled and unpaused for this check"
    return int(fields[6])


def no_model_token(marker):
    return all(marker.encode() not in p.read_bytes()
               for name in ("model.current", "model.previous") if (p := STATE / name).exists())


original = next((w["id"] for w in windows() if w["is_focused"]), None)
try:
    names = sys.argv[1:] or ["gtk", "qt", "browser", "wine"]
    assert all(name in ("gtk", "qt", "browser", "wine") for name in names)
    for name in names:
        old_windows = {w["id"] for w in windows()}
        old_contexts = {c["uuid"] for c in contexts()}
        env = dict(os.environ, TIPE_HARNESS_PASSWORD="1", TIPE_HARNESS_TIMEOUT_MS="60000",
                   GTK_IM_MODULE="fcitx", QT_IM_MODULE="fcitx",
                   GTK_PATH=str(Path.home() / ".local/share/tipe/gtk/lib/gtk-4.0"),
                   QT_PLUGIN_PATH=str(Path.home() / ".local/share/tipe/qt/lib/qt6/plugins"))
        copied = None
        if name in ("gtk", "qt"):
            copied = OUT / (f"privacy-{name}-" + TOKEN)
            shutil.copy2(ROOT / f"build/tipe-{name}-harness", copied)
            command = [str(copied)]
        elif name == "browser":
            env["WAYLAND_DEBUG"] = "client"
            page = OUT / "input.html"
            page.write_text("""<!doctype html><meta charset=utf-8><title>TiPE owned password test</title>
<style>body{padding:24px}input{font:24px sans-serif;width:80%;display:block;margin-bottom:12px}</style>
<input type=text autofocus autocomplete=off><input type=password autocomplete=off>""")
            command = [shutil.which("chromium-browser") or "chromium",
                "--user-data-dir=" + str(profile), "--no-first-run", "--disable-extensions",
                "--disable-background-networking", "--remote-debugging-port=0",
                "--ozone-platform=wayland", "--enable-wayland-ime",
                "--wayland-text-input-version=3", "--app=" + page.as_uri()]
        else:
            exe = OUT / "privacy.exe"
            subprocess.run(["x86_64-w64-mingw32-gcc", str(ROOT / "tests/real_wine_harness.c"),
                            "-municode", "-mwindows", "-o", str(exe)], check=True)
            wine_env = dict(env, WINEPREFIX=str(OUT / "wine-prefix"), WINEDEBUG="-all",
                            XMODIFIERS="@im=fcitx", WINEDLLOVERRIDES="winemenubuilder.exe=d")
            subprocess.run(["wine", "reg", "add", r"HKCU\Software\Wine\Drivers", "/v",
                            "Graphics", "/d", "x11", "/f"], env=wine_env, timeout=60, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            env = wine_env
            (OUT / "text.length").unlink(missing_ok=True)
            command = ["wine", str(exe)]
        stdout = (OUT / (name + ".jsonl")).open("w")
        stderr = (OUT / (name + ".err")).open("w")
        streams.extend([stdout, stderr])
        process = subprocess.Popen(command, env=env, cwd=OUT, stdout=stdout,
                                   stderr=stderr, start_new_session=True)
        current = dict(name=name, process=process, copied=copied)
        for _ in range(160):
            found = [w for w in windows() if w["id"] not in old_windows and
                     (w["title"] == "TiPE Wine isolated regression" if name == "wine"
                      else w["pid"] == process.pid)]
            if found:
                break
            if name != "wine":
                assert process.poll() is None, (name, "fixture exited")
            time.sleep(.1)
        assert len(found) == 1, (name, found)
        wid = found[0]["id"]
        current["id"] = wid
        if name == "browser":
            for command in (["move-window-to-floating", "--id", str(wid)],
                            ["set-window-width", "--id", str(wid), "640"],
                            ["set-window-height", "--id", str(wid), "240"],
                            ["move-floating-window", "--id", str(wid), "-x", "100", "-y", "200"]):
                subprocess.run(["niri", "msg", "action", *command], check=True)
        subprocess.run(["niri", "msg", "action", "focus-window", "--id", str(wid)], check=True)
        time.sleep(.3)
        if name == "browser":
            cdp("document.querySelector('input').focus();true")
        subprocess.run(["fcitx5-remote", "-s", "tipe"], check=True)
        subprocess.run(["fcitx5-remote", "-o"], check=True)
        if name == "browser":
            # Move between actual widgets with a guarded physical Tab. An
            # inactive document can retain activeElement=password; that
            # alone is not evidence that the password protocol is broken.
            assert cdp("document.hasFocus()"), "Owned browser document lost focus"
            send(None, ["-k", "Tab"], window_id=wid)
            assert cdp("document.hasFocus() && document.activeElement.type==='password'"), "Password fixture did not receive keyboard focus"
        if name == "browser":
            for _ in range(80):
                rows = [c for c in contexts() if c["uuid"] not in old_contexts]
                if (any(c["focused"] and c["capabilities"] & ((1 << 3) | (1 << 36)) for c in rows)
                        or not any(c["focused"] for c in rows)):
                    break
                time.sleep(.05)
            else:
                if name == "browser":
                    if os.environ.get("TIPE_PASSWORD_LATE_PROBE") == "1":
                        before_probe = observed()
                        send(None, ["x"], window_id=wid)
                        time.sleep(.5)
                        (OUT / "browser-late-probe.json").write_text(json.dumps(dict(
                            observed_before=before_probe, observed_after=observed(),
                            field_length=cdp("document.querySelector('input[type=password]').value.length"),
                            contexts=[c for c in contexts() if c["uuid"] not in old_contexts]), indent=2))
                    info = cdp("({type:document.querySelector('input[type=password]').type,hasFocus:document.hasFocus(),active:document.activeElement===document.querySelector('input[type=password]'),activated:navigator.userActivation.hasBeenActive,rect:document.querySelector('input[type=password]').getBoundingClientRect().toJSON()})")
                    (OUT / "browser-readiness.json").write_text(json.dumps(dict(dom=info,
                        new_contexts=rows), indent=2))
                    owner = next(w for w in windows() if w["id"] == wid)
                    x, y = owner["layout"]["tile_pos_in_workspace_view"]
                    w, h = owner["layout"]["window_size"]
                    subprocess.run(["grim", "-g", f"{int(x)},{int(y)} {int(w)}x{int(h)}",
                                    str(OUT / "browser-field.png")], check=True)
                raise AssertionError((name, "password capability was not delivered"))
        time.sleep(.5)
        module_path = None
        if name in ("gtk", "qt"):
            module_path = Path.home() / (".local/share/tipe/gtk/lib/gtk-4.0/4.0.0/immodules/libim-fcitx5.so"
                if name == "gtk" else ".local/share/tipe/qt/lib/qt6/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so")
            inode = module_path.stat().st_ino
            assert any(len(parts := row.split(maxsplit=5)) == 6 and int(parts[4]) == inode
                       and parts[5] == str(module_path)
                       for row in Path(f"/proc/{process.pid}/maps").read_text().splitlines()), "Fixture did not load installed toolkit module"
        protected_at_start = [dict(frontend=c["frontend"], capabilities=hex(c["capabilities"]),
                                   focused=c["focused"]) for c in contexts() if c["uuid"] not in old_contexts]
        before = observed()
        marker = "tipeprivacy" + name + "markertest"
        assert no_model_token(marker), (name, "marker already present")
        send(None, ["-d", "50", marker], window_id=wid)
        if name == "wine":
            # XIM has no sensitive-field hint and can keep the spelling as
            # composition even in a Win32 password EDIT. Commit literally;
            # TiPE's protocol privacy fallback must suppress all learning.
            send(None, ["-k", "Return"], window_id=wid)
        time.sleep(.5)
        rows = [c for c in contexts() if c["uuid"] not in old_contexts]
        sensitive_flag = any(c["capabilities"] & ((1 << 3) | (1 << 36)) for c in rows)
        if name in ("gtk", "qt"):
            data = [json.loads(line) for line in (OUT / (name + ".jsonl")).read_text().splitlines()
                    if line.startswith("{")]
            field_length = next(r["value_length"] for r in reversed(data) if r.get("event") == "text")
            assert all(marker not in (OUT / (name + ext)).read_text() for ext in (".jsonl", ".err"))
        elif name == "browser":
            field_length = cdp("document.querySelector('input[type=password]').value.length")
            assert cdp("document.querySelector('input[type=password]').type") == "password"
        else:
            field_length = int((OUT / "text.length").read_text())
        after = observed()
        result = dict(frontend=name, field_length=field_length, expected_length=len(marker),
                      sensitive_capability=sensitive_flag, observed_before=before,
                      observed_after=after, marker_in_model=not no_model_token(marker),
                      protected_at_start=protected_at_start,
                      capabilities=[dict(frontend=c["frontend"], capabilities=hex(c["capabilities"]),
                                         focused=c["focused"]) for c in rows])
        if module_path:
            result["installed_module_verified"] = str(module_path)
        result["passed"] = field_length == len(marker) and before == after and no_model_token(marker)
        results.append(result)
        assert result["passed"], result
        # XIM has no password capability bit; TiPE must exclude it from
        # observation. Toolkit/native fields must expose a sensitive bit.
        assert sensitive_flag or name == "wine" or not any(c["focused"] for c in rows), (name, "password protection missing")
        if name == "browser":
            # The privacy fallback must not leave ordinary text fields stuck
            # in raw mode, nor change selected text via empty acknowledgements.
            result["passed"] = False
            checks = []
            def normal_ready():
                assert cdp("document.hasFocus() && document.activeElement.type==='text'")
                for _ in range(80):
                    fresh = [c for c in contexts() if c["uuid"] not in old_contexts and c["focused"]]
                    if fresh and all(not c["capabilities"] & ((1 << 3) | (1 << 36) | (1 << 40)) for c in fresh):
                        return
                    time.sleep(.05)
                raise AssertionError("Ordinary browser field remained in privacy fallback")
            cdp("document.querySelector('input[type=text]').focus();true")
            normal_ready()
            send(None, ["nihao", "-k", "space"], window_id=wid)
            time.sleep(.3)
            assert cdp("document.querySelector('input[type=text]').value") == "你好"
            checks.append("normal Chinese input after password")
            cdp("document.querySelector('input[type=text]').select();true")
            cdp("document.querySelector('input[type=password]').focus();true")
            time.sleep(.25)
            cdp("document.querySelector('input[type=text]').focus();true")
            normal_ready()
            selection = cdp("({value:document.activeElement.value,start:document.activeElement.selectionStart,end:document.activeElement.selectionEnd})")
            assert selection == dict(value="你好", start=0, end=2), selection
            checks.append("untyped password roundtrip preserves text and selection")
            send(None, ["nihao", "-k", "space"], window_id=wid)
            time.sleep(.3)
            assert cdp("document.querySelector('input[type=text]').value") == "你好"
            checks.append("Chinese replaces selected text after roundtrip")
            assert observed() > after, "Normal browser input did not resume learning"
            assert no_model_token(marker)
            send(None, ["shijie"], window_id=wid)
            cdp("document.querySelector('input[type=password]').focus();true")
            time.sleep(.3)
            # A normal-field composition may be committed to its original
            # widget by the client, but must never move into the password.
            assert cdp("document.querySelector('input[type=password]').value.length") == len(marker)
            before_sensitive_return = observed()
            send(None, ["z"], window_id=wid)
            time.sleep(.3)
            assert cdp("document.querySelector('input[type=password]').value.length") == len(marker) + 1
            assert observed() == before_sensitive_return
            cdp("document.querySelector('input[type=text]').focus();true")
            normal_ready()
            current_text = cdp("document.querySelector('input[type=text]').value")
            assert current_text in ("你好", "你好shijie"), current_text
            checks.append("live composition cannot leak into password field")
            time.sleep(.3)
            def done_count():
                return len(re.findall(r"zwp_text_input_v3#\d+\.done\(", (OUT / "browser.err").read_text()))
            idle_before = done_count()
            time.sleep(.7)
            idle_after = done_count()
            assert idle_after - idle_before <= 2, "Wayland acknowledgements continued without input"
            result["idle_done_events"] = idle_after - idle_before
            result["boundary_checks"] = checks
            result["passed"] = True
        restore_original = any(w["is_focused"] and w["id"] == wid for w in windows())
        if name == "wine":
            subprocess.run(["wineserver", "-k"], env=wine_env, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        if copied:
            copied.unlink(missing_ok=True)
        current = None
    print("PASS password fields:", ", ".join(r["frontend"] for r in results),
          "no learning events or model token")
finally:
    (OUT / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
    restore = restore_original if current is None else any(
        w["is_focused"] and w["id"] == current.get("id") for w in windows())
    if current:
        if current["name"] == "wine" and wine_env:
            subprocess.run(["wineserver", "-k"], env=wine_env, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        p = current["process"]
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill(); p.wait()
        if copied := current.get("copied"):
            copied.unlink(missing_ok=True)
    for stream in streams:
        stream.close()
    shutil.rmtree(profile, ignore_errors=True)
    if restore and original is not None:
        subprocess.run(["niri", "msg", "action", "focus-window", "--id", str(original)], check=False)
