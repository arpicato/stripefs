# StripeFS measurements

These workload-specific results use plain ext4 as the baseline. Cold-cache
methods, swap pressure, and application behavior matter when interpreting
the timings. Application measurements below predate the test host's RAM
upgrade.

## Tabby optimization baseline — 2026-10-19

The pre-optimization source matched the installed daemon byte-for-byte.
Two unchanged cold starts: ready55.66/57.27s, engine35.4/37.4s;
generation74.06/76.27tok/s. Profile `/tmp/opencode/stripe-profile-baseline.jsonl`
and results `/tmp/opencode/stripe-profile-baseline.json` on the test host.
One-second block counters show both disks reading concurrently, with bursts
around900MiB/s combined but many slower intervals. Daemon CPU during sustained
reads is usually100–130% of one core; SSD busy times generally60–75%/55–65%.
Available RAM stays above2.5GiB. This rules out a complete lack of parallel
disk reads, but does not isolate CPU overhead from read-ahead or setup pauses.
No filesystem change yet. Test duration145.1s, two complete runs.
Only the actual `~/code/stripefs` checkout is the source of subsequent changes;
the test host's standalone snapshot repository is historical deployment evidence.

### Duplicate-cache experiment checkpoint

The second baseline trial wrote roughly700MiB into root-backed swap during
its slow disk interval; root writes reached26–90MiB/s. `MemAvailable` alone
missed this pressure. Candidate: native FUSE direct reads for striped files
only when `FUSE_CAP_DIRECT_IO_ALLOW_MMAP` is supported; backing ext4 cache
and explicit stripe read-ahead remain. Older kernels retain cached behavior.
Capability self-check failed before implementation, then passed. Isolated
two-/three-disk, mmap/random read and CLI integration tests all pass.
No application performance conclusion yet. Unchanged daemon retained at
the test host `/tmp/opencode/stripefsd-before-cache-test` for rollback.

First application result for the direct-I/O candidate: ready42.74/44.82s, engine24.4/24.5s,
TTFT0.475/0.504s, generation82.95/80.74tok/s. Two complete cold runs,
101.0s total; `/tmp/opencode/stripe-profile-direct-cache.{json,jsonl}`.
Baseline peak swap growth709.2MiB and root writes1360.7MiB versus0.2MiB
and6.0MiB with direct reads. Block writes are system-wide, not writes to
read-only model weights. This supports duplicate-cache pressure as a cause;
repeat old/new validation pending before final verdict.

Rollback to the retained original daemon: engine67.3/37.7s, ready86.47/57.15s;
peak swap growth3017.4MiB, root writes3736.5MiB. Two complete trials,
175.9s; `/tmp/opencode/stripe-profile-old-repeat.{json,jsonl}`.
Large run-to-run variation makes a precise percentage premature. Reapplying
the candidate and checking that low-swap/faster loading returns is next.

### Reapplied result and verdict

Reapplying the direct-I/O candidate restored engine24.2/23.9s, ready44.75/42.90s;
peak swap growth0MiB and root writes5.9MiB. Two complete runs,101.3s;
`/tmp/opencode/stripe-profile-new-repeat.{json,jsonl}`. Generation77.43/79.74tok/s.
Old/new/old/new engine times: 35.4–37.4 / 24.4–24.5 / 37.7–67.3 / 23.9–24.2s.
The change reproducibly removes swap growth and improves this host's loading.
Reduced memory pressure is a mechanism of the change, not a separately
controlled variable. No claim of faster raw SSDs or benefit without swapping;
no no-swap experiment performed. Inference rate is not demonstrably improved.
Candidate left installed; all GPU services restored inactive. No models,
original backups, stripe layout, swap settings, drivers or AI config changed.

Regression checks on the test host: `test_striping.py` (includes mmap/random reads),
`test_cli.py`, `test_three_disks.py`, and native capability selection:

```sh
cc -O2 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 test_cache_mode.c -o /tmp/opencode/test-cache-mode $(pkg-config --cflags --libs fuse3) -pthread
/tmp/opencode/test-cache-mode
```

