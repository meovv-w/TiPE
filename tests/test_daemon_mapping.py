"""A mapped old inode must never count as the newly installed library."""
import importlib.util
import mmap
import os
from pathlib import Path
import tempfile

source = Path(__file__).resolve().parent.parent / "scripts/verify-user-daemon.py"
spec = importlib.util.spec_from_file_location("daemon_verifier", source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory(prefix="tipe-module-map-") as tmp:
    path = Path(tmp) / "module.so"
    path.write_bytes(bytes(4096))
    with path.open("r+b") as file, mmap.mmap(file.fileno(), 4096) as mapping:
        assert module.verify_mappings(os.getpid(), [path])
        replacement = path.with_suffix(".new")
        replacement.write_bytes(bytes(4096))
        os.replace(replacement, path)
        assert not module.verify_mappings(os.getpid(), [path]), "Old inode accepted"
        with path.open("r+b") as new_file, mmap.mmap(new_file.fileno(), 4096) as new_mapping:
            assert module.verify_mappings(os.getpid(), [path])
print("Live mapping, atomic replacement rejection and new mapping passed")
