"""A duplicate launch raises the existing process; a killed owner can restart."""
import os, pathlib, socket, subprocess, sys, tempfile, time

with tempfile.TemporaryDirectory(prefix="tipe-control-instance-") as tmp:
    root = pathlib.Path(tmp)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
               XDG_RUNTIME_DIR=str(runtime), XDG_STATE_HOME=str(root / "state"))
    address = str(runtime / "tipe-control.sock")

    def start():
        process = subprocess.Popen([sys.argv[1]], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                assert process.poll() is None, "Owner exited before its window was ready"
                try:
                    with socket.socket(socket.AF_UNIX) as client:
                        client.settimeout(.2)
                        client.connect(address)
                        client.sendall(b"show\n")
                        if client.recv(64) == b"shown\n":
                            return process
                except OSError:
                    pass
                time.sleep(.02)
            raise AssertionError("Owner did not acknowledge its window")
        except BaseException:
            process.kill()
            process.wait()
            raise

    first = start()
    try:
        second = subprocess.run([sys.argv[1]], env=env, capture_output=True, timeout=5)
        assert second.returncode == 0, second.stderr.decode()
        assert first.poll() is None, "Duplicate launch replaced the original window"
    finally:
        first.kill()
        first.wait()
    recovered = start()
    recovered.terminate()
    recovered.wait(timeout=5)
    print("Single instance, activation acknowledgement and stale-owner recovery passed")