Test host: Linux 7.0.0-38-generic on 19 October 2026. Backing filesystems are ext4 on a SanDisk X400 M.2 SATA SSD (`/dev/sda5`) and an Intel SSDSC2KB48 SATA SSD (`/dev/sdb1`). Tests used temporary files and left existing model data untouched. The installed service measured speed weights of 475:423 MiB/s.

## Sequential single-file read

The corrected benchmark used the service's 475:423 weights, 3 GiB files, and three trials. It wrote source files with direct I/O, striped one file, and read each path to `/dev/null` with 4 MiB `dd` blocks. It requested page-cache eviction through `posix_fadvise(POSIX_FADV_DONTNEED)` before each run and varied test order. Reads were buffered. `/sys/block/{sda,sdb}/stat` confirmed that the full 3 GiB came from the SSDs in each striped trial.

| Path | Median | Range | Relative to faster ext4 disk |
| --- | ---: | ---: | ---: |
| StripeFS, one striped file | 985 MiB/s | 979–995 | 1.99× |
| ext4, SanDisk | 481 MiB/s | 475–482 | 0.97× |
| ext4, Intel | 496 MiB/s | 495–496 | 1.00× |

StripeFS placed 1,624 MiB on the SanDisk and 1,448 MiB on the Intel. The physical read counters rose by those amounts on every striped trial. This is close to the two drives' combined direct-read rate on this workload.

The original benchmark forced 2:1 placement, sending about 1,364 MiB of a 2 GiB file to one SSD and 684 MiB to the other. It measured 767 MiB/s median striped versus 487 MiB/s on the faster individual SSD, or 1.58×. A repeated 2:1 trial measured 752 MiB/s; physical counters again showed the full 2 GiB came from the SSDs. The uneven placement, rather than a cache artifact, was the main reason for that lower gain. The benchmark now defaults to even weights and accepts `--weights` for other disk-speed ratios.

## Random 4 KiB read and latency

The 3 GiB run then issued 4,096 identical random 4 KiB `pread` offsets to each path after advisory eviction. Requests were sequential within each case (queue depth one).

| Path | IOPS | Median latency | P95 latency |
| --- | ---: | ---: | ---: |
| StripeFS | 7,265 | 0.123 ms | 0.173 ms |
| ext4, SanDisk | 7,510 | 0.128 ms | 0.160 ms |
| ext4, Intel | 9,396 | 0.099 ms | 0.138 ms |

StripeFS improves large sequential reads on this host; it does not improve random reads at queue depth one. The faster direct disk wins that workload.

## Application loading

The application test used `safetensors` 0.8.0, NumPy 2.5.4, and `ml_dtypes` 0.6.0 in a temporary virtual environment. It loaded BF16 tensors from a 3.69 GiB model shard on ext4 into CPU NumPy arrays one at a time, copied every tensor, and sampled its bytes for a checksum. The original model path remained unchanged. A verified copy was striped with the installed speed weights: 2,092,957,696 data bytes on the SanDisk and 1,864,943,144 on the Intel. These total 3,957,900,840 bytes, the source file's full size including its safetensors header. `benchmark_model.py` requested advisory eviction of both files and stripe segments before each load.

| Trial | Direct ext4 | StripeFS | Checksum matched |
| --- | ---: | ---: | --- |
| 1 | 19.47 s | 11.58 s | yes |
| 2 | 21.57 s | 5.57 s | yes |

The second striped result likely benefited from cache despite eviction. The first trial is a more conservative indication of the application-level gain; neither trial proves a cold-cache guarantee. The temporary striped copy and chunks were removed after testing, and the original model remained intact.

## Reproduce

On this test host, from the StripeFS checkout:

```sh
python3 benchmark_stripefs.py --size-mib 3072 --repetitions 3 --weights 475,423 --devices sda,sdb
```

The benchmark needs free space for a 3 GiB file on each disk plus temporary stripe data. It removes its temporary data when finished. `--devices` is optional and reports physical read volume from Linux block counters.
