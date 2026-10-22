#include "storage/core/executor.hpp"

#include "storage/core/organize.hpp"

namespace storage {

namespace {

bool exists(FileSystem& fs, const std::string& path, DirEntry* out = nullptr) {
    DirEntry e;
    if (fs.stat(path, e)) return false;
    if (out) *out = e;
    return true;
}

std::string message(const std::error_code& ec) { return ec.message(); }

}  // namespace

Executor::Executor(FileSystem& fs, Journal& journal, ExecOptions options)
    : fs_(fs), journal_(journal), opt_(std::move(options)) {}

void Executor::hook(const JournalOp& op, const char* point) const {
    if (opt_.faultHook) opt_.faultHook(op, point);
}

void Executor::setCurrent(const std::string& s) {
    std::lock_guard lk(currentMu_);
    current_ = s;
}

ExecProgress Executor::progress() const {
    ExecProgress p;
    p.done = done_;
    p.total = total_;
    p.bytes = bytes_;
    std::lock_guard lk(currentMu_);
    p.current = current_;
    return p;
}

// --- Forward ---------------------------------------------------------------------

ExecResult Executor::run(int64_t batch) {
    ExecResult r;
    auto ops = journal_.ops(batch);
    int pending = 0;
    for (const auto& op : ops)
        if (op.state == OpState::Pending || op.state == OpState::Started) ++pending;
    total_ = pending;
    done_ = 0;
    journal_.setBatchState(batch, BatchState::Running);

    for (auto& op : ops) {
        if (op.state != OpState::Pending && op.state != OpState::Started) continue;
        if (cancel_) {
            r.cancelled = true;
            break;
        }
        setCurrent(op.src);
        journal_.setOp(op.id, OpState::Started);
        hook(op, "started");

        std::string detail, newDst = op.dst;
        Outcome o;
        try {
            o = perform(op, detail, &newDst);
        } catch (const std::system_error& e) {
            o = Outcome::Failed;
            detail = e.what();
        }
        switch (o) {
            case Outcome::Done:
                journal_.setOp(op.id, OpState::Done, detail, &newDst);
                ++r.done;
                r.bytes += op.size;
                bytes_ += op.size;
                break;
            case Outcome::Failed:
                journal_.setOp(op.id, OpState::Failed, detail);
                ++r.failed;
                break;
            case Outcome::Skipped:
                journal_.setOp(op.id, OpState::Skipped, detail);
                ++r.skipped;
                break;
        }
        ++done_;
    }
    journal_.setBatchState(batch, r.cancelled ? BatchState::Interrupted : BatchState::Done);
    setCurrent({});
    return r;
}

Executor::Outcome Executor::performRemoveDir(const JournalOp& op, std::string& detail) {
    std::vector<DirEntry> entries;
    if (fs_.list(op.src, entries)) {
        detail = "already gone";
        return Outcome::Skipped;
    }
    for (const auto& e : entries) {
        if (e.kind != NodeKind::File || !isJunkFile(e.name)) {
            detail = "not empty";
            return Outcome::Skipped;
        }
    }
    for (const auto& e : entries) fs_.removeFile(fs_.join(op.src, e.name));  // .DS_Store, Thumbs.db, desktop.ini
    if (auto ec = fs_.removeDir(op.src)) {
        detail = ec.message();
        return Outcome::Failed;
    }
    detail = "removed empty folder";
    return Outcome::Done;
}

Executor::Outcome Executor::perform(JournalOp& op, std::string& detail, std::string* newDst) {
    if (op.action == Action::RemoveDir) return performRemoveDir(op, detail);
    if (op.action == Action::MakeDir) {
        DirEntry e;
        if (!fs_.stat(op.src, e)) {
            detail = "already existed";  // skipped => undo leaves it alone
            return Outcome::Skipped;
        }
        if (auto ec = fs_.makeDirs(op.src)) {
            detail = ec.message();
            return Outcome::Failed;
        }
        detail = "created folder";
        return Outcome::Done;
    }

    // 1. The file must be exactly what the plan saw.
    DirEntry src;
    if (!exists(fs_, op.src, &src)) {
        detail = "no longer exists";
        return Outcome::Skipped;
    }
    const bool folderAction = op.action == Action::DeleteTree || (op.action == Action::Trash && op.ref.empty());
    if (src.kind != NodeKind::File && !(folderAction && (src.kind == NodeKind::Directory || src.kind == NodeKind::Symlink))) {
        detail = "is not a regular file";
        return Outcome::Skipped;
    }
    if (src.kind == NodeKind::File && (op.size || op.mtime) && (src.size != op.size || src.mtime != op.mtime)) {
        detail = "changed since it was checked";
        return Outcome::Skipped;
    }

    // 2. A reference copy, when there is one, must still be there and unchanged.
    if (!op.ref.empty()) {
        DirEntry ref;
        if (!exists(fs_, op.ref, &ref)) {
            detail = "the copy being kept is missing";
            return Outcome::Skipped;
        }
        if (ref.size != op.size) {
            detail = "the copy being kept has changed";
            return Outcome::Skipped;
        }
        if (src.ino && src.ino == ref.ino && src.dev == ref.dev) {
            detail = "already the same file (hard link)";
            return Outcome::Skipped;
        }
        const bool destroysContent =
            op.action == Action::Delete || op.action == Action::Hardlink || op.action == Action::Clone;
        if (destroysContent && opt_.verifyContent) {
            std::string hs, hr;
            if (auto ec = fs_.hashFull(op.src, hs, &cancel_, nullptr)) {
                detail = "could not re-check contents: " + message(ec);
                return Outcome::Failed;
            }
            if (auto ec = fs_.hashFull(op.ref, hr, &cancel_, nullptr)) {
                detail = "could not re-check the kept copy: " + message(ec);
                return Outcome::Failed;
            }
            if (hs != hr || (!op.hash.empty() && hs != op.hash)) {
                detail = "contents no longer identical";
                return Outcome::Skipped;
            }
        }
    }

    // 3. Act.
    std::error_code ec;
    if (op.action == Action::Trash) {
        std::string location;
        ec = fs_.trash(op.src, location);
        if (!ec) {
            *newDst = location;
            detail = location.empty() ? "moved to Trash" : "moved to Trash: " + location;
        }
    } else if (op.action == Action::Delete) {
        ec = fs_.removeFile(op.src);
        if (!ec) detail = "deleted";
    } else if (op.action == Action::DeleteTree) {
        ec = src.kind == NodeKind::Directory ? fs_.removeTree(op.src) : fs_.removeFile(op.src);
        if (!ec) detail = "deleted";
    } else if (op.action == Action::Hardlink) {
        ec = fs_.replaceWithHardlink(op.src, op.ref);
        if (!ec) detail = "replaced with a hard link";
    } else if (op.action == Action::Clone) {
        ec = fs_.replaceWithClone(op.src, op.ref);
        if (!ec) detail = "replaced with a space-sharing clone";
    } else if (op.action == Action::Move) {
        return performMove(op, op.src, op.dst, detail);
    } else {
        detail = "unknown action " + op.action;
        return Outcome::Failed;
    }
    if (ec) {
        detail = message(ec);
        return Outcome::Failed;
    }
    return Outcome::Done;
}

Executor::Outcome Executor::performMove(JournalOp& op, const std::string& from, const std::string& to,
                                        std::string& detail) {
    if (exists(fs_, to)) {
        detail = "destination already exists";
        return Outcome::Skipped;
    }
    if (auto ec = fs_.makeDirs(fs_.parentOf(to))) {
        detail = "cannot create folder: " + message(ec);
        return Outcome::Failed;
    }

    std::error_code ec = opt_.forceCopy ? std::make_error_code(std::errc::cross_device_link)
                                        : fs_.renameNoReplace(from, to);
    if (!ec) {
        hook(op, "renamed");
        detail = "moved";
        return Outcome::Done;
    }
    if (ec != std::errc::cross_device_link) {
        detail = message(ec);
        return ec == std::errc::file_exists ? Outcome::Skipped : Outcome::Failed;
    }

    // Different volumes: copy to a temp name, verify, publish, and only then remove the source.
    const std::string tmp = to + FileSystem::kTempSuffix;
    fs_.removeFile(tmp);  // leftover from an interrupted attempt
    std::string expected = op.hash;
    if (expected.empty()) {
        if ((ec = fs_.hashFull(from, expected, &cancel_, nullptr))) {
            detail = "cannot read: " + message(ec);
            return Outcome::Failed;
        }
    }
    if ((ec = fs_.copyNew(from, tmp, &cancel_))) {
        fs_.removeFile(tmp);
        detail = "copy failed: " + message(ec);
        return Outcome::Failed;
    }
    hook(op, "copied");
    std::string actual;
    if ((ec = fs_.hashFull(tmp, actual, &cancel_, nullptr)) || actual != expected) {
        fs_.removeFile(tmp);
        detail = ec ? "cannot verify copy: " + message(ec) : "copy did not verify (contents differ)";
        return Outcome::Failed;
    }
    if ((ec = fs_.renameNoReplace(tmp, to))) {
        fs_.removeFile(tmp);
        detail = message(ec);
        return ec == std::errc::file_exists ? Outcome::Skipped : Outcome::Failed;
    }
    hook(op, "published");
    if ((ec = fs_.removeFile(from))) {
        // The verified copy is in place; the original stays too. Nothing is lost.
        detail = "copied, but the original could not be removed: " + message(ec);
        return Outcome::Done;
    }
    detail = "moved (copied across drives and verified)";
    return Outcome::Done;
}

// --- Undo ----------------------------------------------------------------------

ExecResult Executor::undo(int64_t batch) {
    ExecResult r;
    auto ops = journal_.ops(batch);
    int count = 0;
    for (const auto& op : ops)
        if (op.state == OpState::Done) ++count;
    total_ = count;
    done_ = 0;
    journal_.setBatchState(batch, BatchState::Running);

    // Reverse order so that later operations that depended on earlier ones unwind first.
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        JournalOp& op = *it;
        if (op.state != OpState::Done) continue;
        if (cancel_) {
            r.cancelled = true;
            break;
        }
        setCurrent(op.src);
        std::string detail;
        Outcome o;
        try {
            o = reverse(op, detail);
        } catch (const std::system_error& e) {
            o = Outcome::Failed;
            detail = e.what();
        }
        if (o == Outcome::Done) {
            journal_.setOp(op.id, OpState::Undone, detail);
            ++r.done;
            r.bytes += op.size;
        } else {
            // Leave the op "done" so the user can see it was not reverted, and why.
            journal_.setOp(op.id, OpState::Done, op.detail + " (undo: " + detail + ")");
            o == Outcome::Failed ? ++r.failed : ++r.skipped;
        }
        ++done_;
    }
    journal_.setBatchState(batch, r.cancelled || r.failed || r.skipped ? BatchState::Done : BatchState::Undone);
    setCurrent({});
    return r;
}

