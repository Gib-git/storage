#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "storage/core/filesystem.hpp"
#include "storage/core/journal.hpp"

namespace storage {

struct ExecOptions {
    // Re-hash the file and the kept copy right before content-destroying actions
    // (delete, hardlink, clone) and refuse if they no longer match.
    bool verifyContent = true;
    // Testing: treat every move as cross-device to exercise the copy path.
    bool forceCopy = false;
    // Testing: called at named points inside an operation; may throw or exit to simulate a crash.
    std::function<void(const JournalOp&, const char* point)> faultHook;
};

struct ExecProgress {
    int done = 0;
    int total = 0;
    uint64_t bytes = 0;
    std::string current;
};

struct ExecResult {
    int done = 0, failed = 0, skipped = 0;
    uint64_t bytes = 0;
    bool cancelled = false;
};

// Runs (or resumes, or undoes) a journaled batch one operation at a time. Every
// state change is committed to the journal before the next step touches disk,
// so a crash at any instant leaves each file in at least one verified location.
class Executor {
public:
    Executor(FileSystem& fs, Journal& journal, ExecOptions options = {});

    ExecResult run(int64_t batch);
    ExecResult undo(int64_t batch);
    void cancel() { cancel_ = true; }
    ExecProgress progress() const;

    // Resolve operations a crash left "started" and mark their batches interrupted.
    // Returns the ids of the batches that were recovered (resume them with run()).
    static std::vector<int64_t> recover(FileSystem& fs, Journal& journal);

private:
    enum class Outcome { Done, Failed, Skipped };
    Outcome perform(JournalOp& op, std::string& detail, std::string* newDst);
    Outcome performMove(JournalOp& op, const std::string& from, const std::string& to, std::string& detail);
    Outcome reverse(JournalOp& op, std::string& detail);
    void hook(const JournalOp& op, const char* point) const;
    void setCurrent(const std::string& s);

    FileSystem& fs_;
    Journal& journal_;
    ExecOptions opt_;
    std::atomic<bool> cancel_{false};
    std::atomic<int> done_{0}, total_{0};
    std::atomic<uint64_t> bytes_{0};
    mutable std::mutex currentMu_;
    std::string current_;
};

}  // namespace storage
