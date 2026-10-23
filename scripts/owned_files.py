"""Transactional, hash-checked installation of an explicit set of user files.

The manifest is an ownership record, never an authority to write arbitrary
paths. Callers supply the complete allowed target set independently of it.
"""
from __future__ import annotations

import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import uuid


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic_write(path, data, mode=0o600):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        Path(temporary).unlink(missing_ok=True)


def atomic_remove(path):
    path = Path(path)
    if not path.exists():
        return
    path.unlink()
    directory = os.open(path.parent, os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


class OwnedFiles:
    def __init__(self, directory, allowed):
        self.directory = Path(directory).absolute()
        self.allowed = {str(Path(p).absolute()) for p in allowed}
        self.manifest = self.directory / "manifest.json"
        self.pending = self.directory / "pending.json"

    def target(self, value):
        path = Path(value)
        if str(path) not in self.allowed:
            raise RuntimeError(f"清单包含非 TiPE 安装路径：{path}")
        # Do not follow a replaced file or an ancestor symlink on cleanup.
        for parent in (path, *path.parents):
            if parent.is_symlink():
                raise RuntimeError(f"拒绝通过符号链接修改文件：{parent}")
        if path.exists() and not path.is_file():
            raise RuntimeError(f"安装目标不是普通文件：{path}")
        return path

    def blob(self, value):
        path = Path(value)
        if path.parent != self.directory / "backups" or path.is_symlink():
            raise RuntimeError("无效的安装备份路径")
        return path

    def read(self):
        if not self.manifest.exists():
            return {"version": 1, "files": []}
        value = json.loads(self.manifest.read_text())
        if value.get("version") != 1 or not isinstance(value.get("files"), list):
            raise RuntimeError("不支持的安装清单")
        seen = set()
        for row in value["files"]:
            self.target(row["path"])
            if row["path"] in seen:
                raise RuntimeError("重复的安装清单条目")
            seen.add(row["path"])
            if row.get("backup"):
                backup = self.blob(row["backup"])
                if digest(backup.read_bytes()) != row["backup_sha256"]:
                    raise RuntimeError(f"安装备份校验失败：{backup}")
        return value

    def __enter__(self):
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.chmod(self.directory, 0o700)
        self.lock = (self.directory / "lock").open("a")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        self.recover()
        self.collect_backups()
        return self

    def __exit__(self, *_):
        self.lock.close()

    def save(self, value):
        atomic_write(self.manifest, (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode())

    def backup(self, data):
        path = self.directory / "backups" / uuid.uuid4().hex
        path.parent.mkdir(mode=0o700, exist_ok=True)
        atomic_write(path, data)
        return str(path)

    def recover(self):
        if not self.pending.exists():
            return
        journal = json.loads(self.pending.read_text())
        # Publishing the manifest is the commit point. A crash after it must
        # not undo a successfully completed installation.
        current = self.read()
        if current != journal["after"]:
            if current != journal["before"]:
                raise RuntimeError("安装恢复遇到清单冲突；保留文件和备份")
            for row in reversed(journal["operations"]):
                path = self.target(row["path"])
                actual = digest(path.read_bytes()) if path.exists() else None
                if actual == row["before_sha256"]:
                    continue
                if actual != row["after_sha256"]:
                    raise RuntimeError(f"安装中断后文件已修改，保留备份：{path}")
                if row["before_backup"]:
                    data = self.blob(row["before_backup"]).read_bytes()
                    if digest(data) != row["before_sha256"]:
                        raise RuntimeError("事务备份校验失败")
                    atomic_write(path, data, row["before_mode"])
                else:
                    atomic_remove(path)
        self.pending.unlink()
        self.collect_backups()

    def collect_backups(self):
        referenced = {r.get("backup") for r in self.read()["files"]}
        directory = self.directory / "backups"
        if directory.exists():
            for path in directory.iterdir():
                if path.is_file() and not path.is_symlink() and str(path) not in referenced:
                    path.unlink()

    def transaction(self, before, after, operations):
        # Validate and snapshot every target before changing the first file.
        journal_rows = []
        for path, data, mode in operations:
            path = self.target(str(path))
            old = path.read_bytes() if path.exists() else None
            journal_rows.append(dict(path=str(path), before_sha256=digest(old) if old is not None else None,
                before_backup=self.backup(old) if old is not None else None,
                before_mode=stat.S_IMODE(path.stat().st_mode) if old is not None else None,
                after_sha256=digest(data) if data is not None else None))
        journal = dict(before=before, after=after, operations=journal_rows)
        atomic_write(self.pending, (json.dumps(journal, ensure_ascii=False) + "\n").encode())
        try:
            for (path, data, mode), row in zip(operations, journal_rows):
                path = self.target(str(path))
                actual = digest(path.read_bytes()) if path.exists() else None
                if actual != row["before_sha256"]:
                    raise RuntimeError(f"安装期间文件被修改：{path}")
                if data is None:
                    atomic_remove(path)
                else:
                    atomic_write(path, data, mode)
            self.save(after)
        except BaseException:
            self.recover()
            raise
        self.pending.unlink()
        self.collect_backups()

    def install(self, entries, adopt_equal=()):
        """entries: (absolute target, bytes, mode); preserve preexisting files."""
        before = self.read()
        rows = {row["path"]: dict(row) for row in before["files"]}
        operations = []
        for path, data, mode in entries:
            path = self.target(str(path))
            key = str(path)
            actual = digest(path.read_bytes()) if path.exists() else None
            row = rows.get(key)
            if row and actual not in (None, row["sha256"]):
                raise RuntimeError(f"保留用户已修改的文件，安装取消：{path}")
            if row is None:
                old = path.read_bytes() if path.exists() else None
                # Migration only for dedicated TiPE paths with identical
                # reference content, never a shared theme or classicui file.
                adopted = key in adopt_equal and actual == digest(data)
                row = dict(path=key, backup=self.backup(old) if old is not None and not adopted else None,
                           backup_sha256=digest(old) if old is not None and not adopted else None,
                           backup_mode=stat.S_IMODE(path.stat().st_mode) if old is not None and not adopted else None)
            row["sha256"] = digest(data)
            rows[key] = row
            operations.append((path, data, mode))
        after = dict(version=1, files=list(rows.values()))
        self.transaction(before, after, operations)

    def uninstall(self, dry_run=False, records=None):
        if records is not None and not dry_run:
            raise ValueError("临时清单只能用于只读预览")
        before = self.read() if records is None else records
        keep, operations = [], []
        for row in reversed(before["files"]):
            path = self.target(row["path"])
            actual = digest(path.read_bytes()) if path.exists() else None
            if actual not in (None, row["sha256"]):
                print(f"保留已被修改的文件及原备份：{path}")
                keep.append(row)
                continue
            data = self.blob(row["backup"]).read_bytes() if row.get("backup") else None
            if data is not None and digest(data) != row["backup_sha256"]:
                raise RuntimeError(f"安装备份校验失败：{path}")
            print(f"{'恢复' if data is not None else '移除'}：{path}")
            operations.append((path, data, row.get("backup_mode") or 0o644))
        if not dry_run:
            self.transaction(before, dict(version=1, files=list(reversed(keep))), operations)
        return not keep
