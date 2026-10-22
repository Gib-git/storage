#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "storage/core/node.hpp"

struct sqlite3;

namespace storage {

struct ScanInfo {
    int64_t id = 0;
    std::string root;
    std::string volume;
    int64_t started = 0;
    int64_t finished = 0;
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t bytes = 0;
    uint64_t errors = 0;
    bool complete = false;
};

// Persistent SQLite index of scans. Only the latest scan of each (volume, root)
// is kept. Not thread-safe: use one Index per thread.
class Index {
public:
    explicit Index(const std::filesystem::path& dbPath);
    ~Index();

    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;

    // Persist a finalized tree, replacing older scans of the same root. Returns the scan id.
    int64_t save(ScanTree& tree);

    std::vector<ScanInfo> scans();
    std::unique_ptr<ScanTree> load(int64_t scanId);

    // Latest scan whose root equals `root` on `volume`, or 0.
    int64_t findScan(const std::string& root, const std::string& volume = "local");

    void remove(int64_t scanId);

private:
    void exec(const char* sql);
    sqlite3* db_ = nullptr;
};

}  // namespace storage
