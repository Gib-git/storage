# Storage

A small, fast, cross-platform (macOS · Linux · Windows) app that indexes your drives and NAS,
shows what is using space, finds duplicates, cleans junk and reorganizes folders — **without
ever losing a file**, even if the power goes out halfway through.

> Status: **Phase 1 — Foundation & Disk Usage** (see [Roadmap](#roadmap)).

## Features

| | Feature | Phase |
|---|---|---|
| ✅ | Parallel scanner (hundreds of thousands of files per second on SSDs) | 1 |
| ✅ | Persistent SQLite index — browse previous scans instantly | 1 |
| ✅ | Folder size browser: sortable tree, breadcrumbs, treemap | 1 |
| ✅ | `storagectl` CLI with `--json` output for scripts and AI agents | 1 |
| ⏳ | Duplicate finder (size → partial hash → full hash → optional byte compare) | 2 |
| ⏳ | Rule-based organizer with dry-run preview, crash-safe journal and undo | 3 |
| ⏳ | System & developer cache cleanup | 4 |
| ⏳ | NAS over SSH (uses your existing `ssh` — no extra software) | 5 |
| ⏳ | REST API, MCP server for Claude, Ollama-assisted organizing | 6 |

## No dependencies

Downloads are self-contained — nothing to install. Building from source needs only a C++20
compiler and CMake (plus Qt 6 for the desktop app). The only third-party code is SQLite,
vendored in [`third_party/`](third_party/README.md); JSON, argument parsing, hashing, HTTP and
the test harness are implemented here.

## Install

Grab the latest build for your OS from [Releases](https://github.com/Gib-git/storage/releases).

- **macOS:** open the `.dmg`, drag *Storage* to Applications. Builds are not notarized yet, so the
  first launch needs right-click → *Open* (or `xattr -dr com.apple.quarantine /Applications/Storage.app`).
- **Linux:** `chmod +x Storage-*.AppImage && ./Storage-*.AppImage`
- **Windows:** unzip and run `Storage.exe`.

## CLI

```sh
storagectl scan ~                 # scan and save to the index
storagectl du ~ -d 2 -n 10        # largest folders, two levels deep
storagectl du -i ~/Projects       # answer from the saved index, no rescan
storagectl scans --json           # machine-readable list of saved scans
```

## Build from source

```sh
# CLI only — no Qt needed
cmake --preset cli && cmake --build --preset cli && ctest --preset cli

# Everything (needs Qt 6.5+: brew install qt / apt install qt6-base-dev / Qt online installer)
cmake --preset release && cmake --build --preset release && ctest --preset release
```

## Roadmap

1. **v0.1 Foundation & Disk Usage** — scanner, index, size browser, treemap, CLI
2. **v0.2 Duplicates** — confirmed duplicate groups, keep-policies, trash/quarantine, hardlink/clone replacement
3. **v0.3 Organizer** — rules + templates, presets, dry-run diff, journaled moves, recovery, undo
4. **v0.4 Cleanup** — OS & developer caches, large/old files, empty folders
5. **v0.5 NAS over SSH** — remote scan/hash/organize executed on the NAS itself
6. **v0.6 Automation & AI** — REST API, MCP server, Ollama suggestions
7. **v1.0 Polish** — settings, scheduled scans, file watching, installers

See [docs/SUGGESTIONS.md](docs/SUGGESTIONS.md) for ideas beyond the roadmap.

## License

[MIT](LICENSE)
