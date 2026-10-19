#!/usr/bin/env python3
"""Temporary repeated two-file read benchmark on server-0."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import random
import statistics
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parent


def run_dd(path):
    started = time.perf_counter()
    subprocess.run(["dd", f"if={path}", "of=/dev/null", "bs=4M", "status=none"], check=True)
    return time.perf_counter() - started


def evict(paths):
    for path in paths:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk0-parent", type=Path, default=Path.home())
    parser.add_argument("--disk1-parent", type=Path, default=Path("/mnt/data"))
    parser.add_argument("--size-mib", type=int, default=3072)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    if args.size_mib <= 0 or args.repetitions <= 0 or args.size_mib % 4:
        parser.error("size must be a positive multiple of 4 MiB; repetitions must be positive")
    if args.disk0_parent.stat().st_dev == args.disk1_parent.stat().st_dev:
        parser.error("disk parents must be on different filesystems")
    with tempfile.TemporaryDirectory(prefix="dualstripe-bench-", dir=args.disk0_parent) as a, \
         tempfile.TemporaryDirectory(prefix="dualstripe-bench-", dir=args.disk1_parent) as b:
        a, b = Path(a), Path(b)
        d0, d1, mount = a / "disk0", b / "disk1", a / "mount"
        for path in (d0, d1, mount):
            path.mkdir()
        files = [d0 / "disk0.bin", d1 / "disk1.bin"]
        for path in files:
            print(f"Writing {args.size_mib} MiB to {path.parent}...", flush=True)
            subprocess.run(["dd", "if=/dev/zero", f"of={path}", "bs=4M", f"count={args.size_mib // 4}",
                            "oflag=direct", "conv=fdatasync", "status=none"], check=True)
        process = subprocess.Popen([str(HERE / "dualstripe"), str(d0), str(d1), str(mount), "-f"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            for _ in range(50):
                if os.path.ismount(mount):
                    break
                time.sleep(0.1)
            assert os.path.ismount(mount), "mount failed"
            mounted = [mount / path.name for path in files]
            results = {"fuse_sequential": [], "fuse_parallel": [], "backing_parallel": []}
            order = list(results)
            rng = random.Random(42)
            for trial in range(args.repetitions):
                rng.shuffle(order)
                for mode in order:
                    evict(files + mounted)
                    paths = files if mode == "backing_parallel" else mounted
                    start = time.perf_counter()
                    if mode == "fuse_sequential":
                        for path in paths:
                            run_dd(path)
                    else:
                        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                            list(pool.map(run_dd, paths))
                    elapsed = time.perf_counter() - start
                    rate = args.size_mib * 2 / elapsed
                    results[mode].append(rate)
                    print(f"trial {trial + 1}: {mode}: {elapsed:.2f}s, {rate:.0f} MiB/s", flush=True)
            for mode, rates in results.items():
                print(f"median {mode}: {statistics.median(rates):.0f} MiB/s "
                      f"(range {min(rates):.0f}-{max(rates):.0f})", flush=True)
        finally:
            subprocess.run(["fusermount3", "-u", str(mount)], check=False)
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