Executor::Outcome Executor::reverse(JournalOp& op, std::string& detail) {
    std::error_code ec;
    if (op.action == Action::Trash) {
        if (op.dst.empty()) {
            detail = "restore it from the Recycle Bin / Trash";
            return Outcome::Skipped;
        }
        if (exists(fs_, op.src)) {
            detail = "something already exists at the original path";
            return Outcome::Skipped;
        }
        ec = fs_.restore(op.dst, op.src);
        if (!ec) detail = "restored from Trash";
    } else if (op.action == Action::Delete || op.action == Action::DeleteTree) {
        detail = "permanently deleted files cannot be restored";
        return Outcome::Skipped;
    } else if (op.action == Action::RemoveDir) {
        ec = fs_.makeDirs(op.src);
        if (!ec) detail = "folder recreated";
    } else if (op.action == Action::MakeDir) {
        return performRemoveDir(op, detail);  // only removes it if it is empty again
    } else if (op.action == Action::Hardlink || op.action == Action::Clone) {
        DirEntry ref;
        if (!exists(fs_, op.ref, &ref) || ref.size != op.size) {
            detail = "the kept copy is missing or changed";
            return Outcome::Skipped;
        }
        ec = fs_.replaceWithCopy(op.src, op.ref);
        if (!ec) detail = "made an independent copy again";
    } else if (op.action == Action::Move) {
        if (!exists(fs_, op.dst)) {
            detail = "the moved file is no longer at its new location";
            return Outcome::Skipped;
        }
        JournalOp back = op;
        back.hash.clear();
        return performMove(back, op.dst, op.src, detail);
    }
    if (ec) {
        detail = message(ec);
        return Outcome::Failed;
    }
    return Outcome::Done;
}

