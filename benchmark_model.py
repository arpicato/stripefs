#!/usr/bin/env python3
"""Materialize every tensor in a safetensors shard from direct ext4 and StripeFS."""
import argparse
import gc
import os
from pathlib import Path
import random
import time

import numpy as np
import ml_dtypes  # Registers bfloat16 with NumPy for this model shard.
from safetensors import safe_open


def evict(paths):
    for path in paths:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)


def load(path):
    started = time.perf_counter()
    total, checksum = 0, 0
    with safe_open(path, framework="np", device="cpu") as archive:
        for name in archive.keys():
            tensor = np.array(archive.get_tensor(name), copy=True)
            raw = tensor.view(np.uint8)
            total += raw.nbytes
            checksum += int(raw[::4096].sum(dtype=np.uint64))
            del tensor, raw
    gc.collect()
    return time.perf_counter() - started, total, checksum


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("direct", type=Path)
    parser.add_argument("striped", type=Path)
    parser.add_argument("--chunk", type=Path, action="append", default=[])
    parser.add_argument("--repetitions", type=int, default=2)
    args = parser.parse_args()
    modes = [("direct", args.direct), ("striped", args.striped)]
    rng = random.Random(7)
    for trial in range(args.repetitions):
        rng.shuffle(modes)
        for label, path in modes:
            evict([args.direct, args.striped, *args.chunk])
            elapsed, size, checksum = load(path)
            print(f"trial {trial + 1} {label}: {elapsed:.2f}s, "
                  f"{size / (1024 * 1024):.0f} MiB materialized, checksum={checksum}", flush=True)


if __name__ == "__main__":
    main()
