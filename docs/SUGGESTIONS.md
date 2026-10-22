# Feature suggestions

Ideas beyond the core roadmap, with a note on how each is best approached.
Items marked ★ give the most value for the effort.

| Idea | Why it helps | How to build it well |
|---|---|---|
| ★ **Live index via filesystem watching** | The index stays current without rescans | FSEvents (macOS), inotify/fanotify (Linux), `ReadDirectoryChangesW` / USN journal (Windows). Apply events to the tree; fall back to a rescan if events overflow. |
| ★ **Copy-free dedupe with clones** | Get the space back but keep every path working | APFS `clonefile`, Btrfs/XFS `FICLONE`, ReFS block cloning. It's safer than hardlinks because editing one copy doesn't change the other. |
| ★ **Snapshot before organizing** | A second safety net on top of the journal | `tmutil localsnapshot` (macOS), Btrfs/ZFS snapshots over SSH on the NAS, VSS (Windows). Offer it as an optional checkbox. |
| **Similar-image finder** | Catches resized, re-encoded or lightly edited photos | Perceptual hashes (dHash/pHash) on thumbnails; BK-tree for Hamming-distance search. Review side by side; never auto-delete. |
| **Media-aware organizing** | Sort photos and music by real metadata | EXIF `DateTimeOriginal`/GPS, ID3/MP4 tags, PDF creation date. Read only the headers — a few KB per file. |
| **Rules as shareable files** | Reuse and version-control organize rules | Store rules as JSON; support a per-folder `.storage-rules` that applies automatically on scan. |
| **Cloud drives via rclone** | Treat Google Drive / S3 / OneDrive like the NAS | Shell out to `rclone lsjson`/`moveto` behind the same `FileSystem` interface (no new libraries). |
| **Space-over-time reports** | See which folders keep growing | Keep summary rows per scan (not full trees); chart growth, export HTML/CSV. |
| **Disk health** | Warn before a drive dies | Run `smartctl -j` locally and over SSH; show SMART status beside each drive. |
| **Headless NAS agent** | Fastest possible NAS indexing | The core has no Qt, so `storagectl` can be cross-compiled (static musl) and run on the NAS, streaming JSON back over SSH. |
| **Scheduled cleanups** | Hands-off maintenance | OS schedulers (launchd, systemd timers, Task Scheduler) calling `storagectl clean --profile … --yes`. |
| **Large-file and stale-file finders** | Quick wins in big folders | Queries over the index: size ≥ N, not modified in M months, by type. |
