#!/usr/bin/env python3
"""Compare one striped file with direct ext4 reads on both SSDs."""
import argparse
import os
from pathlib import Path
import random
import statistics
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parent


def evict(paths):
    for path in paths:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)


def read_file(path):
    started = time.perf_counter()
    subprocess.run(["dd", f"if={path}", "of=/dev/null", "bs=4M", "status=none"], check=True)
    return time.perf_counter() - started


def read_sectors(devices):
    return {device: int((Path("/sys/block") / device / "stat").read_text().split()[2])
            for device in devices}


def random_reads(path, offsets):
    fd = os.open(path, os.O_RDONLY)
    timings = []
    try:
        started = time.perf_counter()
        for offset in offsets:
            before = time.perf_counter_ns()
            if len(os.pread(fd, 4096, offset)) != 4096:
                raise RuntimeError(f"short random read from {path}")
            timings.append((time.perf_counter_ns() - before) / 1_000_000)
        elapsed = time.perf_counter() - started
    finally:
        os.close(fd)
    sorted_timings = sorted(timings)
    return len(offsets) / elapsed, sorted_timings[len(offsets) // 2], sorted_timings[int(len(offsets) * .95)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size-mib", type=int, default=3072)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--weights", default="1,1",
                        help="positive disk0,disk1 placement weights (default: 1,1)")
    parser.add_argument("--devices", default="",
                        help="comma-separated block devices for physical read accounting")
    args = parser.parse_args()
    if args.size_mib % 4:
        parser.error("size must be a multiple of 4 MiB")
    try:
        weight0, weight1 = (int(value) for value in args.weights.split(","))
        if not 1 <= weight0 <= 1000000 or not 1 <= weight1 <= 1000000:
            raise ValueError
    except ValueError:
        parser.error("--weights must be two positive integers separated by a comma")
    devices = [device for device in args.devices.split(",") if device]
    if any(not (Path("/sys/block") / device / "stat").is_file() for device in devices):
        parser.error("--devices must name devices present under /sys/block")
    with tempfile.TemporaryDirectory(prefix="stripefs-bench-", dir=Path.home()) as a, \
         tempfile.TemporaryDirectory(prefix="stripefs-bench-", dir="/mnt/data") as b:
        a, b = Path(a), Path(b)
        roots = [a / "disk0", b / "disk1"]
        mount, config = a / "mount", a / "config"
        for path in (*roots, mount):
            path.mkdir()
        config.write_text(f"mount\t{mount}\nroot\t{weight0}\t{roots[0]}\n"
                          f"root\t{weight1}\t{roots[1]}\n")
        raw = [roots[0] / "file.bin", roots[1] / "reference.bin"]
        for path in raw:
            print(f"Writing {args.size_mib} MiB to {path.parent}...", flush=True)
            subprocess.run(["dd", "if=/dev/zero", f"of={path}", "bs=4M",
                            f"count={args.size_mib // 4}", "oflag=direct", "conv=fdatasync",
                            "status=none"], check=True)
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
            print("Striping and verifying the first file...", flush=True)
            os.setxattr(mount / "file.bin", "user.stripefs.optimize", b"1")
            identifier = os.getxattr(mount / "file.bin", "user.stripefs.id").decode()
            original = roots[0] / ".stripefs" / "backups" / identifier
            chunks = [p for root in roots for p in (root / ".stripefs" / "chunks" / identifier).iterdir()]
            print(f"Stripe bytes by disk: {[p.stat().st_size for p in chunks]}", flush=True)
            cases = {"striped": mount / "file.bin", "ext4_disk0": original, "ext4_disk1": raw[1]}
            results = {name: [] for name in cases}
            order = list(cases)
            rng = random.Random(13)
            for trial in range(args.repetitions):
                rng.shuffle(order)
                for name in order:
                    evict([*chunks, original, raw[1], mount / "file.bin"])
                    before = read_sectors(devices)
                    elapsed = read_file(cases[name])
                    after = read_sectors(devices)
                    rate = args.size_mib / elapsed
                    results[name].append(rate)
                    physical = {device: round((after[device] - before[device]) * 512 / (1024 * 1024), 1)
                                for device in devices}
                    print(f"trial {trial + 1} {name}: {elapsed:.2f}s, {rate:.0f} MiB/s"
                          + (f"; physical read MiB {physical}" if devices else ""), flush=True)
            for name, rates in results.items():
                print(f"median {name}: {statistics.median(rates):.0f} MiB/s "
                      f"(range {min(rates):.0f}-{max(rates):.0f})", flush=True)
            offsets = [rng.randrange(args.size_mib * 256) * 4096 for _ in range(4096)]
            for name, path in cases.items():
                evict([*chunks, original, raw[1], mount / "file.bin"])
                iops, p50, p95 = random_reads(path, offsets)
                print(f"random 4KiB {name}: {iops:.0f} IOPS, "
                      f"p50 {p50:.3f} ms, p95 {p95:.3f} ms", flush=True)
        finally:
            subprocess.run(["fusermount3", "-u", str(mount)], check=False)
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
