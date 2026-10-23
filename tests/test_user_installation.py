#!/usr/bin/env python3
"""Installation ownership, rollback, crash recovery and profile isolation."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import owned_files
import user_installation
from owned_files import OwnedFiles, digest


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


profile_editor = load("profile_editor", ROOT / "scripts/fcitx-profile.py")
live_editor = load("live_editor", ROOT / "scripts/disable-live.py")


class InstallationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tipe-install-tests-")
        self.base = Path(self.temporary.name)
        self.a, self.b = self.base / "a", self.base / "b"
        self.directory = self.base / "installation"

    def tearDown(self):
        self.temporary.cleanup()

    def manager(self):
        return OwnedFiles(self.directory, [self.a, self.b])

    def test_startup_dropin_quotes_paths_and_preserves_existing_file(self):
        config = self.base / 'config space%"quoted'
        with patch.object(user_installation, 'CONFIG', config):
            contents = user_installation.startup_dropin()
        self.assertIn(b'config space%%\\"quoted/fcitx5/addon', contents)
        with patch.object(user_installation, 'CONFIG', self.base / 'bad\npath'):
            with self.assertRaises(RuntimeError):
                user_installation.startup_dropin()
        # An existing service configuration must retain its backup even
        # when identical, so uninstall restores it instead of removing it.
        self.assertNotIn(str(user_installation.STARTUP_DROPIN), user_installation.dedicated_core())

    def test_update_then_restore_original_and_keep_modified_file(self):
        self.a.write_bytes(b"original user config")
        self.a.chmod(0o640)
        with self.manager() as files:
            files.install([(self.a, b"installed", 0o644), (self.b, b"module-v1", 0o755)])
            files.install([(self.a, b"updated", 0o644), (self.b, b"module-v2", 0o755)])
            self.b.write_bytes(b"user modified module")
            self.assertFalse(files.uninstall())
            self.assertEqual(self.a.read_bytes(), b"original user config")
            self.assertEqual(self.a.stat().st_mode & 0o777, 0o640)
            self.assertEqual(self.b.read_bytes(), b"user modified module")
            self.assertEqual(len(files.read()["files"]), 1)
            # A refused update cannot alter the user's file.
            with self.assertRaises(RuntimeError):
                files.install([(self.b, b"module-v3", 0o755)])
            self.b.write_bytes(b"module-v2")
            self.assertTrue(files.uninstall())
            self.assertFalse(self.b.exists())

    def test_atomic_replacement_keeps_open_reader_valid(self):
        self.a.write_bytes(b"old mapped contents")
        with self.a.open("rb") as reader, self.manager() as files:
            old_inode = self.a.stat().st_ino
            files.install([(self.a, b"new complete contents", 0o755)])
            self.assertNotEqual(old_inode, self.a.stat().st_ino)
            self.assertEqual(reader.read(), b"old mapped contents")

    def test_failed_second_write_rolls_back_first_and_manifest(self):
        self.a.write_bytes(b"original")
        real = owned_files.atomic_write
        def failing(path, data, mode=0o600):
            if Path(path) == self.b:
                raise OSError("injected write failure")
            return real(path, data, mode)
        with self.manager() as files:
            with patch.object(owned_files, "atomic_write", failing):
                with self.assertRaises(OSError):
                    files.install([(self.a, b"changed", 0o644), (self.b, b"new", 0o644)])
            self.assertEqual(self.a.read_bytes(), b"original")
            self.assertFalse(self.b.exists())
            self.assertEqual(files.read()["files"], [])
            self.assertFalse(files.pending.exists())

    def test_process_death_before_and_after_commit(self):
        for after_commit in (False, True):
            with self.subTest(after_commit=after_commit):
                self.a.write_bytes(b"original")
                code = '''
import os, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import owned_files
a=Path(sys.argv[2]); directory=Path(sys.argv[3]); committed=sys.argv[4]=='1'
real=owned_files.atomic_write
def crash(path, data, mode=0o600):
    real(path, data, mode)
    if Path(path)==(directory/'manifest.json' if committed else a): os._exit(77)
owned_files.atomic_write=crash
with owned_files.OwnedFiles(directory, [a]) as files:
    files.install([(a,b'new',0o644)])
'''
                result = subprocess.run([sys.executable, "-c", code, str(ROOT / "scripts"),
                    str(self.a), str(self.directory), str(int(after_commit))], timeout=10)
                self.assertEqual(result.returncode, 77)
                with self.manager() as files:
                    self.assertEqual(self.a.read_bytes(), b"new" if after_commit else b"original")
                    self.assertFalse(files.pending.exists())
                    if after_commit:
                        self.assertTrue(files.uninstall())

    def test_path_tampering_and_symlinks_refused(self):
        outsider = self.base / "unrelated"
        outsider.write_bytes(b"keep")
        with self.manager() as files:
            files.save(dict(version=1, files=[dict(path=str(outsider), sha256=digest(b"keep"))]))
            with self.assertRaises(RuntimeError):
                files.uninstall()
            files.save(dict(version=1, files=[]))
            self.a.symlink_to(outsider)
            with self.assertRaises(RuntimeError):
                files.install([(self.a, b"replace", 0o644)])
        self.assertEqual(outsider.read_bytes(), b"keep")

    def test_dry_run_does_not_change_files_or_manifest(self):
        with self.manager() as files:
            files.install([(self.a, b"installed", 0o644)])
            before = files.manifest.read_bytes()
            self.assertTrue(files.uninstall(dry_run=True))
            self.assertEqual(files.manifest.read_bytes(), before)
            self.assertEqual(self.a.read_bytes(), b"installed")

    def test_profile_all_groups_without_sidecar_and_later_user_changes(self):
        profile = self.base / "profile"
        original = (b"# keep comment\n[Groups/0]\nName=main\nDefaultIM=tipe\n\n"
            b"[Groups/0/Items/0]\nName=keyboard-us\n\n[Groups/0/Items/1]\nName=pinyin\n\n"
            b"[Groups/0/Items/2]\nName=tipe\n\n[Groups/0/Items/3]\nName=tipe\n\n"
            b"[Groups/1]\nName=second\nDefaultIM=tipe\n\n[Groups/1/Items/0]\nName=keyboard-de\n\n"
            b"[Groups/1/Items/1]\nName=tipe\n\n[Unrelated]\nSetting=keep exactly\n")
        profile.write_bytes(original)
        profile_editor.disable(profile)
        value = profile.read_text()
        self.assertNotIn("=tipe", value)
        self.assertIn("DefaultIM=pinyin", value)
        self.assertIn("DefaultIM=keyboard-de", value)
        self.assertIn("# keep comment", value)
        self.assertIn("[Unrelated]\nSetting=keep exactly", value)
        before_enable = profile.read_bytes()
        profile_editor.enable(profile)
        profile.write_bytes(profile.read_bytes() + b"# later user edit\n")
        profile_editor.disable(profile)
        self.assertEqual(profile.read_bytes(), before_enable + b"# later user edit\n")

    def test_real_client_installer_collision_update_and_legacy_tsv(self):
        env = dict(os.environ, HOME=str(self.base), XDG_CONFIG_HOME=str(self.base / "config"),
                   XDG_DATA_HOME=str(self.base / "data"), XDG_STATE_HOME=str(self.base / "state"))
        for key in ("TIPE_GTK_PREFIX", "TIPE_QT_PREFIX", "TIPE_GTK_BUILD_DIR", "TIPE_QT_MODULE", "TIPE_QT_PREFIX_SOURCE", "TIPE_QT_BUILD_PREFIX"):
            env.pop(key, None)
        module = self.base / "input-module.so"
        module.write_bytes(b"test module")
        wrapper = self.base / ".local/bin/tipe-run"
        wrapper.parent.mkdir(parents=True)
        wrapper.write_bytes(b"unrelated wrapper")
        command = ["bash", str(ROOT / "scripts/install-client-ui.sh")]
        result = subprocess.run(command + ["--qt-module", str(module)], env=env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        target = self.base / "data/tipe/qt/lib/qt6/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so"
        self.assertFalse(target.exists(), "collision must be detected before installing modules")
        wrapper.unlink()
        subprocess.run(command + ["--qt-module", str(module)], env=env, check=True)
        module.write_bytes(b"updated module")
        subprocess.run(command + ["--force", "--qt-module", str(module)], env=env, check=True)
        self.assertEqual(target.read_bytes(), b"updated module")
        subprocess.run(command + ["--uninstall"], env=env, check=True)
        self.assertFalse(target.exists())
        self.assertFalse(wrapper.exists())
        # Old TSV manifests have two consecutive tabs for an absent backup.
        directory = self.base / "state/tipe/client-ui"
        (directory / "manifest.json").unlink()
        wrapper.write_bytes(b"legacy wrapper")
        (directory / "manifest.tsv").write_text(f"{wrapper}\t\t{digest(wrapper.read_bytes())}\n")
        subprocess.run(command + ["--uninstall"], env=env, check=True)
        self.assertFalse(wrapper.exists())

    def test_core_scripts_install_dry_run_uninstall_in_isolated_home(self):
        commands = self.base / "commands"
        commands.mkdir()
        cmake = commands / "cmake"
        cmake.write_text('''#!/usr/bin/python3
import os, pathlib, sys
if '--install' in sys.argv:
    stage=pathlib.Path(os.environ['DESTDIR'])/'usr'
    files={'bin/tipe-trainer':b'trainer', 'lib/fcitx5/libtipe.so':b'core',
           'share/fcitx5/inputmethod/tipe.conf':b'TiPE metadata',
           'share/fcitx5/themes/MacOS-Dark/theme.conf':b'theme'}
    for relative, contents in files.items():
        p=stage/relative; p.parent.mkdir(parents=True,exist_ok=True); p.write_bytes(contents)
''')
        cmake.chmod(0o755)
        env = dict(os.environ, HOME=str(self.base), XDG_CONFIG_HOME=str(self.base / "config"),
                   XDG_DATA_HOME=str(self.base / "data"), XDG_STATE_HOME=str(self.base / "state"),
                   XDG_RUNTIME_DIR=str(self.base / "runtime"),
                   PATH=str(commands) + os.pathsep + os.environ["PATH"])
        for key in ("DBUS_SESSION_BUS_ADDRESS", "FCITX_PROFILE", "FCITX_DATA_HOME", "TIPE_CLASSICUI_SO", "TIPE_GTK_PREFIX", "TIPE_QT_PREFIX"):
            env.pop(key, None)
        build = self.base / "build"
        build.mkdir()
        (build / "libtipe.so").write_bytes(b"core")
        profile = self.base / "config/fcitx5/profile"
        profile.parent.mkdir(parents=True)
        profile.write_text("[Groups/0]\nName=main\nDefaultIM=tipe\n[Groups/0/Items/0]\nName=keyboard-us\n[Groups/0/Items/1]\nName=tipe\n")
        classic_conf = self.base / "config/fcitx5/conf/classicui.conf"
        classic_conf.parent.mkdir()
        classic_conf.write_bytes(b"# user config\nFont=Sans 12\n")
        learning = self.base / "state/tipe/model.current"
        learning.parent.mkdir(parents=True)
        learning.write_bytes(b"personal learning must survive")
        install = ["bash", str(ROOT / "scripts/install-user.sh"), str(build)]
        uninstall = ["bash", str(ROOT / "scripts/uninstall-user.sh")]
        subprocess.run(install, env=env, check=True, stdout=subprocess.DEVNULL)
        core = self.base / "config/fcitx5/addon/libtipe.so"
        trainer = self.base / ".local/bin/tipe-trainer"
        self.assertEqual(core.read_bytes(), b"core")
        self.assertTrue(trainer.exists())
        startup = self.base / "config/systemd/user/fcitx5.service.d/60-tipe.conf"
        self.assertIn(f'Environment="FCITX_ADDON_DIRS={core.parent}:'.encode(), startup.read_bytes())
        self.assertIn(b'Environment="TIPE_CLASSIC_GRID=1"', startup.read_bytes())
        self.assertNotIn(b"ExecStart", startup.read_bytes())
        before_profile = profile.read_bytes()
        subprocess.run(uninstall + ["--dry-run"], env=env, check=True, stdout=subprocess.DEVNULL)
        self.assertTrue(core.exists())
        self.assertEqual(profile.read_bytes(), before_profile)
        # User alterations survive a real uninstall, returning a conflict
        # status instead of reporting complete cleanup.
        core.write_bytes(b"user alteration")
        result = subprocess.run(uninstall, env=env, capture_output=True)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertEqual(core.read_bytes(), b"user alteration")
        self.assertFalse(trainer.exists())
        self.assertFalse(startup.exists())
        self.assertEqual(classic_conf.read_bytes(), b"# user config\nFont=Sans 12\n")
        self.assertNotIn("=tipe", profile.read_text())
        backups = list(profile.parent.glob("profile.tipe-uninstall-backup.*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), before_profile)
        self.assertEqual(backups[0].stat().st_mode & 0o777, 0o600)
        self.assertEqual(learning.read_bytes(), b"personal learning must survive")
        core.write_bytes(b"core")
        subprocess.run(uninstall, env=env, check=True, stdout=subprocess.DEVNULL)
        self.assertFalse(core.exists())
        self.assertFalse((self.base / "data/fcitx5/themes/MacOS-Dark/theme.conf").exists())


class LiveProfileTests(unittest.TestCase):
    def test_remove_and_failure_rollback_preserve_groups_and_defaults(self):
        for fail in (False, True):
            with self.subTest(fail=fail):
                groups = {"main": ["us", [("keyboard-us", ""), ("pinyin", ""), ("tipe", "")], "pinyin"],
                          "second": ["de", [("keyboard-de", ""), ("tipe", "")], "tipe"],
                          "untouched": ["us", [("keyboard-us", ""), ("other", "")], "other"]}
                original = json.loads(json.dumps(groups))
                selected = ["main", "pinyin"]
                failure = [fail]
                def call(method, args=None):
                    if method == "CurrentInputMethodGroup": return (selected[0],)
                    if method == "CurrentInputMethod": return (selected[1],)
                    if method == "InputMethodGroups": return (list(groups),)
                    if method == "InputMethodGroupInfo": return groups[args[0]][:2]
                    if method == "FullInputMethodGroupInfo": return (args[0], groups[args[0]][2])
                    if method == "SwitchInputMethodGroup": selected[0] = args[0]
                    if method == "SetCurrentIM":
                        selected[1] = args[0]
                        groups[selected[0]][2] = args[0]
                    if method == "SetInputMethodGroupInfo":
                        if args[0] == "second" and failure[0]:
                            failure[0] = False
                            raise RuntimeError("injected bus failure")
                        groups[args[0]][:2] = [args[1], args[2]]
                        groups[args[0]][2] = ""
                    return ()
                if fail:
                    with self.assertRaises(RuntimeError):
                        live_editor.remove_live(call, lambda _, values: values)
                    self.assertEqual(json.loads(json.dumps(groups)), original)
                else:
                    live_editor.remove_live(call, lambda _, values: values)
                    self.assertEqual(groups["main"][2], "pinyin")
                    self.assertEqual(json.loads(json.dumps(groups["untouched"])), original["untouched"])
                    self.assertFalse(any(name == "tipe" for _, items, _ in groups.values() for name, _ in items))
                self.assertEqual(selected, ["main", "pinyin"])


if __name__ == "__main__":
    unittest.main()
