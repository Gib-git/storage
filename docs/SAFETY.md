# How Storage keeps your files safe

Storage changes files in only three situations: removing duplicates, organizing folders and
cleaning up junk. All three go through the same pipeline, built so that **no file is ever lost —
even if the app crashes or the power goes out halfway through.**

## 1. Plan first

Nothing is changed directly. Every action first becomes a *plan*: a list of operations
("move this file there", "replace this duplicate with a hard link to that copy"). The app shows
the plan and waits for you to confirm it. The CLI is a dry run unless you pass `--apply`.

Each operation in the plan records what the file looked like when the plan was made: its
**size**, **modified time** and, for duplicates, its **SHA-256 content hash**.

## 2. Write it down before doing it

Plans are stored in a journal: a SQLite database in the app's data folder, opened with
`synchronous=FULL`. That setting means a write is not considered done until the operating system
confirms the data is physically on disk.

For every single file the executor:

1. marks the operation **started** in the journal (on disk);
2. checks the file is still exactly what the plan saw (size and modified time);
3. for actions that discard data (delete, hard link, clone), **re-hashes both the file and the
   copy being kept** and refuses if they no longer match;
4. performs the operation (see below);
5. marks it **done**, **skipped** (with the reason) or **failed** (with the error).

## 3. Never half-do anything

| Operation | How it stays atomic |
|---|---|
| Move to Trash | The operating system's own trash (Finder / Recycle Bin / freedesktop.org Trash) |
| Hard link / clone | Built under a temporary name next to the file, then swapped in with one atomic `rename` |
| Move (same drive) | One atomic rename that **refuses to overwrite** (`renamex_np(RENAME_EXCL)`, `renameat2(RENAME_NOREPLACE)`, `MoveFileEx` without replace) |
| Move (other drive) | Copy to a temp name → flush to disk → **re-hash and compare** → atomic rename into place → only then remove the original |
| Delete permanently | Only after the content re-check in step 3, and only when an identical copy remains |

Temporary files always end in `.storage-tmp`. They are never the only copy of anything, so
recovery can safely remove them.

## 4. Recover after a crash

When the app or `storagectl` starts, it looks for operations left in the **started** state and
checks the disk to see how far each one got:

- **Move:** if the file is already at its destination and gone from the source, the move is
  marked done. If it's at both, the two copies are compared: when identical, the original is
  removed; when not, both are kept and flagged for you. If it's only at the source, the move is
  retried.
- **Hard link:** if the duplicate already shares the kept copy's inode, the step is marked done;
  otherwise it is retried.
- **Trash / delete:** if the file is gone, the step is marked done; otherwise it is retried.

The interrupted batch can then be **resumed** from History (or `storagectl resume <id>`).

The test suite covers this directly. It starts a batch in a child process and kills that process
with `_exit()` at every step of an operation: after marking it started, after the copy, after
publishing, after the rename. It then runs recovery and checks that every original file's bytes
exist **exactly once**, in a valid location, with no temp files left behind.

## 5. Undo

Every completed batch can be undone from History or with `storagectl undo <id>`:

- trashed files are restored from the Trash (on Windows, use the Recycle Bin's own *Restore*,
  because Windows doesn't report where a recycled file went);
- hard links and clones become independent copies again;
- moves are moved back, using the same never-overwrite rules.

Permanent deletions are the only thing that cannot be undone, which is why the app asks twice and
re-checks contents first.

## 6. Things that are never touched

- Files that changed after the plan was made are skipped and reported.
- Hard links to the same file aren't counted as duplicates, because removing one frees no space.
- Version-control folders (`.git`, `.svn`, `.hg`) and `node_modules` are skipped by the duplicate
  finder by default.
- Symbolic links and Windows junctions are never followed.
