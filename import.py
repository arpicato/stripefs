#!/usr/bin/env python3
"""Copy an existing tree into both dualstripe backing directories.

Regular files are placed whole on one disk. Directories are mirrored.
The source is never deleted. Every copied file is hashed after writing.
"""
import argparse
import hashlib
import os
import shutil
import stat
import sys
import tempfile
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb", buffering=1024 * 1024) as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.digest()


def copy_verified(source, target):
    before = source.stat()
    fd, temporary = tempfile.mkstemp(prefix=".dualstripe-import-", dir=target.parent)
    try:
        h = hashlib.sha256()
        with os.fdopen(fd, "wb", buffering=1024 * 1024) as out, open(source, "rb", buffering=1024 * 1024) as inp:
            for block in iter(lambda: inp.read(1024 * 1024), b""):
                h.update(block)
                out.write(block)
            out.flush()
            os.fsync(out.fileno())
        after = source.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise RuntimeError(f"source changed during copy: {source}")
        if digest(temporary) != h.digest():
            raise RuntimeError(f"hash mismatch: {source}")
        shutil.copystat(source, temporary, follow_symlinks=False)
        os.replace(temporary, target)
        parent_fd = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="existing directory; its contents become the mount root")
    parser.add_argument("backing0", type=Path)
    parser.add_argument("backing1", type=Path)
    args = parser.parse_args()
    source, *roots = (p.resolve(strict=True) for p in (args.source, args.backing0, args.backing1))
    if not source.is_dir() or any(not root.is_dir() for root in roots):
        parser.error("source and backing paths must be directories")
    if os.stat(roots[0]).st_dev == os.stat(roots[1]).st_dev:
        parser.error("backing paths must be on different filesystems")
    for root in roots:
        if root == source or root.is_relative_to(source) or source.is_relative_to(root):
            parser.error("source and backing directories must not overlap")
    placed = [0, 0]
    counts = [0, 0]
    folders = []
    for current, dirnames, filenames in os.walk(source, followlinks=False):
        current = Path(current)
        relative = current.relative_to(source)
        folders.append(relative)
        for root in roots:
            (root / relative).mkdir(exist_ok=True)
        # os.walk does not descend through directory symlinks; import them as links.
        for name in list(dirnames):
            if (current / name).is_symlink():
                dirnames.remove(name)
                filenames.append(name)
        for name in filenames:
            src = current / name
            rel = relative / name
            existing = [root / rel for root in roots if (root / rel).exists() or (root / rel).is_symlink()]
            if len(existing) > 1:
                raise RuntimeError(f"duplicate path in backing stores: {rel}")
            mode = src.lstat().st_mode
            if stat.S_ISLNK(mode):
                if existing:
                    if not existing[0].is_symlink() or os.readlink(existing[0]) != os.readlink(src):
                        raise RuntimeError(f"conflicting symlink: {rel}")
                    continue
                disk = 0 if counts[0] <= counts[1] else 1
                os.symlink(os.readlink(src), roots[disk] / rel)
                counts[disk] += 1
                continue
            if not stat.S_ISREG(mode):
                raise RuntimeError(f"unsupported file type: {src}")
            if existing:
                if not existing[0].is_file() or digest(src) != digest(existing[0]):
                    raise RuntimeError(f"conflicting file: {rel}")
                continue
            size = src.stat().st_size
            eligible = [i for i, root in enumerate(roots) if shutil.disk_usage(root).free > size + 64 * 1024 * 1024]
            if not eligible:
                raise RuntimeError(f"insufficient free space for {src} ({size} bytes)")
            disk = min(eligible, key=lambda i: (placed[i], counts[i]))
            copy_verified(src, roots[disk] / rel)
            placed[disk] += size
            counts[disk] += 1
            print(f"{rel} -> disk {disk} ({size} bytes)", flush=True)
    for relative in reversed(folders):
        for root in roots:
            shutil.copystat(source / relative, root / relative, follow_symlinks=False)
    print(f"verified files by disk: {counts}; copied bytes: {placed}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(f"import failed: {error}", file=sys.stderr)
        sys.exit(1)