// --- Crash recovery -----------------------------------------------------------------

std::vector<int64_t> Executor::recover(FileSystem& fs, Journal& journal) {
    std::vector<int64_t> recovered;
    // Only this filesystem's batches: checking a NAS path against the local disk (or the
    // reverse) would misjudge what finished.
    for (int64_t batch : journal.runningBatches(fs.volumeName())) {
        for (auto& op : journal.ops(batch)) {
            if (op.state != OpState::Started) continue;
            // Temp files are only ever created by us; they are never the sole copy of anything.
            const std::string tmpOf = (op.action == Action::Move ? op.dst : op.src) + FileSystem::kTempSuffix;
            if (exists(fs, tmpOf)) fs.removeFile(tmpOf);

            const bool srcThere = exists(fs, op.src);
            if (op.action == Action::Move) {
                const bool dstThere = exists(fs, op.dst);
                if (dstThere && !srcThere) {
                    journal.setOp(op.id, OpState::Done, "moved (completed before interruption)");
                } else if (dstThere && srcThere) {
                    // Cross-drive copy was published but the original not yet removed.
                    // The destination only appears after verification, but check again anyway.
                    std::string a, b;
                    if (!fs.hashFull(op.src, a, nullptr, nullptr) && !fs.hashFull(op.dst, b, nullptr, nullptr) && a == b) {
                        fs.removeFile(op.src);
                        journal.setOp(op.id, OpState::Done, "moved (finished during recovery)");
                    } else {
                        journal.setOp(op.id, OpState::Failed, "both copies kept: contents differ, please review");
                    }
                } else {
                    journal.setOp(op.id, OpState::Pending, "interrupted; will retry");
                }
            } else if (op.action == Action::Hardlink) {
                DirEntry s, r;
                const bool linked = srcThere && exists(fs, op.ref, &r) && !fs.stat(op.src, s) && s.ino && s.ino == r.ino;
                journal.setOp(op.id, linked ? OpState::Done : OpState::Pending,
                              linked ? "replaced with a hard link (completed before interruption)" : "interrupted; will retry");
            } else if (op.action == Action::MakeDir) {
                // Unknown whether we created it: retry, which marks it skipped if it exists,
                // so undo can never remove a folder that was there before.
                journal.setOp(op.id, OpState::Pending, "interrupted; will retry");
            } else if (op.action == Action::Trash || op.action == Action::Delete || op.action == Action::DeleteTree ||
                       op.action == Action::RemoveDir) {
                journal.setOp(op.id, srcThere ? OpState::Pending : OpState::Done,
                              srcThere ? "interrupted; will retry" : "completed before interruption");
            } else {
                // Clone: the swap is a single atomic rename; retrying is harmless.
                journal.setOp(op.id, OpState::Pending, "interrupted; will retry");
            }
        }
        journal.setBatchState(batch, BatchState::Interrupted);
        recovered.push_back(batch);
    }
    return recovered;
}

}  // namespace storage
