# Changelog

All notable changes are listed here. Each phase of the roadmap ships as a release.

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
