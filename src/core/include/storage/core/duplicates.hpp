#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "storage/core/filesystem.hpp"
#include "storage/core/node.hpp"

namespace storage {

class Index;

struct DupFile {
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t dev = 0;
    uint64_t ino = 0;
};

struct DupGroup {
    uint64_t size = 0;    // size of each copy
    std::string hash;     // full SHA-256
    std::vector<DupFile> files;
    uint64_t reclaimable() const { return files.size() > 1 ? size * (files.size() - 1) : 0; }
};

struct DupOptions {
    uint64_t minSize = 1;      // ignore files smaller than this (empty files are never duplicates)
    unsigned threads = 0;      // 0 = min(4, CPU count) — good for SSDs; use 1-2 for spinning disks
    bool verifyBytes = false;  // "paranoid" byte-by-byte confirmation after hashing
    bool skipHidden = false;   // skip dot-files and anything under dot-folders
    std::vector<std::string> excludeNames;  // folder names never descended into (e.g. ".git")
};

enum class DupStage { Collecting, PartialHash, FullHash, Verifying, Done };

struct DupProgress {
    DupStage stage = DupStage::Collecting;
    uint64_t filesDone = 0, filesTotal = 0;
    uint64_t bytesDone = 0, bytesTotal = 0;
    uint64_t errors = 0;
};

// Finds files with identical content: size → partial hash → full hash →
// optional byte compare. Hard links to the same inode count as one file.
class DuplicateFinder {
public:
    // `cache` (optional) supplies and receives hashes keyed by path+size+mtime.
    DuplicateFinder(FileSystem& fs, DupOptions options, Index* cache = nullptr);
    ~DuplicateFinder();

    // Groups sorted by reclaimable bytes, largest first.
    std::vector<DupGroup> run(const Node& root);
    void cancel();
    bool cancelled() const;
    DupProgress progress() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

const char* stageName(DupStage s);

// --- Choosing which copy to keep ----------------------------------------------------

enum class KeepRule { Newest, Oldest, ShortestPath, LongestPath, Alphabetical, InFolder };

const char* keepRuleName(KeepRule r);
// Accepts "newest", "oldest", "shortest", "longest", "alpha", "folder"; throws on anything else.
KeepRule parseKeepRule(const std::string& s);

// Index of the file to keep. InFolder keeps the first file under any of `preferred`
// (falling back to ShortestPath when none is).
size_t chooseKeeper(const DupGroup& g, KeepRule rule, const std::vector<std::string>& preferred = {});

struct JournalOp;
// Turn a selection into journal operations. `remove[g][i]` marks file i of group g.
// Throws std::invalid_argument if any group would lose every copy.
std::vector<JournalOp> planDedupe(const std::vector<DupGroup>& groups, const std::vector<std::vector<bool>>& remove,
                                  const std::string& action);

}  // namespace storage
