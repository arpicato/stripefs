# StripeFS

- Use plain ext4 as the baseline for performance comparisons and reports.
  Report swap growth from each trial's own starting usage.
- This repository is the canonical source. Deployment copies may have separate
  histories; do not treat them as upstream.
- Preserve all retained model originals and StripeFS internal backups. Check
  available space before importing additional large files.
- Before restarting `stripefs.service`, stop and check all consumers of its
  FUSE mount. Owner-only mounts may not be visible to a root `fuser` check.
- Historical Tabby and synthetic I/O measurements are in `RESULTS.md`.
