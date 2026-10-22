# Changelog

All notable changes are listed here. Each phase of the roadmap ships as a release.

## [0.4.0] — Phase 4: Cleanup

### Added
- **Clean Up view** (Ctrl/⌘+4; History moves to Ctrl/⌘+5)
  - **System & Apps**: per-OS catalogue of cleanable locations, measured in the background and
    grouped (System, Apps, Browsers, Developer), each marked **Safe**, **Review** or **Caution**
    - macOS: app caches, logs, Mail attachment cache, Trash, iPhone/iPad backups, Xcode DerivedData,
      device support, archives, simulator caches
    - Windows: user and system temp, crash dumps, INet cache, thumbnail cache, Chrome/Edge caches
    - Linux: `~/.cache`, thumbnails, Trash
    - Everywhere: npm, pip, Gradle and Cargo caches, downloads untouched for 6 months
  - Commands you can copy and run yourself for things that need admin rights or another tool
    (`apt-get clean`, `journalctl --vacuum-time`, `brew cleanup`, `docker system prune`, DISM…)
  - **In This Folder**: rebuildable project folders (`node_modules`, `target`, `.venv`,
    `__pycache__`, …) in projects untouched for months, large files not modified recently,
    and empty folders
- **CLI**: `storagectl clean [--all] [--only IDS] [--trash] [--apply]`,
  `storagectl insights <path> [--older DAYS] [--min-size SIZE]`
- `Storage --cleanup` opens straight into the measured Clean Up view

### Safety
- Only items marked *Safe* (caches that rebuild themselves) are ever deleted permanently, and
  only with the box ticked. Everything else goes to the Trash.
- Work is journaled per top-level item, so huge caches stay manageable and are shown in History.
- Folder deletions never follow symbolic links; files that are in use are skipped and reported.
- Project-junk detection ignores the junk folder's own timestamps and only flags projects whose
  real files have not changed.

## [0.3.0] — Phase 3: Organizer

### Added
- **Organize view** (Ctrl/⌘+3; History moves to Ctrl/⌘+4)
  - Six built-in rule sets: *By file type*, *By type, then year*, *Photos & videos by date*,
    *By year and month*, *By extension*, *Tidy Downloads*
  - Rule editor: match by kind of file, extension, name text, regular expression, size and age;
    send matches to a folder template, rename them, or leave them where they are
  - "Everything else" fallback, organize in place or into another folder, choose date source,
    keep-both or skip on name clashes, include subfolders, remove emptied folders, hidden files
  - Live **After organizing** preview tree before anything moves
  - Save, import and export your own rule sets (JSON, in the app data `rules/` folder)
- Templates: `{type} {ext} {EXT} {name} {filename} {year} {month} {day} {monthname} {size} {first} {parent}`
- Photos are dated by their **EXIF "date taken"** (JPEG and TIFF-based RAW: DNG, CR2, NEF, ARW…),
  read in parallel; other files use the modified date
- **CLI**: `storagectl organize <path> [--preset NAME | --rules FILE] [--dest DIR] [--apply]`,
  `storagectl presets [--show NAME]`
- `Storage <path> --organize` opens a folder straight into an organize preview

### Safety
- App bundles, photo/music/video libraries, `.git`, `node_modules` and similar packages are never
  moved into or out of
- File and folder names are made safe for every OS (reserved names, illegal characters, length)
- Name clashes are resolved in the plan (`name (2).ext`), and the executor still refuses to
  overwrite at run time
- Folders the organizer creates are journaled; **undo removes them again** (never a folder that
  existed before). Emptied source folders are removed only if nothing but OS junk
  (`.DS_Store`, `Thumbs.db`, `desktop.ini`) is left, and are recreated on undo

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
