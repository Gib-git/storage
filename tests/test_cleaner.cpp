#include <algorithm>
#include <set>

#include "TempTree.hpp"
#include "check.hpp"
#include "storage/core/cleaner.hpp"
#include "storage/core/executor.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"

using namespace storage;
namespace fs = std::filesystem;

namespace {
void age(const fs::path& p, int days) {
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(24 * days));
}
CleanTarget tempTarget(const fs::path& dir, const std::string& pattern, Safety s, int minAge = 0) {
    CleanTarget t;
    t.id = "test";
    t.name = "Test cache";
    t.paths = {toUtf8(dir) + "/" + pattern};
    t.safety = s;
    t.minAgeDays = minAge;
    return t;
}
}  // namespace

TEST_CASE("cleaner: catalogue and commands exist for this OS") {
    const auto cat = cleanCatalogue();
    CHECK(!cat.empty());
    std::set<std::string> ids;
    for (const auto& t : cat) {
        CHECK(!t.paths.empty());
        CHECK(ids.insert(t.id).second);  // ids are unique
    }
    CHECK(!cleanCommands().empty());
}

TEST_CASE("cleaner: analyze sizes entries, applies age and wildcard filters") {
    TempTree t;
    t.file("cache/a.bin", 1000);
    t.file("cache/sub/b.bin", 2000);
    t.file("cache/sub/c.bin", 3000);
    const auto old = t.file("cache/old.log", 400);
    age(old, 30);
    age(t.root() / "cache" / "a.bin", 30);
    t.file("thumbs/thumbcache_32.db", 50);
    t.file("thumbs/thumbcache_96.db", 60);
    t.file("thumbs/keep.txt", 70);

    LocalFileSystem lfs;
    auto items = analyzeCleanTargets(lfs, {tempTarget(t.root() / "cache", "*", Safety::Safe)});
    REQUIRE(items.size() == 1);
    CHECK(items[0].bytes == 6400);
    CHECK(items[0].files == 4);
    REQUIRE(items[0].entries.size() == 3);
    CHECK(items[0].entries[0].isDir);  // "sub" (5000 bytes) sorts first
    CHECK(items[0].entries[0].size == 5000);

    // Age filter: the folder counts as recent because its newest content is recent.
    items = analyzeCleanTargets(lfs, {tempTarget(t.root() / "cache", "*", Safety::Safe, 7)});
    CHECK(items[0].bytes == 1400);  // a.bin + old.log

    items = analyzeCleanTargets(lfs, {tempTarget(t.root() / "thumbs", "thumbcache_*.db", Safety::Safe)});
    CHECK(items[0].entries.size() == 2);
    CHECK(items[0].bytes == 110);

    items = analyzeCleanTargets(lfs, {tempTarget(t.root() / "missing", "*", Safety::Safe)});
    CHECK(items[0].entries.empty());
}

TEST_CASE("cleaner: plan deletes only Safe items permanently, and executes") {
    TempTree t, j;
    t.file("safe/x/1.bin", 100);
    t.file("safe/2.bin", 200);
    t.file("review/3.bin", 300);
    LocalFileSystem lfs;
    auto items = analyzeCleanTargets(lfs, {tempTarget(t.root() / "safe", "*", Safety::Safe),
                                           tempTarget(t.root() / "review", "*", Safety::Review)});
    auto ops = planClean(items, true);
    REQUIRE(ops.size() == 3);
    int rmtree = 0, del = 0, trash = 0;
    for (const auto& op : ops) {
        rmtree += op.action == Action::DeleteTree;
        del += op.action == Action::Delete;
        trash += op.action == Action::Trash;
    }
    CHECK(rmtree == 1);
    CHECK(del == 1);
    CHECK(trash == 1);  // Review items always go to the Trash

    // Execute only the permanent deletions (keeps the developer's Trash untouched).
    std::vector<JournalOp> permanent;
    for (const auto& op : ops)
        if (op.action != Action::Trash) permanent.push_back(op);
    Journal journal(j.root() / "journal.db");
    const int64_t batch = journal.createBatch("clean", "test", "local", permanent);
    const ExecResult r = Executor(lfs, journal).run(batch);
    CHECK(r.done == 2);
    CHECK(r.bytes == 300);
    CHECK(fs::exists(t.root() / "safe"));  // the cache folder itself stays
    CHECK(fs::is_empty(t.root() / "safe"));
    CHECK(fs::exists(t.root() / "review" / "3.bin"));
}

TEST_CASE("cleaner: insights find empty folders, project junk and large old files") {
    TempTree t;
    t.dir("empty/a/b");
    t.file("empty/a/.DS_Store", 1);
    t.dir("notempty/x");
    t.file("notempty/keep.txt", 1);
    const auto pkg = t.file("proj/package.json", 10);
    t.file("proj/node_modules/lib/index.js", 5000);
    const auto big = t.file("big.iso", 3000000);
    t.file("recent.bin", 3000000);
    for (const auto& p : {pkg, t.root() / "proj" / "node_modules" / "lib" / "index.js", big}) age(p, 400);
    age(t.root() / "proj" / "node_modules" / "lib", 400);
    age(t.root() / "proj" / "node_modules", 400);
    age(t.root() / "proj", 400);

    LocalFileSystem lfs;
    auto tree = Scanner(lfs, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
    const char sep = nativeSeparator();

    auto empty = findEmptyFolders(*tree->root, sep);
    std::vector<std::string> names;
    for (const auto& f : empty) names.push_back(fs::relative(fromUtf8(f.path), t.root()).generic_string());
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"empty", "notempty/x"});

    auto junk = findProjectJunk(*tree->root, sep, 180);
    REQUIRE(junk.size() == 1);
    CHECK(fs::path(fromUtf8(junk[0].path)).filename() == "node_modules");
    CHECK(junk[0].size == 5000);
    CHECK(findProjectJunk(*tree->root, sep, 1000).empty());  // not stale enough

    auto large = findLargeOldFiles(*tree->root, sep, 1000000, 365);
    REQUIRE(large.size() == 1);
    CHECK(fs::path(fromUtf8(large[0].path)).filename() == "big.iso");
}
