#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "storage/core/filesystem.hpp"
#include "storage/core/journal.hpp"
#include "storage/core/node.hpp"

namespace storage {

enum class Safety {
    Safe,     // caches apps rebuild automatically
    Review,   // usually fine, but look first (logs, archives, old downloads)
    Caution,  // can contain things you want (Trash, device backups)
};

const char* safetyName(Safety s);

// One cleanable location from the built-in catalogue.
struct CleanTarget {
    std::string id;
    std::string group;  // "System", "Browsers", "Developer", "Apps"
    std::string name;
    std::string description;
    std::vector<std::string> paths;  // ~ and %ENV% are expanded; a trailing "/*" means "every child"
    Safety safety = Safety::Safe;
    int minAgeDays = 0;           // only entries untouched for this long
    bool defaultOn = false;
    bool permanentOnly = false;   // e.g. the Trash itself: trashing it again would free nothing
    std::string hint;             // e.g. "Quit Xcode first"
};

// A shell command the user can run themselves (needs admin rights or another tool).
struct CleanCommand {
    std::string name;
    std::string command;
    std::string description;
};

std::vector<CleanTarget> cleanCatalogue();   // for the current OS
std::vector<CleanCommand> cleanCommands();   // for the current OS

struct CleanEntry {
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
    bool isDir = false;
};

struct CleanItem {
    CleanTarget target;
    std::vector<CleanEntry> entries;  // what would be removed (top-level children of each path)
    uint64_t bytes = 0;
    uint64_t files = 0;
    uint64_t errors = 0;  // unreadable folders while sizing
};

// Measure what each target would free. Missing paths produce empty items.
std::vector<CleanItem> analyzeCleanTargets(FileSystem& fs, const std::vector<CleanTarget>& targets,
                                           const std::atomic<bool>* cancel = nullptr);

// Turn analyzed items into journal operations. `permanent` deletes Safe items instead of
// trashing them (only Safe items, or permanentOnly targets, are ever deleted outright).
std::vector<JournalOp> planClean(const std::vector<CleanItem>& items, bool permanent);

// --- Insights over a scanned folder ---------------------------------------------------

struct Finding {
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
    bool isDir = false;
    std::string reason;
};

// Files at least `minSize` bytes and not modified for `olderThanDays` days, largest first.
std::vector<Finding> findLargeOldFiles(const Node& root, char sep, uint64_t minSize, int olderThanDays, size_t limit = 500);
// Folders containing nothing (or only OS junk files), deepest first.
std::vector<Finding> findEmptyFolders(const Node& root, char sep);
// Rebuildable project folders (node_modules, target, build, .venv, __pycache__, ...) whose
// project has not been touched for `olderThanDays` days, largest first.
std::vector<Finding> findProjectJunk(const Node& root, char sep, int olderThanDays);

std::string expandUserPath(const std::string& path);

}  // namespace storage
