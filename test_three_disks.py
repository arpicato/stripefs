#!/usr/bin/env python3
"""Three-device weighted placement test; the third device is tmpfs."""
import os
from pathlib import Path
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="stripefs-three-", dir=Path.home()) as a, \
         tempfile.TemporaryDirectory(prefix="stripefs-three-", dir="/mnt/data") as b, \
         tempfile.TemporaryDirectory(prefix="stripefs-three-", dir="/dev/shm") as c:
        a, b, c = Path(a), Path(b), Path(c)
        roots = [a / "disk0", b / "disk1", c / "disk2"]
        mount, config = a / "mount", a / "config"
        for path in (*roots, mount):
            path.mkdir()
        config.write_text(f"mount\t{mount}\n" + "".join(
            f"root\t{weight}\t{root}\n" for weight, root in zip((1, 2, 4), roots)))
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
            data = os.urandom(63 * 1024 * 1024 + 17)
            file = mount / "three.bin"
            file.write_bytes(data)
            os.setxattr(file, "user.stripefs.optimize", b"1")
            assert file.read_bytes() == data
            sizes = [sum(p.stat().st_size for p in (root / ".stripefs" / "chunks").rglob("data")) for root in roots]
            assert sizes[2] > sizes[1] > sizes[0] > 0, sizes
            assert sum(sizes) == len(data), sizes
            print(f"PASS: three disks, weighted placement {sizes}, no duplicate chunk data")
        finally:
            subprocess.run(["fusermount3", "-u", str(mount)], check=False)
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
