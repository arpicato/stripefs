#!/usr/bin/env python3
"""Isolated integration test; creates temporary data on each SSD."""
import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk0-parent", type=Path, default=Path.home())
    parser.add_argument("--disk1-parent", type=Path, default=Path("/mnt/data"))
    args = parser.parse_args()
    if args.disk0_parent.stat().st_dev == args.disk1_parent.stat().st_dev:
        parser.error("disk parents must be on different filesystems")
    with tempfile.TemporaryDirectory(prefix="dualstripe-smoke-", dir=args.disk0_parent) as a, \
         tempfile.TemporaryDirectory(prefix="dualstripe-smoke-", dir=args.disk1_parent) as b:
        base = Path(a)
        source, disk0, mountpoint = (base / x for x in ("source", "disk0", "mount"))
        disk1 = Path(b) / "disk1"
        for p in (source, disk0, disk1, mountpoint):
            p.mkdir()
        (source / "nested").mkdir()
        expected = {}
        for i in range(12):
            rel = Path("nested" if i % 2 else ".") / f"sample-{i}.bin"
            data = os.urandom(1024 * 1024)
            (source / rel).write_bytes(data)
            expected[rel] = hashlib.sha256(data).hexdigest()
        (source / "shortcut").symlink_to("nested/sample-1.bin")
        subprocess.run([sys.executable, str(HERE / "import.py"), str(source), str(disk0), str(disk1)], check=True)
        subprocess.run([sys.executable, str(HERE / "import.py"), str(source), str(disk0), str(disk1)], check=True,
                       stdout=subprocess.DEVNULL)
        assert any(disk0.rglob("*.bin")) and any(disk1.rglob("*.bin"))
        process = subprocess.Popen([str(HERE / "dualstripe"), str(disk0), str(disk1), str(mountpoint), "-f"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            for _ in range(50):
                if os.path.ismount(mountpoint):
                    break
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read())
                time.sleep(0.1)
            assert os.path.ismount(mountpoint), "FUSE mount timed out"
            for rel, digest in expected.items():
                assert sha(mountpoint / rel) == digest, rel
            assert (mountpoint / "shortcut").read_bytes() == (mountpoint / "nested/sample-1.bin").read_bytes()
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results = list(pool.map(lambda p: sha(mountpoint / p), expected))
            assert results == list(expected.values())
            (mountpoint / "newdir").mkdir()
            fresh = mountpoint / "newdir" / "fresh"
            fresh.write_bytes(b"abcdef")
            with fresh.open("r+b") as f:
                f.seek(2)
                f.write(b"XY")
            assert fresh.read_bytes() == b"abXYef"
            fresh.rename(mountpoint / "newdir" / "renamed")
            assert (mountpoint / "newdir" / "renamed").read_bytes() == b"abXYef"
            (mountpoint / "newdir" / "renamed").unlink()
            (mountpoint / "newdir").rmdir()
            # A file can move between mirrored directories without changing disks.
            moved = mountpoint / "moved.bin"
            (mountpoint / "nested" / "sample-1.bin").rename(moved)
            assert sha(moved) == expected[Path("nested/sample-1.bin")]
            (mountpoint / "nested").rename(mountpoint / "renamed-nested")
            assert sha(mountpoint / "renamed-nested" / "sample-3.bin") == expected[Path("nested/sample-3.bin")]
            try:
                (mountpoint / "renamed-nested").rmdir()
            except OSError:
                pass
            else:
                raise AssertionError("rmdir accepted a populated directory")
            print("PASS: import, repeat import, both disks, mounted reads, parallel reads, symlink, create/write/rename/unlink")
        finally:
            subprocess.run(["fusermount3", "-u", str(mountpoint)], check=False)
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
