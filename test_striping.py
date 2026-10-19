#!/usr/bin/env python3
"""Isolated two-disk StripeFS integration test."""
import hashlib
import mmap
import os
from pathlib import Path
import random
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="stripefs-test-", dir=Path.home()) as a, \
         tempfile.TemporaryDirectory(prefix="stripefs-test-", dir="/mnt/data") as b:
        a, b = Path(a), Path(b)
        roots = [a / "disk0", b / "disk1"]
        mount = a / "mount"
        config = a / "config"
        for path in (*roots, mount):
            path.mkdir()
        config.write_text(f"mount\t{mount}\nroot\t2\t{roots[0]}\nroot\t1\t{roots[1]}\n")
        process = subprocess.Popen([str(HERE / "stripefsd"), str(config), "-f"],
                                   stderr=subprocess.PIPE)
        try:
            for _ in range(50):
                if os.path.ismount(mount):
                    break
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode())
                time.sleep(0.1)
            assert os.path.ismount(mount)
            data = random.Random(17).randbytes(29 * 1024 * 1024 + 137)
            target = mount / "model.bin"
            target.write_bytes(data)
            assert os.getxattr(target, "user.stripefs.state") == b"plain"
            os.setxattr(target, "user.stripefs.optimize", b"1")
            assert os.getxattr(target, "user.stripefs.state") == b"striped"
            assert target.stat().st_size == len(data)
            assert hashlib.sha256(target.read_bytes()).digest() == hashlib.sha256(data).digest()
            with target.open("rb") as file:
                for _ in range(50):
                    offset = random.randrange(0, len(data) - 65536)
                    file.seek(offset)
                    assert file.read(65536) == data[offset:offset + 65536]
                with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
                    assert mapped[4000000:5000000] == data[4000000:5000000]
            sizes = [sum(p.stat().st_size for p in (root / ".stripefs" / "chunks").rglob("data")) for root in roots]
            assert sizes[0] > sizes[1] > 0, sizes
            try:
                target.write_bytes(b"unsafe")
            except OSError:
                pass
            else:
                raise AssertionError("striped write should be refused")
            assert target.read_bytes() == data
            os.setxattr(target, "user.stripefs.unstripe", b"1")
            assert os.getxattr(target, "user.stripefs.state") == b"plain"
            assert target.read_bytes() == data
            target.write_bytes(b"writable again")
            assert target.read_bytes() == b"writable again"
            print(f"PASS: striped read, random read, mmap, weighted placement {sizes}, safe unstripe")
        finally:
            subprocess.run(["fusermount3", "-u", str(mount)], check=False)
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
