#!/usr/bin/env python3
"""Exercise startup ownership races without touching the desktop daemon."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def check(succeeds):
    with tempfile.TemporaryDirectory(prefix="tipe-activation-test-") as directory:
        base = Path(directory)
        scripts = base / "scripts"
        commands = base / "commands"
        scripts.mkdir()
        commands.mkdir()
        # Keep the real readiness/rollback branches but shorten the bounded
        # retry count: mocks need no 10-second desktop startup allowance.
        source = (ROOT / "scripts/activate-user.sh").read_text()
        (scripts / "activate-user.sh").write_text(
            source.replace("seq 1 100", "seq 1 3"))
        (scripts / "enable-user.sh").write_text("#!/bin/bash\nexit 0\n")
        (scripts / "enable-user.sh").chmod(0o755)
        # Verify only the replacement owner, never systemd's transient PID 0.
        (scripts / "verify-user-daemon.py").write_text(
            "import os,sys,pathlib\n"
            "p=pathlib.Path(os.environ['TEST_BASE'])/'checks'\n"
            "count=int(p.read_text())+1 if p.exists() else 1\n"
            "p.write_text(str(count))\n"
            "raise SystemExit(0 if os.environ['TEST_READY']=='1' "
            "and count>=2 and sys.argv[1]=='424242' else 1)\n")
        stub = commands / "stub"
        stub.write_text('''#!/usr/bin/python3
import os, pathlib, sys
base = pathlib.Path(os.environ['TEST_BASE'])
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if name == 'systemctl':
    if 'is-active' in args: raise SystemExit(1)
    if 'restart' in args or 'start' in args: (base/'running').touch()
    if 'show' in args: print('0')
elif name == 'pgrep':
    if not (base/'running').exists(): raise SystemExit(1)
    print('424242')
elif name == 'fcitx5-remote':
    if '-n' in args: print('tipe')
''')
        stub.chmod(0o755)
        for name in ("systemctl", "pgrep", "fcitx5-remote",
                     "dbus-update-activation-environment", "sleep"):
            (commands / name).symlink_to(stub)
        env = dict(os.environ, HOME=str(base), TEST_BASE=str(base),
                   TMPDIR=str(base),
                   FCITX_PROFILE=str(base / "config/profile"),
                   TEST_READY="1" if succeeds else "0",
                   PATH=str(commands) + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["bash", str(scripts / "activate-user.sh")],
                                env=env, capture_output=True, text=True,
                                timeout=15, start_new_session=True)
        assert result.returncode == (0 if succeeds else 1), result
        assert ("当前输入法：tipe" if succeeds else "Fcitx5 未能启动") in (
            result.stdout + result.stderr), result
        assert not list(base.glob("tipe-activate.*")), "Snapshot leaked"


check(True)
check(False)
print("PASS replacement bus owner accepted; transient PID 0 never signalled")
