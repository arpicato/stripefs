#!/usr/bin/env python3
"""Directory watch policy tests without mounting FUSE or touching configured disks."""
import argparse
import errno
import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


CLI = Path(__file__).with_name("stripefs")
loader = importlib.machinery.SourceFileLoader("stripefs_cli", str(CLI))
spec = importlib.util.spec_from_loader(loader.name, loader)
stripefs = importlib.util.module_from_spec(spec)
loader.exec_module(stripefs)


class WatchTest(unittest.TestCase):
    def test_new_nested_files_are_striped_only_after_stability_and_unwatch_stops_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mount = root / "mount"
            directory = mount / "models"
            directory.mkdir(parents=True)
            state = {}

            def getxattr(path, name):
                self.assertEqual(name, "user.stripefs.state")
                return state.get(str(path), b"plain")

            def setxattr(path, name, value):
                self.assertEqual(value, b"1")
                if name == "user.stripefs.optimize":
                    state[str(path)] = b"striped"
                elif name == "user.stripefs.unstripe":
                    state[str(path)] = b"plain"
                else:
                    self.fail(name)

            with mock.patch.object(stripefs, "STATE", root / "state"), \
                 mock.patch.object(stripefs, "WATCHES", root / "state/watches.json"), \
                 mock.patch.object(stripefs, "WATCH_LOCK", root / "state/watches.lock"), \
                 mock.patch.object(stripefs, "load_config", return_value=(mount, [])), \
                 mock.patch.object(stripefs, "require_mount"), \
                 mock.patch.object(stripefs, "install_watch_service"), \
                 mock.patch.object(stripefs.os, "getxattr", side_effect=getxattr), \
                 mock.patch.object(stripefs.os, "setxattr", side_effect=setxattr), \
                 mock.patch.object(stripefs.time, "monotonic", return_value=100):
                stripefs.cmd_optimize(argparse.Namespace(path=[directory], watch=True,
                                                         discard_original=False))
                nested = directory / "nested"
                nested.mkdir()
                file = nested / "model.bin"
                file.write_bytes(b"model")
                seen = stripefs.watch_scan(mount, {}, 60)
                self.assertEqual(state, {})
                file.write_bytes(b"model still downloading")
                with mock.patch.object(stripefs.time, "monotonic", return_value=161):
                    seen = stripefs.watch_scan(mount, seen, 60)
                self.assertEqual(state, {})
                with mock.patch.object(stripefs.time, "monotonic", return_value=222):
                    seen = stripefs.watch_scan(mount, seen, 60)
                self.assertEqual(state[str(file)], b"striped")

                stripefs.cmd_unwatch(argparse.Namespace(path=[directory]))
                another = nested / "another.bin"
                another.write_bytes(b"another")
                with mock.patch.object(stripefs.time, "monotonic", return_value=250):
                    stripefs.watch_scan(mount, seen, 0)
                self.assertNotIn(str(another), state)

    def test_busy_file_is_retried_without_losing_policy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mount = root / "mount"
            directory = mount / "models"
            directory.mkdir(parents=True)
            file = directory / "busy.bin"
            file.write_bytes(b"data")
            calls = []

            def optimize(path, name, value):
                calls.append((path, name, value))
                if len(calls) == 1:
                    raise OSError(errno.EBUSY, "file is open")

            with mock.patch.object(stripefs, "STATE", root / "state"), \
                 mock.patch.object(stripefs, "WATCHES", root / "state/watches.json"), \
                 mock.patch.object(stripefs, "WATCH_LOCK", root / "state/watches.lock"), \
                 mock.patch.object(stripefs.os, "getxattr", return_value=b"plain"), \
                 mock.patch.object(stripefs.os, "setxattr", side_effect=optimize), \
                 mock.patch.object(stripefs.time, "monotonic", return_value=100):
                stripefs.save_watches({"models": {"source": str(directory)}})
                seen = stripefs.watch_scan(mount, {}, 60)
                with mock.patch.object(stripefs.time, "monotonic", return_value=161):
                    seen = stripefs.watch_scan(mount, seen, 60)
                with mock.patch.object(stripefs.time, "monotonic", return_value=176):
                    stripefs.watch_scan(mount, seen, 60)
                self.assertEqual(len(calls), 2)

    def test_remove_stops_watch_before_restoring_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mount = root / "mount"
            directory = mount / "models"
            directory.mkdir(parents=True)
            file = directory / "model.bin"
            file.write_bytes(b"model")
            watch = {"models": {"source": str(directory)}}
            with mock.patch.object(stripefs, "STATE", root / "state"), \
                 mock.patch.object(stripefs, "WATCHES", root / "state/watches.json"), \
                 mock.patch.object(stripefs, "WATCH_LOCK", root / "state/watches.lock"), \
                 mock.patch.object(stripefs, "load_config", return_value=(mount, [])), \
                 mock.patch.object(stripefs, "require_mount"), \
                 mock.patch.object(stripefs.os, "getxattr", return_value=b"striped"), \
                 mock.patch.object(stripefs.os, "setxattr") as setxattr:
                stripefs.save_watches(watch)
                stripefs.cmd_remove(argparse.Namespace(path=[directory]))
                self.assertEqual(stripefs.load_watches(), {})
                setxattr.assert_called_once_with(file, "user.stripefs.unstripe", b"1")


if __name__ == "__main__":
    unittest.main()
