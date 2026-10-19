#!/usr/bin/env python3
"""Isolated CLI workflow test on two SSDs."""
import os
from pathlib import Path
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="stripefs-cli-", dir=Path.home()) as a, \
         tempfile.TemporaryDirectory(prefix="stripefs-cli-", dir="/mnt/data") as b:
        a, b = Path(a), Path(b)
        home, mount, root0, root1 = a / "home", a / "mount", a / "disk0", b / "disk1"
        for path in (home, mount, root0, root1):
            path.mkdir()
        config_dir = home / ".config" / "stripefs"
        config_dir.mkdir(parents=True)
        config = config_dir / "config"
        config.write_text(f"mount\t{mount}\nroot\t2\t{root0}\nroot\t1\t{root1}\n")
        process = subprocess.Popen([str(HERE / "stripefsd"), str(config), "-f"],
                                   stderr=subprocess.PIPE)
        env = dict(os.environ, HOME=str(home))
        source = a / "model.bin"
        data = os.urandom(17 * 1024 * 1024 + 11)
        source.write_bytes(data)
        def cli(*args):
            return subprocess.run([str(HERE / "stripefs"), *args], env=env,
                                  text=True, capture_output=True, check=True).stdout
        try:
            for _ in range(50):
                if os.path.ismount(mount):
                    break
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode())
                time.sleep(0.1)
            assert os.path.ismount(mount)
            cli("add", str(source))
            assert source.is_symlink() and source.read_bytes() == data
            cli("optimize", str(source), "--discard-original")
            assert source.read_bytes() == data
            assert os.getxattr(source, "user.stripefs.state") == b"striped"
            assert "striped" in cli("status", str(source))
            assert not list(a.glob(".model.bin.stripefs-original-*"))
            cli("remove", str(source))
            assert os.getxattr(source, "user.stripefs.state") == b"plain"
            assert source.read_bytes() == data
            source.write_bytes(b"modified")
            assert source.read_bytes() == b"modified"
            print("PASS: add, original-path symlink, optimize, verified discard, status, remove")
        finally:
            subprocess.run(["fusermount3", "-u", str(mount)], check=False)
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
