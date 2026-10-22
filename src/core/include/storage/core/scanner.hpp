#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "storage/core/filesystem.hpp"
#include "storage/core/node.hpp"

namespace storage {

struct ScanOptions {
    std::string root;
    unsigned threads = 0;               // 0 = hardware concurrency
    bool stayOnDevice = true;           // like `du -x`: don't descend into other mounts
    std::vector<std::string> excludes;  // absolute paths that are not descended into

    // Sensible defaults for the current platform (pseudo filesystems, firmlinked
    // macOS system volumes that would otherwise be counted twice, ...).
    static ScanOptions defaultsFor(const std::string& root);
};

struct ScanProgress {
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t bytes = 0;
    uint64_t errors = 0;
    std::string current;
};

// Parallel directory walker producing an in-memory ScanTree.
// run() blocks; progress() and cancel() may be called from any other thread.
class Scanner {
public:
    Scanner(FileSystem& fs, ScanOptions options);
    ~Scanner();

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    std::unique_ptr<ScanTree> run();
    void cancel();
    bool cancelled() const;
    ScanProgress progress() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace storage
