#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace storage {

// One file operation inside a batch. Recorded *before* anything touches disk.
struct JournalOp {
    int64_t id = 0;
    int64_t batch = 0;
    int seq = 0;
    std::string action;  // see Action below
    std::string src;     // file acted on
    std::string dst;     // move target, or where a trashed file went (for undo)
    std::string ref;     // file kept as the reference copy (dedupe link/clone/delete)
    uint64_t size = 0;   // expected size of src when the plan was made
    int64_t mtime = 0;   // expected mtime of src when the plan was made
    std::string hash;    // content hash when known
    std::string state = "pending";
    std::string detail;  // human-readable result / reason
};

namespace Action {
inline constexpr const char* Trash = "trash";
inline constexpr const char* Delete = "delete";
inline constexpr const char* Hardlink = "hardlink";
inline constexpr const char* Clone = "clone";
inline constexpr const char* Move = "move";
inline constexpr const char* RemoveDir = "rmdir";  // only if empty apart from OS junk files
inline constexpr const char* DeleteTree = "rmtree";  // permanently delete a folder and everything in it
inline constexpr const char* MakeDir = "mkdir";    // skipped if it already exists, so undo never removes it
}  // namespace Action

namespace OpState {
inline constexpr const char* Pending = "pending";
inline constexpr const char* Started = "started";  // in flight; resolved by recovery after a crash
inline constexpr const char* Done = "done";
inline constexpr const char* Failed = "failed";
inline constexpr const char* Skipped = "skipped";
inline constexpr const char* Undone = "undone";
}  // namespace OpState

namespace BatchState {
inline constexpr const char* Planned = "planned";
inline constexpr const char* Running = "running";
inline constexpr const char* Interrupted = "interrupted";  // crashed or cancelled; can resume
inline constexpr const char* Done = "done";
inline constexpr const char* Undone = "undone";
}  // namespace BatchState

struct JournalBatch {
    int64_t id = 0;
    int64_t created = 0;
    int64_t finished = 0;
    std::string kind;    // "dedupe", "organize", "clean"
    std::string title;
    std::string volume;
    std::string state;
    int total = 0, done = 0, failed = 0, skipped = 0, undone = 0;
    uint64_t bytes = 0;  // bytes affected by completed ops
};

// Durable write-ahead record of every file operation (SQLite, synchronous=FULL).
// Not thread-safe: use one Journal per thread.
class Journal {
public:
    explicit Journal(const std::filesystem::path& dbPath);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    static std::filesystem::path defaultPath();

    int64_t createBatch(const std::string& kind, const std::string& title, const std::string& volume,
                        const std::vector<JournalOp>& ops);
    std::vector<JournalOp> ops(int64_t batch);
    void setOp(int64_t opId, const std::string& state, const std::string& detail = {}, const std::string* dst = nullptr);
    void setBatchState(int64_t batch, const std::string& state);

    JournalBatch batch(int64_t id);
    std::vector<JournalBatch> batches(int limit = 100);
    // Batches left running by a crash or kill, on `volume` (all volumes when empty).
    std::vector<int64_t> runningBatches(const std::string& volume = {});
    // Volumes that have batches left running.
    std::vector<std::string> volumesWithRunningBatches();

private:
    void exec(const char* sql);
    sqlite3* db_ = nullptr;
};

}  // namespace storage
