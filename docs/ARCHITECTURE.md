# Architecture

```
src/core/   libstoragecore — plain C++20, no Qt. Everything that matters lives here.
src/cli/    storagectl — command-line front end (+ REST and MCP servers)
src/gui/    Storage — Qt 6 Widgets desktop app
tests/      self-contained test harness (no framework dependency)
third_party/sqlite/   the only third-party code (public-domain amalgamation)
```

Both front ends are thin. The CLI and the app call the same core classes, and so do the REST API
and the MCP server (through `Service`).

## Core building blocks

| Piece | File | Role |
|---|---|---|
| `FileSystem` | `filesystem.hpp` | One interface for a disk. Read side: `stat`, `list`, `walkTree`, hashing, `readHead`. Write side: atomic, never-overwriting operations. |
| `LocalFileSystem` | `local_fs.cpp`, `local_ops.cpp`, `trash*.{cpp,mm}` | Native APIs on each OS: `readdir`/`fstatat`, `FindFirstFileEx`, `renamex_np`/`renameat2`/`MoveFileEx`, `clonefile`/`FICLONE`, and the OS trash. |
| `SshFileSystem` | `ssh_fs.cpp`, `process.cpp` | The same interface on a NAS, driven through the system `ssh` client with pooled shell sessions. |
| `Scanner` | `scanner.cpp` | Parallel tree walk → `ScanTree` of `Node`s with totals per folder. |
| `Index` | `index.cpp` | SQLite store of scans and the content-hash cache. |
| `DuplicateFinder` | `duplicates.cpp` | Size → partial SHA-256 → full SHA-256 → optional byte compare. |
| `Organizer` | `organize.cpp`, `exif.cpp` | Rules + templates → `OrganizePlan` (moves, folders to create and remove). |
| Cleaner | `cleaner.cpp` | Per-OS catalogue of caches and junk, plus insights over scans. |
| `Journal` | `journal.cpp` | Write-ahead record of every file operation (SQLite, `synchronous=FULL`). |
| `Executor` | `executor.cpp` | Runs, resumes, undoes and recovers journaled batches. |
| `Service` | `service.cpp`, `api.cpp`, `http.cpp` | Tools for agents; MCP over stdio; REST over a tiny HTTP server. |
| AI | `ai.cpp` | Folder summary → Ollama → validated `RuleSet`. |

## The one rule: plan, journal, verify, act

Every feature that changes files produces a list of `JournalOp`s instead of touching the disk.
The `Executor` is the only thing that carries operations out, and each one is:

1. recorded as *started* (flushed to disk);
2. re-checked against what the plan saw (size and modified time; for content-destroying
   actions, a fresh hash of both copies);
3. performed atomically (temp name + rename, or a never-overwrite rename/link);
4. recorded as *done*, *skipped* or *failed*.

`Executor::recover` resolves operations left *started* by a crash by checking the disk. Undo
replays a batch backwards. [SAFETY.md](SAFETY.md) explains the guarantees, and
`tests/test_executor.cpp` kills a forked process at every step to prove them.

## Threading

- Scans, hashing, planning and execution run on worker threads. The GUI only polls atomic progress
  counters and receives results back on the UI thread (`BackgroundTask`).
- SQLite is built thread-safe, but each connection is used by one thread at a time.
- `SshFileSystem` is thread-safe: each call borrows one pooled session.

## Adding a feature

1. Put the logic in `src/core`. Changes to files must go through `JournalOp`s and the `Executor`.
2. Add tests in `tests/` (the harness is `check.hpp`; `TempTree` builds throwaway folders).
3. Expose it in `storagectl` (with `--json`), as a `Service` tool if agents should use it, and
   in the GUI.
