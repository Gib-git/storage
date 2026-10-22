#pragma once

#include <cstdint>
#include <regex>
#include <unordered_map>
#include <string>
#include <vector>

#include "storage/core/category.hpp"
#include "storage/core/exif.hpp"
#include "storage/core/filesystem.hpp"
#include "storage/core/journal.hpp"
#include "storage/core/json.hpp"
#include "storage/core/node.hpp"

namespace storage {

// Which files a rule applies to. Empty / zero fields match everything.
struct RuleMatch {
    std::vector<Category> categories;
    std::vector<std::string> extensions;  // lowercase, without the dot
    std::string nameContains;             // case-insensitive
    std::string nameRegex;                // ECMAScript, case-insensitive
    uint64_t minSize = 0, maxSize = 0;
    int olderThanDays = 0, newerThanDays = 0;  // by modified time
};

// First matching rule wins. `folder` and `rename` are templates:
//   {type} {ext} {EXT} {name} {filename} {year} {month} {day} {monthname} {size} {first} {parent}
// `folder` uses '/' between levels. An empty `rename` keeps the file name.
struct OrganizeRule {
    std::string name;
    RuleMatch match;
    std::string folder;
    std::string rename;
    bool leave = false;  // matching files stay where they are
};

enum class DateSource { Modified, CaptureThenModified };
enum class ConflictPolicy { Rename, Skip };

struct RuleSet {
    std::string name;
    std::string description;
    std::vector<OrganizeRule> rules;
    std::string fallbackFolder;  // for files no rule matches; empty = leave them alone
    DateSource dateSource = DateSource::CaptureThenModified;
    ConflictPolicy conflict = ConflictPolicy::Rename;
    bool includeSubfolders = true;
    bool removeEmptyFolders = true;
    bool includeHidden = false;

    json::Value toJson() const;
    static RuleSet fromJson(const json::Value& v);  // throws std::invalid_argument
};

std::vector<RuleSet> builtinPresets();

struct PlannedMove {
    std::string src, dst;
    std::string rule;
    uint64_t size = 0;
    int64_t mtime = 0;
    bool renamed = false;  // got a " (2)" suffix to avoid a clash
};

struct OrganizePlan {
    std::string scope;
    std::string destination;
    std::vector<std::string> createDirs;  // parents first; folders that do not exist yet
    std::vector<PlannedMove> moves;
    std::vector<std::string> removeDirs;  // deepest first; only removed if empty at run time
    size_t alreadyInPlace = 0;
    size_t unmatched = 0;
    size_t conflictsSkipped = 0;
    size_t protectedSkipped = 0;  // inside app bundles, libraries, .git, ...
    uint64_t bytes = 0;

    std::vector<JournalOp> toOps() const;
};

class Organizer {
public:
    Organizer(FileSystem& fs, RuleSet rules);

    // Plan moves for files under `scope` (a node of a finalized scan). Files land under
    // `destination` (empty = the scope folder itself).
    OrganizePlan plan(const Node& scope, const std::string& destination = {});

    // Expand a template for a file; '/' separates folder levels. Exposed for tests and previews.
    std::string expand(const std::string& tmpl, const Node& file, const std::string& scopePath);

private:
    const OrganizeRule* ruleFor(const Node& file) const;
    bool usesDates() const;
    void prefetchCaptureDates(const std::vector<const Node*>& files);
    std::string dateFor(const Node& file, const std::string& path, char token);

    FileSystem& fs_;
    RuleSet rules_;
    std::vector<std::regex> regexes_;  // compiled once, parallel to rules_.rules
    int64_t now_ = 0;
    std::unordered_map<const Node*, CaptureDate> captureDates_;  // filled in parallel before planning
    // Cache of dates for the file currently being expanded.
    std::string datePath_;
    int dateY_ = 0, dateM_ = 0, dateD_ = 0;
};

// Make one path segment safe on every OS (no separators, reserved names, trailing dots...).
std::string sanitizeSegment(const std::string& s);

const char* junkFileNames();  // for display/docs: ".DS_Store, Thumbs.db, desktop.ini"
bool isJunkFile(const std::string& name);
bool isPackageName(const std::string& name);  // .app, .photoslibrary, .git, node_modules, ...

}  // namespace storage
