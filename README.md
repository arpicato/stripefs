# StripeFS

StripeFS is an experimental Linux FUSE filesystem for selectively striping individual files across 2–16 existing filesystems. It leaves ext4 partitions intact. The data path is C/libfuse3; the `stripefs` command is Python 3. Ordinary files stay whole on one disk. `stripefs optimize` divides selected files into 4 MiB chunks and places more chunks on faster disks according to measured or supplied weights. A striped file occupies approximately its original size across all disks combined, plus a small map and optional retained backups.

**Current scope:** striped files support concurrent and memory-mapped reads, including random reads. They are read-only until `stripefs remove` reconstructs an ordinary file. StripeFS is not RAID redundancy: losing one disk makes any striped file that uses it unreadable. Keep an independent backup of important data.

Striped explicit reads bypass the FUSE page cache when direct-I/O mmap is
supported; mmap itself still caches pages. Small reads (up to128KiB) use Linux
`RWF_DONTCACHE` when supported to avoid retaining a second backing-cache copy.
Larger reads retain backing caching and explicit stripe read-ahead. Unsupported
systems fall back to buffered reads. See `RESULTS.md` for measured limits.

**Benchmark your actual application.** Sequential throughput gains do not
guarantee faster model loading or inference. Request size/order, mmap,
read-ahead, RAM and swapping can change the result—even cause slowdowns.
Compare cold and warm runs against unstriped files, and retest after RAM or
loader changes. See [measured results and limits](RESULTS.md).

## Install and initialize

Requires Linux, FUSE 3, `libfuse3-dev`, a C compiler, `pkg-config`, Python 3.9+, and a running systemd user manager. The backing paths must be on distinct filesystems. They can have different capacities. Do not place the mount inside a backing path, or edit backing stores directly while StripeFS is running.

```sh
make
./stripefs init --disk /path/to/ssd1/stripefs-store --disk /path/to/ssd2/stripefs-store
./stripefs status
```

`init` measures direct read speed on each disk using a temporary 256 MiB file and uses the resulting MiB/s as stripe weights. More chunks go to the faster disk, subject to free space. Supply one `--weight N` per `--disk` to override this, or `--keep-weights` when appending disks. The default mount is `~/stripefs-mount`; use `--mount PATH` to change it. It writes `~/.config/stripefs/config`, installs and enables `~/.config/systemd/user/stripefs.service`, and starts the mount. The user manager must run at boot for automatic mounting; an administrator can enable systemd lingering for this account.

To make the command available from any directory, link the checked-out executable into a directory on your `PATH` (for example `~/.local/bin/stripefs`). Keep the checkout in place because the service runs its daemon there.

## Select files and directories

```sh
stripefs add /path/to/game
stripefs optimize /path/to/game
stripefs optimize --watch /path/to/game
stripefs status /path/to/game
stripefs unwatch /path/to/game
stripefs remove /path/to/game
```

`add` imports recursively as ordinary files, verifies every copy with SHA-256, moves the original to an adjacent hidden backup, then links the original path to the managed copy. Applications can keep using that path. `--no-link` imports without changing the original path. Keep the source quiet during import. Unsupported special files and conflicting destination names cause an error. `optimize` imports automatically when needed, then stripes regular files recursively. It verifies each stripe against the source before switching the visible file. By default it retains both the pre-stripe managed copy and the original path backup. `--discard-original` deletes those copies only after verification; this is the explicit space-reclaiming operation. A 100 GB striped file then uses roughly 100 GB across the backing disks, not 200 GB.

`remove` reverses striping by reconstructing and verifying ordinary managed files. It does not remove them from management or delete either original path backup. Existing original-path symlinks continue to work and the files become writable. Run `stripefs recover` after an interrupted import or original-path switch; the service also runs it before mounting. `status PATH` shows per-disk chunk placement and usage. For service details, use `systemctl --user status stripefs.service`.

`optimize --watch DIR` also saves a persistent, recursive directory policy. StripeFS immediately processes existing files and its `stripefs-watch.service` checks for new files every 15 seconds. A new file must remain unchanged for 60 seconds before conversion; open files are retried later. The policy survives service restarts and applies to files created or renamed into nested directories through the managed path. It does not follow symlinked subdirectories. Use it for files that become immutable after being written, such as installed game assets or finished model downloads. Once a file is striped, writes return `EROFS`; run `stripefs remove FILE` after `stripefs unwatch DIR` to make it writable. `unwatch` stops future conversions without changing files already striped. `remove DIR` also stops watches inside that directory before reconstructing its files. The watcher retains verified pre-stripe copies, so check free space and use a separate `optimize --discard-original` operation when you explicitly want to reclaim it. The watcher service is installed by `init` and by `optimize --watch` on existing installations.

## Safety and limits

- Imports and stripe conversions use temporary files, `fsync`, content checks, and transaction records before changing visible paths. Interrupted transactions are recovered on the next mount or `stripefs recover`. Verify the mount and backups after an unexpected shutdown before discarding anything.
- Original path handoff uses a symlink. Programs that require a real directory entry at that path, or that refuse symlinks, may need their paths changed to the StripeFS mount. Open files should be closed before a path handoff or conversion.
- FUSE adds latency and metadata overhead. Sequential reads of a sufficiently large file can improve; small and random I/O may be slower. Writes to striped files return `EROFS`. Use `stripefs remove PATH` before modifying them.
- There is no redundancy, automatic repair, or hot disk removal. If a disk is missing, restore it at its original mountpoint and restart `stripefs.service`; restore damaged data from your independent backup.
- The current importer preserves mode and timestamps. It does not preserve hard-link identity, owners when run unprivileged, or all ACL/xattr details. Avoid relying on those for imported trees until this is improved.

## Benchmarks and tests

`test_striping.py`, `test_three_disks.py`, and `test_cli.py` exercise an isolated mount. `benchmark_stripefs.py` creates temporary files on the configured disks, compares selected-file reads with direct ext4, and removes test data afterward. Results and methodology are in [RESULTS.md](RESULTS.md).

## Architecture

Each optimized logical file becomes a small stub with `user.stripefs.id`. Disk 0 holds its chunk map; every configured disk has one segment file for its assigned chunks under `.stripefs/chunks/<id>/data`. The map stores the target disk and offset for each 4 MiB logical chunk. Reads locate map entries and issue parallel readahead on the backing segments. These internals are hidden from the FUSE mount. A plain file remains a normal ext4 file and takes the direct passthrough path through FUSE.

## License

All original StripeFS code and documentation in this repository, including
revisions committed before this license was added, are offered under the
[Zero-Clause BSD license](LICENSE) (`0BSD`). It allows use, modification, and
redistribution without an attribution requirement.
