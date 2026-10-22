# Changelog

All notable changes are listed here. Each phase of the roadmap ships as a release.

## [0.2.0] — Phase 2: Duplicates

### Added
- **Duplicates view** in the desktop app (Ctrl/⌘+2)
  - Searches the folder you are browsing; size filter, skip hidden files, optional byte-by-byte check
  - Groups sorted by reclaimable space; folders shown relative to the search root
  - Keep rules: oldest, newest, shortest path, longest path, alphabetical; "keep only this copy"
  - Actions: **Move to Trash** (default), replace with **hard links**, replace with **space-sharing
    clones** (APFS / Btrfs / XFS), or **delete permanently**
  - It is impossible to select every copy of a file
- **History view** (Ctrl/⌘+3): every batch of changes with per-file results, **Undo** and **Resume**
- On startup, work interrupted by a crash or power cut is checked and can be resumed
- **CLI**: `storagectl dupes` (dry run unless `--apply`), `history`, `undo`, `resume`
- `Storage <path> --find-duplicates` opens a folder straight into a duplicate search

### Safety
- Write-ahead **operation journal** (SQLite, `synchronous=FULL`): each operation is recorded before it
  touches the disk
- Before acting, every file is re-checked against what the plan saw (size + modified time); before
  delete / hardlink / clone, both the file and the kept copy are **re-hashed**
- Replacements are atomic (temp file + rename); temp files are cleaned up by recovery
- Moves (used by the organizer next) never overwrite; cross-drive moves copy → verify → publish → remove
- Crash tests kill the process at every step of an operation and prove no file is lost or duplicated

### Performance
- SHA-256 uses ARMv8 crypto instructions or Intel SHA-NI when available (~4× faster)
- Hashes are cached in the index by path + size + modified time, so repeat searches are near-instant
- Partial hash (first + last 64 KB) rules out most look-alikes before reading whole files

## [0.1.0] — Phase 1: Foundation & Disk Usage

### Added
- **Desktop app** (Qt 6, light/dark/system themes)
  - Sidebar with common folders, mounted drives (with free-space bars) and saved scans
  - Sortable folder tree with size, % of parent bar, file count and modified date
  - Squarified, nested treemap coloured by file type; hover for details, click to select,
    double-click to drill in
  - File-type breakdown bar for the current folder
  - Breadcrumbs, back / forward / up navigation, reveal in Finder / Explorer, copy path
  - Saved scans reopen instantly from the index
  - `Storage <path>` opens a path on launch; `--screenshot` for docs and headless smoke tests
- **`storagectl` CLI**: `scan`, `du` (live or `--indexed`), `scans`; `--json` on every command
- **Core library** (no Qt):
  - Parallel scanner using native APIs (`readdir`/`fstatat`, `FindFirstFileEx`)
  - Hard links counted once; symlinks and junctions never followed
  - Mount-aware defaults (skips `/proc`, `/sys`; handles macOS firmlinks so data isn't counted twice)
  - SQLite index of scans
- No third-party dependencies apart from the vendored SQLite amalgamation
- CI on Linux, macOS and Windows; tagged releases build a `.dmg`, `.AppImage` and Windows `.zip`
